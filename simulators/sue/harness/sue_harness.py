#!/usr/bin/env python3
"""SUE experiment harness: stdlib only; no UB protocol dependency."""
import argparse
import copy
import csv
from datetime import datetime, timezone
import hashlib
import io
import html
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid
from collections import deque
from report import render

ROOT = Path(__file__).resolve().parents[1]
SIM = ROOT / 'ns-3-alibabacloud/simulation'
BINARY = SIM / 'build/scratch/ns3.36.1-sue-harness-default'
DEFAULT = {
    'version': 1, 'name': 'sue-moe-demo',
    'goal': '验证 dispatch 完成后执行固定计算等待，再进行 combine；网络协议为 SUE。',
    'topology': {'kind': 'single_switch', 'hosts': 4, 'hosts_per_leaf': 2, 'spines': 2},
    'network': {'rate_gbps': 100, 'prop_ns': 100, 'fwd_ns': 300, 'fc': 'CBFC', 'llr': True,
                'credits': 64, 'credit_size': 256, 'cf_threshold_bytes': 2075,
                'ack_spacing_bytes': 2048, 'error_rate': 0, 'bad_every': 0},
    'runtime': {'threads': 1, 'seed': 1, 'payload_bytes': 2048, 'interval_ns': 200,
                'queue_bytes': 16384, 'visibility_ns': 100, 'stop_ns': 200000, 'timeout_s': 120},
    'workload': {'kind': 'moe', 'bytes_per_peer': 16384, 'compute_ns': 5000},
    'prediction': '任务全部交付，combine 的开始时间晚于依赖的 dispatch 完成和计算等待。',
}
FIELDS = ['id','src','dst','bytes','delay_ns','vc','deps','label']

def dump(path, obj):
    path.write_text(json.dumps(obj,ensure_ascii=False,indent=2)+'\n')

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''): h.update(block)
    return h.hexdigest()

def integer(x, low, high, label):
    if type(x) is not int or not low <= x <= high: raise ValueError(f'{label}: integer in [{low}, {high}] required')

def validate_spec(s):
    if set(s)-set(DEFAULT): raise ValueError('unknown experiment fields: '+str(set(s)-set(DEFAULT)))
    if s.get('version')!=1: raise ValueError('version must be 1')
    for key in ('name','goal','prediction'):
        if not isinstance(s.get(key),str) or not s[key].strip(): raise ValueError(f'{key} is required')
    for group in ('topology','network','runtime'):
        if set(s[group]) != set(DEFAULT[group]): raise ValueError(f'{group}: fields must be {list(DEFAULT[group])}')
    t,n,r=s['topology'],s['network'],s['runtime']
    integer(t['hosts'],2,1024,'hosts'); integer(t['hosts_per_leaf'],1,1024,'hosts_per_leaf'); integer(t['spines'],1,16,'spines')
    if t['kind'] not in ('single_switch','leaf_spine'): raise ValueError('unsupported topology')
    if t['kind']=='leaf_spine' and t['hosts']%t['hosts_per_leaf']: raise ValueError('hosts must be divisible by hosts_per_leaf')
    for k,lo,hi in [('rate_gbps',1,800),('prop_ns',1,100000),('fwd_ns',0,100000),('credits',1,16383),('credit_size',1,65536),('cf_threshold_bytes',1,100000000),('ack_spacing_bytes',400,16384),('bad_every',0,100000000)]: integer(n[k],lo,hi,k)
    if n['fc'] not in ('CBFC','PFC','OFF') or type(n['llr']) is not bool: raise ValueError('invalid FC or LLR')
    if type(n['error_rate']) not in (int,float) or not 0<=n['error_rate']<=1: raise ValueError('invalid error_rate')
    if n['bad_every'] and n['error_rate']: raise ValueError('choose deterministic or random errors, not both')
    if not n['llr'] and (n['bad_every'] or n['error_rate']): raise ValueError('this adapter currently requires LLR for error injection')
    for k,lo,hi in [('threads',1,64),('seed',1,2147483646),('payload_bytes',1,65500),('interval_ns',1,100000000),('queue_bytes',64,1000000000),('visibility_ns',1,100000000),('stop_ns',1000,1000000000),('timeout_s',1,3600)]: integer(r[k],lo,hi,k)
    # Same delay in 1-thread and MTP: a declared scheduling abstraction, not an implicit timing offset.
    if r['visibility_ns']<n['prop_ns']: raise ValueError('visibility_ns must be >= prop_ns for MTP causality')
    frame=r['payload_bytes']+18  # MAC + VLAN, no shim
    if r['queue_bytes']<frame: raise ValueError('host queue cannot fit one frame')
    if n['fc']=='CBFC':
        need=(frame+n['credit_size']-1)//n['credit_size']
        if need>n['credits']: raise ValueError('credit window cannot fit one frame')
        if n['cf_threshold_bytes']>n['credits']*n['credit_size']: raise ValueError('CF threshold exceeds credit window')
    w=s['workload']
    if w.get('kind') not in ('p2p','ring','moe','custom'): raise ValueError('unsupported workload')
    allowed={'kind','bytes_per_peer','compute_ns'} if w['kind']!='custom' else {'kind','provenance'}
    if set(w)-allowed: raise ValueError('unknown workload fields')
    if w['kind']!='custom':
        integer(w['bytes_per_peer'],1,1000000000,'bytes_per_peer'); integer(w['compute_ns'],0,1000000000,'compute_ns')

def generate(s):
    hosts=s['topology']['hosts']; w=s['workload']; tasks=[]
    def add(src,dst,size,delay,deps,label):
        i=len(tasks); tasks.append(dict(id=i,src=src,dst=dst,bytes=size,delay_ns=delay,vc=0,deps=deps,label=label)); return i
    b=w['bytes_per_peer']
    if w['kind']=='p2p': add(0,1,b,10000,[],'p2p')
    elif w['kind']=='ring':
        for h in range(hosts): add(h,(h+1)%hosts,b,10000,[],'ring')
    elif w['kind']=='moe':
        dispatch=[add(0,h,b,10000,[],'dispatch') for h in range(1,hosts)]
        for h,d in zip(range(1,hosts),dispatch):
            c=add(h,h,0,w['compute_ns'],[d],'compute-wait')
            add(h,0,b,0,[c],'combine')
    return tasks

def load_tasks(case):
    result=[]
    with (case/'traffic.csv').open(newline='') as f:
        reader=csv.DictReader(f)
        if reader.fieldnames!=FIELDS: raise ValueError(f'traffic.csv header must be {FIELDS}')
        for row in reader:
            t={k:int(row[k]) for k in ('id','src','dst','bytes','delay_ns','vc')}
            t['deps']=[int(x) for x in row['deps'].split()]; t['label']=row['label']; result.append(t)
    return result

def write_tasks(case,tasks):
    with (case/'traffic.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=FIELDS); writer.writeheader()
        for t in tasks: writer.writerow({**t,'deps':' '.join(map(str,t['deps']))})

def validate_tasks(s,tasks):
    if not tasks: raise ValueError('empty traffic')
    if len(tasks)>200000: raise ValueError('task count exceeds bounded harness limit 200000')
    count=len(tasks); nexts=[[] for _ in tasks]; pending=[]
    for i,t in enumerate(tasks):
        if t['id']!=i: raise ValueError('task ids must be contiguous from 0')
        for k,lo,hi in [('src',0,s['topology']['hosts']-1),('dst',0,s['topology']['hosts']-1),('bytes',0,1000000000),('delay_ns',0,s['runtime']['stop_ns']),('vc',0,7)]: integer(t[k],lo,hi,k)
        if (t['bytes']==0)!=(t['src']==t['dst']): raise ValueError('zero-byte tasks must be local waits; network tasks must have different endpoints')
        if len(set(t['deps']))!=len(t['deps']): raise ValueError('duplicate dependency')
        for d in t['deps']:
            integer(d,0,count-1,'dependency'); nexts[d].append(i)
        pending.append(len(t['deps']))
    q=deque(i for i,x in enumerate(pending) if not x); seen=0
    while q:
        i=q.popleft(); seen+=1
        for j in nexts[i]:
            pending[j]-=1
            if not pending[j]: q.append(j)
    if seen!=count: raise ValueError('dependency cycle')
    return {'tasks':count,'logical_bytes':sum(t['bytes'] for t in tasks),'estimated_data_frames':sum((t['bytes']+s['runtime']['payload_bytes']-1)//s['runtime']['payload_bytes'] for t in tasks),'dependency_edges':sum(len(t['deps']) for t in tasks)}

def read_case(case):
    s=json.loads((case/'experiment.json').read_text()); validate_spec(s)
    tasks=load_tasks(case); metrics=validate_tasks(s,tasks); return s,tasks,metrics

def prepare(case,s,tasks):
    t,n,r=s['topology'],s['network'],s['runtime']; hosts=t['hosts']
    if t['kind']=='single_switch': switches=[hosts]; links=[(h,hosts) for h in range(hosts)]
    else:
        leaves=hosts//t['hosts_per_leaf']; switches=list(range(hosts,hosts+leaves+t['spines']))
        links=[(h,hosts+h//t['hosts_per_leaf']) for h in range(hosts)]
        links += [(hosts+l,hosts+leaves+p) for l in range(leaves) for p in range(t['spines'])]
    (case/'topology.txt').write_text('type=SUE\nnodes '+str(hosts+len(switches))+'\nswitch '+' '.join(map(str,switches))+'\n'+''.join(f'link {a} {b} {n["rate_gbps"]}Gbps {int(n["llr"])} {n["fc"]} {n["error_rate"]}\n' for a,b in links))
    # Versioned stock preset + explicit resolved harness overrides; record complete input file.
    preset=(SIM/'conf/sue-400g.conf').read_text()
    cfg={}
    for line in preset.splitlines():
        line=line.split('#',1)[0].strip()
        if line:
            parts=line.split(None,1); cfg[parts[0]]=parts[1] if len(parts)>1 else ''
    cfg.update({'ENABLE_SCALEUP':'1','ENABLE_SCALEUP_WORKLOAD':'0','SUE_MTP_THREADS':str(r['threads']),
        'SUE_LINK_PROPAGATION_NS':str(n['prop_ns']),'SUE_SWITCH_FWD_BUDGET_NS':str(n['fwd_ns']),
        'SUE_ETH_TYPE_0_HAS_SHIM':'0','PCP_TO_VC_MAP':','.join(f'{i}:{i%8}' for i in range(16)),
        'FC_MODE':n['fc'],'CBFC_CREDIT_SIZE':str(n['credit_size']),
        'CBFC_VC_CREDIT_LIMIT':','.join(f'{i}:{n["credits"]}' for i in range(8)),
        'CBFC_CF_THRESHOLD_BYTES':str(n['cf_threshold_bytes']),'CBFC_EGRESS_BUF_BYTES':'0',
        'CBFC_CF_MIN_TIMER_BYTES':'0','CBFC_CF_MAX_TIMER_BYTES':'1250000',
        'LLR_CTLOS_TARGET_SPACING':str(n['ack_spacing_bytes']),
        'LLR_REPLAY_TIMER_MAX_NS':'0','LLR_INIT_TIMER_MAX_NS':'0',
        'LLR_DATA_AGE_TIMER_MAX_NS':'4000000000','LLR_REPLAY_CT_MAX':'255'})
    (case/'sue.conf').write_text(''.join(f'{k} {v}\n' for k,v in cfg.items()))
    (case/'tasks.tsv').write_text('# id src dst bytes delay_ns vc deps\n'+''.join(f'{t["id"]} {t["src"]} {t["dst"]} {t["bytes"]} {t["delay_ns"]} {t["vc"]} '+(','.join(map(str,t['deps'])) or '-')+'\n' for t in tasks))

def create(case,s,tasks=None):
    validate_spec(s)
    if tasks is None: tasks=generate(s)
    validate_tasks(s,tasks)
    case.mkdir(parents=True,exist_ok=False)
    dump(case/'experiment.json',s); write_tasks(case,tasks)
    (case/'experiment-spec.md').write_text(f'# {s["name"]}\n\n{s["goal"]}\n\n预测（运行前记录）：{s["prediction"]}\n\n协议：SUE AFH + 配置的 LLR / FC。计算：固定等待，无 GPU 资源排队。\n\n事实来源为 experiment.json 和 traffic.csv；更改实验时同步更新这两个文件。\n')
    print(case)

def build():
    logdir=ROOT/'harness/build-logs'; logdir.mkdir(exist_ok=True)
    log=logdir/(datetime.now().strftime('%Y%m%d-%H%M%S')+'.log')
    commands=[['cmake','-S',str(SIM),'-B',str(SIM/'build'),'-DNS3_EXAMPLES=OFF','-DNS3_TESTS=OFF','-DNS3_MTP=ON','-DCMAKE_CXX_FLAGS=-include stdint.h'],['cmake','--build',str(SIM/'build'),'--target','scratch_sue-harness','-j','8']]
    with log.open('w') as f:
        for c in commands:
            p=subprocess.run(c,stdout=f,stderr=subprocess.STDOUT)
            if p.returncode: raise RuntimeError(f'build failed: {log}')
    print('BUILD PASS',log)

def analyze(run):
    execution=json.loads((run/'execution.json').read_text())
    s,tasks,_=read_case(run)
    stats=[]; ports=[]
    if (run/'task_statistics.csv').exists(): stats=list(csv.DictReader(io.StringIO((run/'task_statistics.csv').read_text())))
    if (run/'port_statistics.csv').exists(): ports=list(csv.DictReader(io.StringIO((run/'port_statistics.csv').read_text())))
    valid=len(stats)==len(tasks) and {int(x['task_id']) for x in stats}==set(range(len(tasks)))
    byid={int(x['task_id']):x for x in stats}; errors=[]
    for t in tasks:
        x=byid.get(t['id'])
        if x is None: errors.append(f'missing task {t["id"]}'); continue
        if int(x['bytes'])!=t['bytes'] or int(x['received_bytes'])!=t['bytes'] or x['status']!='complete': valid=False
        for d in t['deps']:
            if int(x['start_ps'])<0: continue  # Not started is incomplete, not an ordering violation.
            y=byid.get(d)
            if y is None or int(y['end_ps'])<0 or int(x['start_ps'])<int(y['end_ps'])+(s['runtime']['visibility_ns']+t['delay_ns'])*1000:
                valid=False; errors.append(f'dependency violation {d}->{t["id"]}')
    discards=sum(int(p['discard']) for p in ports)
    passed=execution['returncode']==0 and valid and bool(ports) and discards==0 and 'SUE HARNESS PASS' in (run/'stdout.log').read_text()
    ends=[int(x['end_ps']) for x in stats if int(x['end_ps'])>=0]
    starts=[int(x['start_ps']) for x in stats if int(x['start_ps'])>=0]
    summary={'status':'passed' if passed else 'failed','tasks':len(tasks),'completed':sum(x['status']=='complete' for x in stats),
       'logical_bytes':sum(t['bytes'] for t in tasks),'received_bytes':sum(int(x['received_bytes']) for x in stats),
       'makespan_us':(max(ends)-min(starts))/1e6 if ends and starts else None,
       'replay_frames':sum(int(p['replay_frames']) for p in ports),'replay_sessions':sum(int(p['replay_sessions']) for p in ports),
       'credit_exhausted_checks':sum(int(p['credit_exhausted']) for p in ports),'discard':discards,
       'queue_blocked_polls':sum(int(x['queue_blocked_polls']) for x in stats),'errors':errors,'wall_seconds':execution['wall_seconds']}
    dump(run/'summary.json',summary); render(run,s,tasks,stats,ports,summary)
    return summary

def run_case(case):
    s,tasks,metrics=read_case(case)
    if not BINARY.exists(): raise ValueError('backend missing: run build first')
    dest=case/'runs'/(datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+uuid.uuid4().hex[:8]); dest.mkdir(parents=True)
    dump(dest/'experiment.json',s); write_tasks(dest,tasks); prepare(dest,s,tasks); dump(dest/'validation.json',metrics)
    r=s['runtime']; n=s['network']
    cmd=[str(BINARY),'--case='+str(dest),'--out='+str(dest),f'--threads={r["threads"]}',f'--seed={r["seed"]}',f'--payload={r["payload_bytes"]}',f'--intervalNs={r["interval_ns"]}',f'--visibilityNs={r["visibility_ns"]}',f'--queueBytes={r["queue_bytes"]}',f'--stopNs={r["stop_ns"]}',f'--badEvery={n["bad_every"]}']
    manifest={'command':cmd,'cwd':str(dest),'timeout_s':r['timeout_s'],'input_sha256':{p.name:sha(p) for p in [dest/'experiment.json',dest/'traffic.csv',dest/'tasks.tsv',dest/'topology.txt',dest/'sue.conf']},'binary_sha256':sha(BINARY),'adapter_sha256':sha(SIM/'scratch/sue-harness.cc'), 'harness_sha256':sha(Path(__file__)), 'shared_libraries_sha256':{p.name:sha(p) for pattern in ('*sue-default.so','*mtp-default.so','*core-default.so','*network-default.so') for p in (SIM/'build/lib').glob(pattern)}}
    dump(dest/'command-manifest.json',manifest)
    execution={'status':'running','returncode':None,'wall_seconds':0}; dump(dest/'execution.json',execution)
    start=time.monotonic()
    try:
        with (dest/'stdout.log').open('w') as f:
            p=subprocess.run(cmd,cwd=dest,stdout=f,stderr=subprocess.STDOUT,timeout=r['timeout_s']); execution['returncode']=p.returncode; execution['status']='exited'
    except subprocess.TimeoutExpired: execution.update(status='timeout',returncode=124)
    except OSError as e: execution.update(status='launch-error',returncode=127,error=str(e))
    except KeyboardInterrupt: execution.update(status='interrupted',returncode=130)
    finally:
        execution['wall_seconds']=round(time.monotonic()-start,4); dump(dest/'execution.json',execution)
    summary=analyze(dest)
    entry={'run':str(dest),'status':summary['status'],'returncode':execution['returncode'],'wall_seconds':execution['wall_seconds']}
    with (case/'run-ledger.jsonl').open('a') as f: f.write(json.dumps(entry)+'\n')
    dump(case/'latest.json',entry)
    print(json.dumps({**summary,'run':str(dest)},ensure_ascii=False))
    return summary['status']=='passed'

def import_ub(args):
    source=Path(args.source).resolve(); allrows=list(csv.DictReader(io.StringIO(source.read_text())))
    rows=allrows[:args.max_tasks] if args.max_tasks else allrows
    if not rows: raise ValueError('empty UB CSV')
    ids={int(x['taskId']):i for i,x in enumerate(rows)}
    if len(ids)!=len(rows): raise ValueError('duplicate UB taskId')
    phases={}
    for row in allrows: phases.setdefault(row['phaseId'],[]).append(int(row['taskId']))
    tasks=[]
    for i,row in enumerate(rows):
        if row['opType'] not in ('URMA_WRITE','URMA_SEND'): raise ValueError('unsupported UB operation: '+row['opType'])
        delay=re.fullmatch(r'(\d+)(ns|us|ms)',row['delay'])
        if not delay: raise ValueError('unsupported delay: '+row['delay'])
        deps=[]
        for phase in row['dependOnPhases'].split():
            if phase not in phases: raise ValueError('missing phase '+phase)
            for d in phases[phase]:
                if d not in ids: raise ValueError('selected prefix cuts a dependency phase; increase --max-tasks')
                deps.append(ids[d])
        tasks.append(dict(id=i,src=int(row['sourceNode']),dst=int(row['destNode']),bytes=int(row['dataSize(Byte)']),delay_ns=int(delay[1])*{'ns':1,'us':1000,'ms':1000000}[delay[2]],vc=int(row['priority']),deps=sorted(set(deps)),label='imported-'+row['phaseId']))
    s=copy.deepcopy(DEFAULT); s['name']=args.name; s['topology']['hosts']=max(max(t['src'],t['dst']) for t in tasks)+1
    s['topology']['kind']='leaf_spine'; s['topology']['hosts_per_leaf']=8
    if s['topology']['hosts']%8: s['topology']['kind']='single_switch'
    s['runtime']['stop_ns']=2000000
    s['goal']='在 SUE 网络上回放 UB CSV 的逻辑通信字节和 phase 依赖；不保留 UB 事务语义。'
    s['workload']={'kind':'custom','provenance':{'source':str(source),'sha256':sha(source),'selected_rows':len(rows),'total_rows':len(allrows),'mapping':'URMA_WRITE/URMA_SEND -> one-way SUE delivery; priority -> identity VC; no UB transaction semantics'}}
    create(Path(args.case).resolve(),s,tasks)

def comparison_report(out,results):
    out=out.resolve()
    rows=[]
    for entry in results:
        case=Path(entry['case']); run=Path(json.loads((case/'latest.json').read_text())['run']); summary=json.loads((run/'summary.json').read_text())
        rows.append({'value':entry['value'],'status':summary['status'],'completed':summary['completed'],'tasks':summary['tasks'],
                     'makespan_us':summary['makespan_us'],'replay_frames':summary['replay_frames'],
                     'credit_exhausted_checks':summary['credit_exhausted_checks'],'report':str((run/'report.html').relative_to(out))})
    if rows:
        with (out/'comparison.csv').open('w',newline='') as f:
            w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    matrix=json.loads((out/'matrix.json').read_text())
    text='<meta charset="utf-8"><title>SUE Scale-Up Ethernet 仿真平台 v2 单变量实验</title><style>body{font:16px/1.6 system-ui;margin:40px}td,th{padding:12px;border-bottom:1px solid #ccc}table{border-collapse:collapse}</style><h1>SUE Scale-Up Ethernet 仿真平台 v2 单变量实验</h1>'
    text+='<p>变化参数：'+html.escape(matrix['changed_variable'])+'</p><p>运行前预测：'+html.escape(matrix['prediction'])+'</p><p>其他配置与任务保持一致。未完成案例保留为 failed；信用检查次数不是阻塞时长。时间为合成工作负载窗口，不能当作真实模型推理延迟。</p>'
    text+='<table><tr><th>参数值</th><th>状态</th><th>完成任务</th><th>窗口 µs</th><th>重传帧</th><th>信用检查失败</th><th>证据</th></tr>'
    for r in rows:
        text+='<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in (r['value'],r['status'],str(r['completed'])+'/'+str(r['tasks']),r['makespan_us'],r['replay_frames'],r['credit_exhausted_checks']))+'<td><a href="'+html.escape(r['report'],quote=True)+'">交互报告</a></td></tr>'
    (out/'report.html').write_text(text+'</table>')

def main():
    p=argparse.ArgumentParser(description=__doc__); sub=p.add_subparsers(dest='cmd',required=True)
    q=sub.add_parser('init'); q.add_argument('case'); q.add_argument('--workload',choices=['p2p','ring','moe'],default='moe'); q.add_argument('--hosts',type=int,default=4)
    sub.add_parser('build')
    for name in ['validate','run']:
        q=sub.add_parser(name); q.add_argument('case')
    q=sub.add_parser('analyze'); q.add_argument('run')
    q=sub.add_parser('import-ub'); q.add_argument('source'); q.add_argument('case'); q.add_argument('--max-tasks',type=int,default=0); q.add_argument('--name',default='ub-traffic-on-sue')
    q=sub.add_parser('sweep'); q.add_argument('case'); q.add_argument('output'); q.add_argument('--field',choices=['network.credits','network.bad_every','runtime.threads','network.rate_gbps'],required=True); q.add_argument('--values',required=True); q.add_argument('--prediction',required=True)
    args=p.parse_args()
    try:
        if args.cmd=='init':
            s=copy.deepcopy(DEFAULT); s['topology']['hosts']=args.hosts; s['workload']['kind']=args.workload; create(Path(args.case).resolve(),s)
        elif args.cmd=='build': build()
        elif args.cmd=='validate': print(json.dumps(read_case(Path(args.case))[2]))
        elif args.cmd=='run': return 0 if run_case(Path(args.case).resolve()) else 1
        elif args.cmd=='analyze': print(json.dumps(analyze(Path(args.run).resolve()),ensure_ascii=False))
        elif args.cmd=='import-ub':
            if args.max_tasks<0: raise ValueError('max-tasks must be >= 0')
            import_ub(args)
        elif args.cmd=='sweep':
            base,tasks,_=read_case(Path(args.case)); out=Path(args.output).resolve()
            values=[int(x) for x in args.values.split(',')]
            if not values or len(values)!=len(set(values)): raise ValueError('sweep values must be nonempty and unique')
            group,key=args.field.split('.'); cases=[]
            for value in values:
                s=copy.deepcopy(base); s[group][key]=value; s['prediction']=args.prediction; validate_spec(s); cases.append((value,s))
            out.mkdir(parents=True,exist_ok=False)
            dump(out/'matrix.json',{'changed_variable':args.field,'values':values,'prediction':args.prediction,'fixed_controls':'All other experiment fields and traffic.csv','base':base})
            results=[]
            for value,s in cases:
                case=out/str(value); create(case,s,tasks); passed=run_case(case); results.append({'value':value,'passed':passed,'case':str(case)}); dump(out/'run-ledger.json',results); comparison_report(out,results)
            return 0 if all(x['passed'] for x in results) else 1
        return 0
    except (ValueError,KeyError,FileNotFoundError,FileExistsError,RuntimeError) as e:
        print('HARNESS ERROR:',e,file=sys.stderr); return 2
if __name__=='__main__': sys.exit(main())
