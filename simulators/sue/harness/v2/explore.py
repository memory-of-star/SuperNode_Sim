#!/usr/bin/env python3
"""Predeclared paired sensitivity experiments; preserve every attempt."""
import json,sys,time,statistics,traceback,hashlib
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
from collections import Counter
from run import create,run_case,dump,csvread,csvwrite,HERE,BINARY,SOURCE,sha
from workloads import build,task
from topologies import TOPOLOGIES

PLAN='''# SUE v2 受控探索：预注册实验设计

目标：寻找接线、放置、UCIe资源、信用及突发调度的适用条件，不预设拓扑赢家。

配对控制：128 XPU，parity/rotate，外链200G、传播20ns、交换预算300ns；单线程、LLR+CBFC。模型为冻结的Kimi K3/GLM5.3配置推导的decode代表层，4步。路由与口哈希使用种子1/2/3；同一配对共用种子，跨拓扑tasks.json和token_routes.csv必须完全相同。三个种子同时改变路由和哈希，不能分离其贡献。报告逐seed配对差值和均值/范围，不把3样本当可靠置信区间。

| 实验 | 自变量 | 固定控制与预测 |
|---|---|---|
| topology | token=64/256，uniform/hotspot80，计算等待0/5000ns；两模型两拓扑三种子 | 双因素负载×等待下比较接线；预测热点可改变排名，5us可能压低相对差异 |
| placement | community80下spread/affinity；256token，等待0/5000ns | 专家chip不变，Die按0,1,2,3→0,2,1,3双射置换；相同逻辑专家选择。预测亲和减少parity绕行但可能加剧热点；本地消除可能改变网络字节，需单列 |
| ring_rate | 100/200/400/800 Gbit/s | 两模型、uniform256token、零计算；64KiB缓冲、20ns/hop。预测低带宽放大排队 |
| ring_buffer | 4/16/64/256 KiB | 同上，400G；预测小缓冲背压，容量增加可能出现收益平台 |
| ring_delay | 20/100/500ns/hop | 同上，400G、64KiB；预测环延迟变化影响绕行消息 |
| congestion | CBFC信用16/64/512个256B单元 | 32源Die0各发256KiB到卡127Die1，强制组0；同任务比较，观察信用门控，不能把检查次数当持续时间 |
| packetization | 分片512/2048/8192 B | 上述incast，信用512、源队列64KiB；预测分片数与注入间隔共同影响；不是只改变以太网MTU |
| scheduling | burst/stagger/batched | 同一incast字节与端点；stagger源间错开2us；batched每批8项，后一批依赖前一批全部完成。以共同1us开始计完成时间，不扣除人为等待 |
| reliability | badEvery=0/100/20 | 上述incast，固定源卡0入口按每N个数据帧确定性损坏；不是全网随机BER。预测重传增加，关键路径是否受影响看窗口与卡0任务 |

uniform随机无放回选top-k；hotspot80每token约80%槽位固定从前32专家选择（Kimi13/16，GLM6/8），其余从外部专家选择。community80按token奇偶选择专家社区(e//2)%2，约80%槽位落相同社区；affinity只置换专家Die，token位置不变。decode以0.75概率沿用上步集合。

计算等待不模拟GPU：0ns是去除专家等待的网络敏感性对照，仍有DAG通知和事务确认；5000ns固定且不随工作量缩放。当前实验不研究真实计算扩卡、MPU内存服务或实测router。

保留失败、超时、原始CSV及输入/二进制哈希。失败案例不进入性能均值，配对必须双方成功，不能拿不完整任务的早停当更快。截止5ms，单例墙钟60s；4个独立实验进程。实验因素交互以预定义实验组分别解释。
'''

def definitions():
    out=[]
    def add(study,variant,model='kimi-k3',tokens=256,compute=0,profile='uniform',placement='spread',settings=None,probe=False,schedule='burst'):
        for seed in (1,2,3):
            for topo in ('xpu128-parity','xpu128-rotate'):
                out.append(dict(study=study,variant=variant,model=model,tokens=tokens,compute=compute,profile=profile,placement=placement,settings=settings or {},seed=seed,topology=topo,probe=probe,schedule=schedule))
    for model in ('kimi-k3','glm-5.3'):
        for tokens in (64,256):
            for profile in ('uniform','hotspot80'):
                for compute in (0,5000):add('topology',f'n{tokens}-{profile}-c{compute}',model,tokens,compute,profile)
        for placement in ('spread','affinity'):
            for compute in (0,5000):add('placement',f'{placement}-c{compute}',model,256,compute,'community80',placement)
        for rate in (100,200,400,800):add('ring_rate',str(rate),model,settings={'ring_gbps':rate})
        for capacity in (4096,16384,65536,262144):add('ring_buffer',str(capacity),model,settings={'ring_bytes':capacity})
        for delay in (20,100,500):add('ring_delay',str(delay),model,settings={'ring_delay_ns':delay})
    for credits in (16,64,512):add('congestion',str(credits),model='incast',settings={'credits':credits},probe=True)
    for payload in (512,2048,8192):add('packetization',str(payload),model='incast',settings={'payload':payload},probe=True)
    for schedule in ('burst','stagger','batched'):add('scheduling',schedule,model='incast',probe=True,schedule=schedule)
    for every in (0,100,20):add('reliability',str(every),model='incast',settings={'bad_every':every},probe=True)
    return out

def workload(d):
    if not d['probe']:
        return build(TOPOLOGIES[d['topology']],d['model'],'decode',d['seed'],tokens=d['tokens'],compute_ns=d['compute'],profile=d['profile'],placement=d['placement'])
    tasks=[]
    for s in range(32):
        deps=list(range((s//8-1)*8,s//8*8)) if d['schedule']=='batched' and s>=8 else []
        delay=(1000+s*2000 if d['schedule']=='stagger' else (0 if deps else 1000))
        tasks.append(dict(task(s,0,127,1,262144,delay,deps,label='incast',group=0),id=s))
    return tasks,[],dict(model='incast',window='32-source',schedule=d['schedule'],scope='synthetic incast, not model trace')

def augment(r,d):
    r={**r,**{k:d[k] for k in ('study','variant','seed','profile','placement','compute','tokens','schedule')}}
    if not r.get('run'):return r
    run=Path(r['run'])
    if not all((run/name).exists() for name in ['port_statistics.csv','ring_statistics.csv','task_statistics.csv']):return r
    ps=csvread(run/'port_statistics.csv');rs=csvread(run/'ring_statistics.csv');ts=csvread(run/'task_statistics.csv')
    frames=sum(int(x['frames']) for x in rs);pools={int(x['chip']):int(x['pool_blocked_polls']) for x in rs}
    r.update(ring_frames=frames,ring_mean_queue_ns=r.get('ucie_queue_ps',0)/max(1,frames)/1000,
        ring_pool_blocked_polls=sum(pools.values()),max_port_busy_pct=max(int(x['tx_busy_ps']) for x in ps if x['side']=='1')/max(1,r.get('sim_end_us',0)*1e6)*100,
        network_tasks=sum(int(x['bytes'])>0 for x in ts),epoch_completion_us=max(int(x['end_ps']) for x in ts)/1e6-1 if ts else None,
        card0_task_us=max(((int(x['end_ps'])-int(x['start_ps']))/1e6 for x in ts if x['src_chip']=='0' and int(x['bytes'])),default=0),
        ucie_share_pct=100*sum(v for k,v in r.get('task_path_bytes',{}).items() if int(k)!=0)/max(1,r['logical_bytes']),
        task_sha256=sha(run/'tasks.json'),routes_sha256=sha(run/'token_routes.csv') if (run/'token_routes.csv').exists() else None)
    return r

def main(dest,workers):
    dest=Path(dest).resolve();dest.mkdir(parents=True,exist_ok=False)
    defs=definitions();dump(dest/'design.json',defs);(dest/'plan.md').write_text(PLAN+f'\n预注册总案例：{len(defs)}。\n')
    dump(dest/'implementation.json',{'binary':sha(BINARY),'adapter':sha(SOURCE),'sources':{p.name:sha(p) for p in HERE.glob('*.py')}})
    # All comparisons are declared before any run starts.
    results=[]
    def execute(item):
        i,d=item;case=dest/'cases'/f'{i:03d}-{d["study"]}-{d["model"]}-{d["variant"]}-s{d["seed"]}-{d["topology"]}'
        try:
            ts,routes,meta=workload(d)
            meta['exploration']=d
            create(case,d['topology'],ts,meta,dict(seed=d['seed'],stop_ns=5000000,timeout_s=60,**d['settings']),routes)
            (case/'experiment-spec.md').write_text(PLAN+'\n当前案例：\n```json\n'+json.dumps(d,ensure_ascii=False,indent=2)+'\n```\n')
            return augment(run_case(case),d)
        except Exception as e:
            return dict(**d,status='runner_error',case=str(case),error=str(e),traceback=traceback.format_exc())
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures=[pool.submit(execute,x) for x in enumerate(defs)]
        for f in as_completed(futures):
            r=f.result();results.append(r);dump(dest/'results.json',results)
            print(json.dumps(dict(done=len(results),total=len(defs),study=r['study'],model=r['model'],variant=r['variant'],seed=r['seed'],topology=r['topology'],status=r['status'],us=r.get('makespan_us'),error=r.get('error')),ensure_ascii=False),flush=True)
    print(Counter(r['status'] for r in results),flush=True)

if __name__=='__main__':main(sys.argv[1],int(sys.argv[2]) if len(sys.argv)>2 else 4)
