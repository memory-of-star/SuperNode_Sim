#!/usr/bin/env python3
"""Validated SUE v2 cases, reproducible runs and topology/model matrix."""
import os,argparse,csv,json,hashlib,shutil,subprocess,time,uuid,sys
from datetime import datetime,timezone
from pathlib import Path
from collections import Counter
from concurrent.futures import ThreadPoolExecutor,as_completed
from dataclasses import asdict
from topologies import TOPOLOGIES,BASE_TOPOLOGY_NAMES,ports,validate,plan,proxy,ring_distance,flow_hash
from workloads import build,task,model_config
HERE=Path(__file__).resolve().parent; ROOT=HERE.parents[1]
SIM=ROOT/'ns-3-alibabacloud/simulation'; SOURCE=SIM/'scratch/sue-harness-v3.cc'
BINARY=SIM/'build/scratch/ns3.36.1-sue-harness-v3-default'
DEFAULT=dict(seed=1,payload=2048,interval_ns=1,visibility_ns=1,stop_ns=1000000,
             queue_bytes=65536,ring_bytes=65536,ring_gbps=400,ring_delay_ns=20,
             bad_every=0,timeout_s=180,credits=512)

def dump(p,v): p.write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n')
from trace_storage import digest as sha, open_trace
def csvwrite(p,rows):
    rows=list(rows)
    if not rows:return
    with p.open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
def csvread(p):
    with open_trace(p) as f:return list(csv.DictReader(f))
def stamp():return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+uuid.uuid4().hex[:6]

def check_tasks(t,tasks,settings):
    assert tasks,'empty tasks'
    assert set(DEFAULT)<=set(settings)<=set(DEFAULT)|{'xpu_port_bps','mpu_port_bps','port_sample_ns','xpu_port_delay_ns','mpu_port_delay_ns','time_resolution_ns','io_event_driven'},'unknown/missing settings'
    assert type(settings.get('io_event_driven',1)) is int and settings.get('io_event_driven',1) in (0,1),'invalid IO scheduler'
    assert type(settings.get('time_resolution_ns',0)) is int and settings.get('time_resolution_ns',0) in (0,1),'invalid time resolution'
    for k in ('xpu_port_delay_ns','mpu_port_delay_ns'):
        if k in settings:assert type(settings[k]) is int and 0<=settings[k]<=1000000,'invalid port delay'
    sample=settings.get('port_sample_ns',0)
    assert type(sample) is int and (sample==0 or sample>=100),'port_sample_ns must be 0 or >=100ns'
    assert not sample or settings['stop_ns']//sample<=1000000,'too many telemetry samples'
    for k in ('xpu_port_bps','mpu_port_bps'):
        assert type(settings.get(k,200000000000)) is int and 0<settings.get(k,200000000000)<=10**15,(k,'invalid bit/s')
    for k in DEFAULT: assert type(settings[k]) is int and settings[k]>=0,(k,'nonnegative integer required')
    assert 1<=settings['seed']<=2147483646 and 64<=settings['payload']<=8192
    assert settings['interval_ns']>0 and settings['visibility_ns']>=1
    assert settings['ring_gbps']>0 and settings['ring_bytes']>=max(settings['payload'],64)
    assert settings['queue_bytes']>=settings['payload']+32 and settings['timeout_s']>0 and settings['stop_ns']>0
    assert settings['credits']*256>=settings['payload']+32
    for i,row in enumerate(tasks):
        assert row['id']==i and type(row['bytes']) is int and 0<=row['bytes']<=1000000000
        assert 0<=row['delay_ns']<settings['stop_ns'] and row['group'] in (-1,0,1) and 0<=row['vc']<8
        for prefix in ('src','dst'):
            c,d=row[prefix+'_chip'],row[prefix+'_die'];assert 0<=c<t.xpu+t.mpu and 0<=d<(4 if c<t.xpu else 1)
        equal=(row['src_chip'],row['src_die'])==(row['dst_chip'],row['dst_die'])
        assert equal==(row['bytes']==0),'wait must be local and network bytes must leave SA'
        assert len(set(row['deps']))==len(row['deps']) and all(type(d) is int and 0<=d<i for d in row['deps']),'DAG must be topologically ordered'
        rd=row.get('receive_deps',[])
        assert isinstance(rd,list) and len(rd)==len(set(rd)) and all(type(d) is int and d in row['deps'] for d in rd),'invalid receive dependencies'
        for d in rd:
            parent=tasks[d]
            assert parent['bytes']>0 and (parent['dst_chip'],parent['dst_die'])==(row['src_chip'],row['src_die']),'receive dependency must start at receiver'


def receive_text(tasks):
    return ''.join(f"{t['id']} {d}\n" for t in tasks for d in t.get('receive_deps',[]))

def check_resources(t,tasks,resources,stop_ns):
    seen=set()
    for r in resources:
        assert set(r)=={'id','kind','service_ns','units','peer_die'}
        assert all(type(v) is int for v in r.values())
        i=r['id'];assert 0<=i<len(tasks) and i not in seen;seen.add(i)
        q=tasks[i];k=r['kind'];assert q['bytes']==0 and k in (0,1,2,3,4,5)
        assert (q['src_chip']>=t.xpu)==(k==2)
        assert r['units']>0
        if k==3:assert r['service_ns']==0 and 0<=r['peer_die']<4 and r['peer_die']!=q['src_die']
        elif k==5:assert r['service_ns']==0 and r['peer_die']==-1
        else:assert 0<r['service_ns']<stop_ns and r['peer_die']==-1

def resource_text(resources):
    return ''.join(' '.join(str(r[k]) for k in ('id','kind','service_ns','units','peer_die'))+'\n' for r in resources)

def create(case,topology,tasks,workload,settings=None,routes=None,resources=None):
    t=TOPOLOGIES[topology]; settings={**DEFAULT,'time_resolution_ns':0,**(settings or {})}
    tasks=[dict(r,id=i) for i,r in enumerate(tasks)];check_tasks(t,tasks,settings)
    rows=ports(t); structural=validate(t,rows)
    case=Path(case);case.mkdir(parents=True,exist_ok=False)
    dump(case/'experiment.json',dict(version=3,topology=asdict(t),settings=settings,workload=workload))
    dump(case/'tasks.json',tasks)
    traffic=[{**{k:v for k,v in r.items() if k!='receive_deps'},'deps':' '.join(map(str,r['deps']))} for r in tasks]
    if receive_text(tasks):
        for row,taskrow in zip(traffic,tasks):row['receive_deps']=' '.join(map(str,taskrow.get('receive_deps',[])))
    csvwrite(case/'traffic.csv',traffic)
    if receive_text(tasks):(case/'receive-deps.tsv').write_text(receive_text(tasks))
    if resources is not None:
        check_resources(t,tasks,resources,settings['stop_ns'])
        dump(case/'resources.json',resources);(case/'resources.tsv').write_text(resource_text(resources))
        hw=workload.get('hardware',{})
        if 'mpu_max_inflight' in hw and any(r['kind']==2 for r in resources):
            import mpu_config
            mpu_config.write(case,hw,resources)
    if os.environ.get('SUE_IO_DIE_PROFILE'):
        import io_die_config
        io_die_config.write(case,os.environ['SUE_IO_DIE_PROFILE'],tasks,workload.get('hardware'),t.xpu)
    if routes:csvwrite(case/'token_routes.csv',routes)
    idx={(r['chip'],r['die'],r['port']):r for r in rows}
    plans=[dict(id=r['id'],**plan(t,rows,r,settings['seed'],idx)) for r in tasks]
    if (case/'io-die.json').exists():
        import io_die_config
        plans=io_die_config.paths(t,rows,tasks,settings['seed'],idx,json.loads((case/'io-die.json').read_text())['tasks'])
    csvwrite(case/'path_plan.csv',plans)
    # Verbatim faceplate identifiers and explicit dense simulation port IDs.
    csvwrite(case/'ports.csv',rows)
    (case/'ports.tsv').write_text(''.join(' '.join(str(r[k]) for k in ('chip','die','port','switch','vrf','physical'))+'\n' for r in rows))
    (case/'tasks.tsv').write_text(''.join(' '.join(map(str,[r['id'],r['src_chip'],r['src_die'],r['dst_chip'],r['dst_die'],r['bytes'],r['delay_ns'],r['vc'],r['group'],','.join(map(str,r['deps'])) or '-']))+'\n' for r in tasks))
    cfg={}
    for line in (SIM/'conf/sue-400g.conf').read_text().splitlines():
        s=line.split('#',1)[0].strip()
        if s:
            parts=s.split(None,1);cfg[parts[0]]=parts[1] if len(parts)>1 else ''
    cfg.update(ENABLE_SCALEUP='1',SUE_MTP_THREADS='1',SUE_LINK_PROPAGATION_NS=('0' if all(k in settings for k in ('xpu_port_delay_ns','mpu_port_delay_ns')) else '20'),SUE_SWITCH_FWD_BUDGET_NS='300',
               SUE_ETH_TYPE_1_HAS_SHIM='1',SUE_ETH_TYPE_0_HAS_SHIM='0',DSCP_TO_VRF_MAP='',VLAN_TO_VRF_MAP='',
               PORT_TO_VRF_MAP=','.join(f'{k}:{v}' for k,v in sorted(structural['port_vrf'].items())),
               DSCP_TO_VC_MAP=','.join(f'{k}:{k%8}' for k in range(64)),
               FC_MODE='CBFC',CBFC_VC_CREDIT_LIMIT=','.join(f'{k}:{settings["credits"]}' for k in range(8)),
               LLR_REPLAY_TIMER_MAX_NS='0',LLR_INIT_TIMER_MAX_NS='0',LLR_DATA_AGE_TIMER_MAX_NS='1000000000',
               LLR_REPLAY_CT_MAX='255',LLR_CTLOS_TARGET_SPACING='16384',CBFC_EGRESS_BUF_BYTES='0')
    (case/'sue.conf').write_text(''.join(f'{k} {v}\n' for k,v in cfg.items()))
    dump(case/'validation.json',dict(structural=structural,tasks=len(tasks),logical_bytes=sum(r['bytes'] for r in tasks),
                                   predicted_unreachable=sum(not p['reachable'] for p in plans)))
    (case/'experiment-spec.md').write_text('# SUE v2 实验定义\n\n'+
        f"协议：SUE AFH + LLR + CBFC；XPU端口{settings.get('xpu_port_bps',200000000000)/1e9:g} Gbit/s，MPU端口{settings.get('mpu_port_bps',200000000000)/1e9:g} Gbit/s；无 ISL、无 VLAN。\n\n"+
        '预测（运行前）：合法路径完整交付，响应回原口；R1/R2保持；奇偶分面跨奇偶增加1次目的侧Die转交，pure-rail对角增加2次。拥塞可能改变时延排序。内存池跨侧或强制错误组应记录不可达。\n\n'+
        'UCIe为有界共享缓冲、逐方向带宽及逐跳延迟的包级抽象，参数未用硬件profile校准。固定计算等待不模拟GPU排队。\n')
    return case

def validate_case(case):
    spec=json.loads((case/'experiment.json').read_text());assert spec['version']==3
    t=TOPOLOGIES[spec['topology']['name']];assert spec['topology']==asdict(t)
    tasks=json.loads((case/'tasks.json').read_text());check_tasks(t,tasks,spec['settings'])
    rows=ports(t);v=validate(t,rows)
    expected=''.join(' '.join(str(r[k]) for k in ('chip','die','port','switch','vrf','physical'))+'\n' for r in rows)
    assert (case/'ports.tsv').read_text()==expected,'stale physical mapping'
    expected=''.join(' '.join(map(str,[r['id'],r['src_chip'],r['src_die'],r['dst_chip'],r['dst_die'],r['bytes'],r['delay_ns'],r['vc'],r['group'],','.join(map(str,r['deps'])) or '-']))+'\n' for r in tasks)
    assert (case/'tasks.tsv').read_text()==expected,'stale compiled tasks'
    assert ((case/'receive-deps.tsv').read_text() if (case/'receive-deps.tsv').exists() else '')==receive_text(tasks),'stale receive dependencies'
    if (case/'resources.json').exists():
        resources=json.loads((case/'resources.json').read_text());check_resources(t,tasks,resources,spec['settings']['stop_ns'])
        assert (case/'resources.tsv').read_text()==resource_text(resources),'stale compiled resources'
    else:assert not (case/'resources.tsv').exists(),'resource sidecar lacks JSON source'
    if (case/'io-die.json').exists():
        import io_die_config
        io_die_config.validate(case,tasks,spec['settings'])
    else:assert not (case/'io-die.tsv').exists(),'orphan IO mapping'
    if (case/'mpu.json').exists():
        import mpu_config
        mpu_config.validate(case,resources)
    else:assert not (case/'mpu.tsv').exists(),'MPU sidecar lacks JSON source'
    detailed=[r for r in resources if r['kind']==5] if (case/'resources.json').exists() else []
    if detailed or (case/'pcie.json').exists():
        import pcie_config
        profile=json.loads((case/'pcie.json').read_text());requests=json.loads((case/'pcie-requests.json').read_text())
        pcie_config.check_requests(profile,requests,tasks,resources,t.xpu)
        assert (case/'pcie.tsv').read_text()==pcie_config.text(profile),'stale PCIe config'
        assert (case/'pcie-requests.tsv').read_text()==pcie_config.request_text(requests),'stale PCIe requests'
    else:assert not (case/'pcie.tsv').exists() and not (case/'pcie-requests.tsv').exists(),'PCIe sidecar lacks JSON'
    if (case/'hbm.json').exists():
        import hbm_config
        h=hbm_config.load(case/'hbm.json');hr=json.loads((case/'hbm-requests.json').read_text())
        hbm_config.check(h,hr,resources)
        assert (case/'hbm.tsv').read_text()==hbm_config.text(h)
        assert (case/'hbm-requests.tsv').read_text()==hbm_config.request_text(hr)
        if (case/'pcie.json').exists():assert h['bandwidth_bytes_s_per_die']==profile['receiver']['hbm_bytes_s_per_die']
    else:assert not (case/'hbm.tsv').exists() and not (case/'hbm-requests.tsv').exists()
    return spec,tasks

def analyze(run):
    spec=json.loads((run/'experiment.json').read_text());tasks=json.loads((run/'tasks.json').read_text());execution=json.loads((run/'execution.json').read_text())
    out=dict(topology=spec['topology']['name'],model=spec['workload'].get('model','probe'),window=spec['workload'].get('window','probe'),
             status='failed',wall_s=execution['wall_s'],exit_code=execution['exit_code'],tasks=len(tasks),logical_bytes=sum(t['bytes'] for t in tasks),
             run=str(run),failures=[],xpu=spec['topology']['xpu'],mpu=spec['topology']['mpu'],switches=spec['topology']['switches'])
    if not (run/'task_statistics.csv').exists():
        out['failures']=['task statistics missing'];dump(run/'summary.json',out);return out
    rows=csvread(run/'task_statistics.csv');stats={int(r['id']):r for r in rows}; plans=csvread(run/'path_plan.csv')
    t=TOPOLOGIES[out['topology']];physical=ports(t);idx={(r['chip'],r['die'],r['port']):r for r in physical}
    from io_die_config import managed_edges
    streaming_edges=managed_edges(run)
    io_plans=None
    if (run/'io-die.json').exists():
        import io_die_config
        io_plans=io_die_config.paths(t,physical,tasks,spec['settings']['seed'],idx,json.loads((run/'io-die.json').read_text())['tasks'])
    completed=0;hops=Counter();lat=[];bytehops=Counter();starts=[];ends=[]
    for taskrow in tasks:
        i=taskrow['id'];r=stats.get(i)
        if r is None:out['failures'].append(f'missing task {i}');continue
        ok=r['status']=='complete' and int(r['received_bytes'])==taskrow['bytes'] and int(r['invalid_frames'])==0
        p=io_plans[i] if io_plans is not None else plan(t,physical,taskrow,spec['settings']['seed'],idx)
        if taskrow['bytes']:
            for k,outkey in [('group','group'),('port','port'),('proxy_die','proxy_die'),('hops','ucie_hops')]:
                if int(r[outkey])!=p[k]:ok=False
            h=int(r['ucie_hops']);hops[h]+=1;bytehops[h]+=taskrow['bytes']
        for d in taskrow['deps']:
            if (i,d) in streaming_edges:continue
            event='data_end_ps' if d in taskrow.get('receive_deps',[]) else 'end_ps'
            if int(stats[d][event])<0 or int(r['start_ps'])<int(stats[d][event])+(spec['settings']['visibility_ns']+taskrow['delay_ns'])*1000:ok=False
        if not ok:out['failures'].append(f'task {i}: {r["status"]}')
        else:
            completed+=1;starts.append(int(r['start_ps']));ends.append(int(r['end_ps']))
            if taskrow['bytes']:lat.append((int(r['end_ps'])-int(r['start_ps']))/1e6)
    ps=csvread(run/'port_statistics.csv');rs=csvread(run/'ring_statistics.csv');engine=json.loads((run/'engine.json').read_text())
    out.update(time_resolution_ns=spec['settings'].get('time_resolution_ns',0),completed=completed,received_bytes=sum(int(r['received_bytes']) for r in rows),
        makespan_us=(max(ends)-min(starts))/1e6 if ends and completed==len(tasks) else None,
        sim_end_us=engine['sim_end_ps']/1e6,latency_p50_us=sorted(lat)[len(lat)//2] if lat else None,
        latency_p99_us=sorted(lat)[min(len(lat)-1,int(len(lat)*.99))] if lat else None,
        task_path_counts=dict(hops),task_path_bytes=dict(bytehops),replay_frames=sum(int(p['replay_frames']) for p in ps),
        discard=sum(int(p['discard']) for p in ps),credit_exhausted_checks=sum(int(p['credit_exhausted']) for p in ps),
        queue_blocked_polls=sum(int(r['queue_blocked_polls']) for r in rows),
        ucie_byte_hops=sum(int(r['bytes']) for r in rs),ucie_queue_ps=sum(int(r['queue_ps']) for r in rs),
        ring_peak_bytes=max((int(r['pool_peak_bytes']) for r in rs),default=0),
        wire_bytes=sum(int(p['wire_bytes']) for p in ps),path_errors=engine['path_errors'],
        active_xpu=len({int(r[k]) for r in rows if int(r['bytes'])>0 for k in ('src_chip','dst_chip') if int(r[k])<t.xpu}))
    if out['discard']:out['failures'].append('LLR discard')
    if len(rows)!=len(tasks):out['failures'].append('statistics row count mismatch')
    if engine['path_errors'] or engine['unknown_frames']:out['failures'].append('invalid receive path')
    if (run/'resources.json').exists():
        resource_rows=csvread(run/'resource_statistics.csv') if (run/'resource_statistics.csv').exists() else []
        indexed={int(r['id']):r for r in resource_rows};pools={};queued=0
        specs=json.loads((run/'resources.json').read_text())
        for q in specs:
            r=indexed.get(q['id']);good=r is not None
            if good:
                start,end=int(r['service_start_ps']),int(r['end_ps']);kind=q['kind'];queued+=max(0,int(r['queued_ps']))
                good=start>=int(stats[q['id']]['start_ps']) and end>=start and int(r['kind'])==kind and int(r['units'])==q['units']
                if kind in (3,5):good=good and int(r['local_received_bytes'])==q['units']
                elif kind==2 and (run/'mpu.json').exists():pass # latency overlaps, bandwidth FIFO is audited separately
                elif kind==1 and (run/'hbm.json').exists():pass # variable service time under fair sharing
                else:
                    good=good and end-start==q['service_ns']*1000
                    pools.setdefault((r['chip'],'0' if kind==4 else r['die'],r['kind']),[]).append((start,end))
            if not good:out['failures'].append(f'resource task {q["id"]}: incomplete/invalid')
        for intervals in pools.values():
            intervals.sort()
            if any(b[0]<a[1] for a,b in zip(intervals,intervals[1:])):out['failures'].append('resource server overlap')
        if len(resource_rows)!=len(specs):out['failures'].append('resource row count mismatch')
        out.update(resource_tasks=len(specs),resource_queue_us=queued/1e6,local_d2d_bytes=sum(q['units'] for q in specs if q['kind']==3))
    if (run/'io-die.json').exists():
        from io_die_config import analyze as analyze_io
        try:out['io_die']=analyze_io(run)
        except (AssertionError,KeyError,FileNotFoundError,ValueError) as exc:out['failures'].append('IO validation: '+str(exc))
    if (run/'mpu.json').exists():
        from mpu_config import analyze as analyze_mpu
        try:out['mpu']=analyze_mpu(run)
        except (AssertionError,KeyError,FileNotFoundError,ValueError) as exc:out['failures'].append('MPU validation: '+str(exc))
    if (run/'pcie.json').exists():
        from pcie_config import analyze as analyze_pcie
        try:out['pcie']=analyze_pcie(run)
        except (AssertionError,KeyError,FileNotFoundError,ValueError) as exc:out['failures'].append('PCIe validation: '+str(exc))
    if (run/'hbm.json').exists():
        from hbm_config import analyze as analyze_hbm
        try:out['hbm']=analyze_hbm(run)
        except (AssertionError,KeyError,FileNotFoundError,ValueError) as exc:out['failures'].append('HBM validation: '+str(exc))
    if execution['exit_code']==0 and not out['failures'] and engine['ok']:out['status']='passed'
    dump(run/'summary.json',out);return out

def run_case(case):
    case=Path(case).resolve();spec,tasks=validate_case(case)
    run=case/'runs'/stamp();run.mkdir(parents=True)
    for p in case.iterdir():
        if p.is_file():shutil.copy2(p,run/p.name)
    s=spec['settings'];t=spec['topology']
    args=dict(case=str(run),out=str(run),xpu=t['xpu'],mpu=t['mpu'],switches=t['switches'],pure=int(t['family']=='pure'),
        seed=s['seed'],payload=s['payload'],intervalNs=s['interval_ns'],visibilityNs=s['visibility_ns'],stopNs=s['stop_ns'],
        queueBytes=s['queue_bytes'],ringBytes=s['ring_bytes'],ringGbps=s['ring_gbps'],ringDelayNs=s['ring_delay_ns'],badEvery=s['bad_every'],xpuPortBps=s.get('xpu_port_bps',200000000000),mpuPortBps=s.get('mpu_port_bps',200000000000))
    args['timeResolutionNs']=s.get('time_resolution_ns',0)
    args['portSampleNs']=s.get('port_sample_ns',0)
    if 'io_event_driven' in s:args['ioEventDriven']=s['io_event_driven']
    for key,arg in (('xpu_port_delay_ns','xpuPortDelayNs'),('mpu_port_delay_ns','mpuPortDelayNs')):
        if key in s:args[arg]=s[key]
    command=[str(BINARY)]+[f'--{k}={v}' for k,v in args.items()]
    dump(run/'command-manifest.json',dict(command=command,cwd=str(run),inputs={p.name:sha(p) for p in run.iterdir() if p.is_file()},
         binary_sha256=sha(BINARY),adapter_sha256=sha(SOURCE),adapter_headers_sha256={p.name:sha(p) for p in SOURCE.parent.glob('sue-*.h')},python_sha256={p.name:sha(p) for p in HERE.glob('*.py')},
         libraries={p.name:sha(p) for pattern in ('*sue-default.so','*network-default.so','*core-default.so') for p in (SIM/'build/lib').glob(pattern)}))
    start=time.monotonic()
    with (run/'stdout.log').open('w') as log:
        try:
            rc=subprocess.run(command,cwd=run,stdout=log,stderr=subprocess.STDOUT,timeout=s['timeout_s']).returncode
        except subprocess.TimeoutExpired:rc=124
        except OSError as e:log.write(str(e));rc=127
    dump(run/'execution.json',dict(exit_code=rc,wall_s=time.monotonic()-start,timeout_s=s['timeout_s']))
    summary=analyze(run);dump(case/'latest.json',dict(run=str(run),status=summary['status']))
    with (case/'run-ledger.jsonl').open('a') as f:f.write(json.dumps(summary,ensure_ascii=False)+'\n')
    return summary

def matrix(dest,workers=2):
    dest=Path(dest).resolve();dest.mkdir(parents=True,exist_ok=False)
    (dest/'plan.md').write_text('# v2 拓扑实验矩阵\n\n16拓扑 × 2模型 × 3窗口 = 96个逐包SUE实验。\n\n固定全网token数：prefill128；decode64×4步；train64个token、前反向。固定种子1，BF16通信；不同规模重排专家和token位置，属于配置对比，不是等价GPU部署或饱和带宽测试。MPU只在独立功能验证中承载通信，模型流量没有虚构内存offload。\n\n预测：所有合法MoE任务应完整交付；奇偶分面出现1跳UCIe，pure-rail出现1/2跳；同一模型的消息内容与专家选择由相同种子控制，但本地消除和物理映射随拓扑变化。计算等待可能掩盖网络差别，不预设拓扑排名。\n')
    cases=[]
    for name,t in TOPOLOGIES.items():
        if name not in BASE_TOPOLOGY_NAMES:continue
        for model in ('kimi-k3','glm-5.3'):
            for window in ('prefill','decode','train'):
                tasks,routes,metadata=build(t,model,window)
                cases.append(create(dest/'cases'/f'{name}--{model}--{window}',name,tasks,metadata,routes=routes))
    dump(dest/'matrix.json',[str(c) for c in cases]);results=[]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures={pool.submit(run_case,c):c for c in cases}
        for f in as_completed(futures):
            try:r=f.result()
            except Exception as e:r=dict(status='runner_error',case=str(futures[f]),error=str(e))
            results.append(r);dump(dest/'results.json',results)
            print(json.dumps({k:r.get(k) for k in ('topology','model','window','status','completed','tasks','makespan_us','wall_s','error')},ensure_ascii=False),flush=True)
    return results

if __name__=='__main__':
    p=argparse.ArgumentParser();sub=p.add_subparsers(dest='cmd',required=True)
    sub.add_parser('build')
    q=sub.add_parser('init');q.add_argument('case');q.add_argument('--topology',choices=TOPOLOGIES,required=True);q.add_argument('--model',choices=['kimi-k3','glm-5.3','deepseek-v4.1-flash-1017'],required=True);q.add_argument('--window',choices=['prefill','decode','train'],default='decode')
    for name in ('validate','run','analyze'):
        q=sub.add_parser(name);q.add_argument('case')
    q=sub.add_parser('matrix');q.add_argument('dest');q.add_argument('--workers',type=int,choices=[1,2,3,4],default=2)
    a=p.parse_args()
    if a.cmd=='build':
        subprocess.run(['cmake','-S',str(SIM),'-B',str(SIM/'build'),'-DNS3_TESTS=OFF','-DNS3_MTP=ON','-DNS3_EXAMPLES=OFF'],check=True)
        subprocess.run(['cmake','--build',str(SIM/'build'),'-j','4','--target','scratch_sue-harness-v3'],check=True)
    elif a.cmd=='init':
        ts,rs,meta=build(TOPOLOGIES[a.topology],a.model,a.window);print(create(a.case,a.topology,ts,meta,routes=rs))
    elif a.cmd=='validate':validate_case(Path(a.case));print('PASS')
    elif a.cmd=='run':
        r=run_case(a.case);print(json.dumps(r,ensure_ascii=False,indent=2));sys.exit(0 if r['status']=='passed' else 1)
    elif a.cmd=='analyze':print(json.dumps(analyze(Path(a.case)),ensure_ascii=False,indent=2))
    elif a.cmd=='matrix':matrix(a.dest,a.workers)
