"""Audit the offload sweep and emit reproducible tables, plots and offline HTML."""
import argparse
import base64
from collections import Counter, defaultdict
import csv
import hashlib
import html
import json
from pathlib import Path
import statistics

import pcie_config
import hbm_config
import io_die_config

from run import csvread, csvwrite, dump, sha

SHORT = {'glm-5.3':'GLM-5.3','kimi-k3':'Kimi-K3','deepseek-v4.1-flash-1017':'DeepSeek'}
TARGET = {'kv':'KV cache','engram':'Engram','both':'KV + Engram'}


def group_key(r):
    return (r['model'],r['target'],r['backend'],r['mpus'],r.get('study_variant','main'),
            r.get('mpu_fraction',-1),r.get('pcie_setup_ns',0))


def audit(root, results):
    checks, controls, details = [], {}, []
    task_count = 0
    for r in results:
        if r['status']!='passed':
            continue
        p=Path(r['run'])
        manifest=json.loads((p/'command-manifest.json').read_text())
        assert all(sha(p/name)==value for name,value in manifest['inputs'].items())
        assert sha(p/'tasks.json')==r['tasks_sha256']
        assert sha(p/'resources.json')==r['resources_sha256']
        visibility=json.loads((p/'experiment.json').read_text())['settings']['visibility_ns']
        tasks=json.loads((p/'tasks.json').read_text())
        resources=json.loads((p/'resources.json').read_text())
        stats={int(x['id']):x for x in csvread(p/'task_statistics.csv')}
        rss={int(x['id']):x for x in csvread(p/'resource_statistics.csv')}
        from io_die_config import managed_edges
        streaming_edges=managed_edges(p)
        queues=defaultdict(list);busy=Counter();units=Counter();compute=Counter()
        for t in tasks:
            s=stats[t['id']];task_count+=1
            assert s['status']=='complete' and int(s['received_bytes'])==t['bytes']
            assert not int(s['invalid_frames'])
            assert all(int(s['start_ps'])>=int(stats[d]['data_end_ps' if d in t.get('receive_deps',[]) else 'end_ps'])+(visibility+t['delay_ns'])*1000 for d in t['deps'] if (t['id'],d) not in streaming_edges)
        assert abs((max(int(s['end_ps']) for s in stats.values())-1000000)/1e6-r['window_us'])<1e-7
        for q in resources:
            s=rss[q['id']];kind=q['kind'];t=tasks[q['id']]
            a,b=int(s['service_start_ps']),int(s['end_ps'])
            if kind==2 and (p/'mpu.json').exists():
                assert b>=a
                units[(int(s['chip']),int(s['die']),kind)]+=q['units']
            elif kind==1 and r.get('hbm_profile'):
                assert b>=a  # Time is determined by concurrent capped source service.
            elif kind in (3,5):
                assert int(s['local_received_bytes'])==q['units']
            else:
                assert b-a==q['service_ns']*1000
                die=0 if kind==4 else int(s['die'])
                key=(int(s['chip']),die,kind)
                queues[key].append((a,b));busy[key]+=b-a;units[key]+=q['units']
            if kind==0:
                compute[t['label'],t['src_chip'],t['src_die']]+=q['units']
        for intervals in queues.values():
            intervals.sort()
            assert all(b[0]>=a[1] for a,b in zip(intervals,intervals[1:]))
        if r.get('hbm_profile'):
            assert hbm_config.load(p/'hbm.json')==r['hbm_profile']
            hbm_config.analyze(p)
        if (p/'io-die.json').exists():
            import io_die_config
            io_stats=io_die_config.analyze(p)
        if (p/'mpu.json').exists():
            import mpu_config
            mpu_config.analyze(p)
        requests=json.loads((p/'offload-requests.json').read_text())
        payload=Counter();by_xpu=Counter();by_mpu=Counter()
        for req in requests:
            payload[req['target']+'_'+req['backend']]+=req['bytes']
            if req['backend']=='pcie':
                by_xpu[req['chip']]+=req['bytes']
                setup=0 if r.get('pcie_profile') else r.get('pcie_setup_ns',0)
                if setup:
                    t=tasks[req['first_task']]
                    assert t['label'].endswith(':dma-setup')
                    assert t['delay_ns']==setup+(1000 if not t['deps'] else 0)
                    s=stats[t['id']]
                    if t['deps']:
                        expected=max(int(stats[d]['end_ps']) for d in t['deps'])+(visibility+setup)*1000
                        assert int(s['start_ps'])==expected
            else:by_mpu[req['bank']]+=req['bytes']
            assert int(stats[req['consumer_task']]['end_ps'])>=int(stats[req['first_task']]['end_ps'])
        assert dict(payload)==r['offload_bytes']
        if r.get('pcie_profile'):
            assert json.loads((p/'pcie.json').read_text())==r['pcie_profile']
            pcie_stats=pcie_config.analyze(p)
            assert sum(by_xpu.values())==sum(q['units'] for q in resources if q['kind']==5)
        else:
            assert sum(by_xpu.values())==sum(v for (c,d,k),v in units.items() if k==4)
        assert sum(by_mpu.values())==sum(v for (c,d,k),v in units.items() if k==2)
        key=(r['model'],r['target'],r['seed'],r['batch'],r['context'],r['tp'])
        control=(sha(p/'token_routes.csv'),sorted(compute.items()),
                 {target:sum(v for k,v in payload.items() if k.startswith(target+'_')) for target in ('kv','engram')})
        if key in controls:assert control==controls[key],(key,r['backend'])
        else:controls[key]=control
        diag=dict(case_index=r['case_index'],run=str(p),
                  pcie_payload_per_chip=dict(by_xpu),mpu_payload_per_node=dict(by_mpu),
                  max_pcie_service_us=max([v/1e6 for (c,d,k),v in busy.items() if k==4] or [0]),
                  max_mpu_service_us=max([v/1e6 for (c,d,k),v in busy.items() if k==2] or [0]),
                  max_resource_queue_us=max([int(x['queued_ps'])/1e6 for x in rss.values()] or [0]))
        if r.get('pcie_profile'):
            diag.pop('max_pcie_service_us')
            diag['pcie']=pcie_stats
        if (p/'io-die.json').exists():
            diag.pop('max_mpu_service_us',None)
            diag['io_die']=io_stats
        details.append(diag)
    failed=[dict(case_index=r['case_index'],model=r['model'],target=r['target'],backend=r['backend'],
                 mpus=r['mpus'],status=r['status'],error=r.get('error'),failures=r.get('failures')) for r in results if r['status']!='passed']
    verification=dict(status='passed' if not failed else 'has-failed-cases',planned=len(json.loads((root/'design.json').read_text())),
                      completed_records=len(results),passed=sum(r['status']=='passed' for r in results),
                      failed=failed,checked_tasks=task_count,paired_workload_groups=len(controls),
                      checks=['task/resource SHA256','complete delivery','DAG dependency ordering','exact service duration',
                              'resource non-overlap including shared PCIe across Die','MPU/PCIe request-byte conservation',
                              'same token routes, compute and requested bytes across all transport arms',
                              'configured PCIe setup present on every PCIe request and exact simulated wait'])
    assert verification['planned']==verification['completed_records']
    if any(r.get('pcie_profile') for r in results):
        verification['checks'][-1]='Transactional descriptor completion, payload/wire/DDR/HBM conservation and bounded DMA/tags/credits/RX; frozen profile matches every arm'
        verification['checks'][4]='Explicit HBM/compute and MPU bandwidth FIFO non-overlap; PCIe streaming staging audited separately'
    if any(r.get('hbm_profile') for r in results):
        verification['checks'][3]='Fixed service durations except fair HBM; fair HBM byte integrals and per-source/Die bandwidth bounds'
        verification['checks'][4]='Compute FIFO; MPU latency/inflight bounds, FIFO byte order and per-cycle bandwidth budget; overlapping HBM sources permitted and bounded'
    dump(root/'verification.json',verification);dump(root/'resource-diagnostics.json',details)
    return verification


def audit_partition(args):
    root,results=args
    root.mkdir(parents=True,exist_ok=False)
    dump(root/'design.json',results)
    return audit(root,results)

def parallel_audit(root,results,workers):
    # Keep every transport arm/seed pairing in one partition; no cross-arm check is lost.
    from multiprocessing import Pool
    from run import stamp
    groups=defaultdict(list)
    for r in results:groups[(r['model'],r['target'],r['seed'],r['batch'],r['context'],r['tp'])].append(r)
    base=root/('audit-parts-'+stamp());base.mkdir()
    args=[(base/str(i),rows) for i,rows in enumerate(groups.values())]
    with Pool(processes=workers,maxtasksperchild=1) as pool:parts=pool.map(audit_partition,args,chunksize=1)
    merged={**parts[0]}
    for key in ('planned','completed_records','passed','checked_tasks','paired_workload_groups'):merged[key]=sum(p[key] for p in parts)
    merged['failed']=[v for p in parts for v in p['failed']]
    merged['status']='passed' if not merged['failed'] else 'has-failed-cases'
    assert merged['planned']==len(json.loads((root/'design.json').read_text()))==len(results)
    details=[v for path,_ in args for v in json.loads((path/'resource-diagnostics.json').read_text())]
    dump(root/'verification.json',merged);dump(root/'resource-diagnostics.json',details)
    return merged


def aggregate(results):
    settings=[json.loads((Path(r['run'])/'experiment.json').read_text())['settings'] for r in results]
    clocks={(s.get('time_resolution_ns',1),s['interval_ns'],s['visibility_ns'],s.get('io_event_driven',0)) for s in settings}
    assert len(clocks)==1, 'Mixed precision or scheduling settings'
    precision,interval,visibility,event_driven=next(iter(clocks))
    slots={json.loads((Path(r['run'])/'io-die.json').read_text())['axi_mpu']['max_inflight'] for r in results if r.get('io_die') and 'axi_mpu' in json.loads((Path(r['run'])/'io-die.json').read_text())}
    assert len(slots)<=1, 'Mixed MPU concurrency configurations'
    mpu_slots=next(iter(slots),None)
    groups=defaultdict(list)
    for r in results:
        if r['status']=='passed':groups[group_key(r)].append(r)
    rows=[]
    for key,rs in groups.items():
        model,target,backend,m,variant,fraction,setup=key
        times=[r['block_us'] for r in rs]
        seed_set={r['seed'] for r in rs}
        baseline=[r for r in results if r['status']=='passed' and
                  (r['model'],r['target'],r['backend'],r.get('study_variant','main'))==(model,target,'pcie','main')
                  and r['seed'] in seed_set]
        assert len(baseline)==len(seed_set) and {r['seed'] for r in baseline}==seed_set
        baseline_setup={r.get('pcie_setup_ns',0) for r in baseline}
        assert len(baseline_setup)==1
        if variant!='dma-latency':assert baseline_setup=={setup}
        base=statistics.mean(r['block_us'] for r in baseline)
        volume=rs[0]['offload_bytes'];total=sum(volume.values())
        mpu_bytes=sum(v for k,v in volume.items() if k.endswith('_mpu'))
        rows.append(dict(model=model,target=target,backend=backend,mpus=m,variant=variant,
                         mpu_fraction=fraction,pcie_setup_ns=None if rs[0].get('pcie_profile') else setup,
                         transport_model='transactional' if rs[0].get('pcie_profile') else 'legacy',seeds=len(rs),
                         hbm_policy='capped-max-min' if rs[0].get('hbm_profile') else 'fifo',
                         io_transport='AXI' if rs[0].get('io_die') else 'message',mpu_max_inflight=mpu_slots,
                         time_resolution_ns=precision,interval_ns=interval,visibility_ns=visibility,io_event_driven=event_driven,
                         mean_block_us=statistics.mean(times),min_block_us=min(times),max_block_us=max(times),
                         PCIe_baseline_us=base,PCIe_baseline_setup_ns=None if rs[0].get('pcie_profile') else next(iter(baseline_setup)),speedup_vs_PCIe=base/statistics.mean(times),
                         latency_change_pct=(statistics.mean(times)/base-1)*100,
                         actual_MPU_payload_fraction=mpu_bytes/total if total else 0,
                         HBM_freed_GB=rs[0]['capacity']['HBM_freed_GB'],
                         MPU_dataset_GB=rs[0]['capacity']['MPU_dataset_GB'],
                         host_unique_dataset_GB=rs[0]['capacity']['host_unique_dataset_GB']))
    return sorted(rows,key=lambda r:(r['model'],r['target'],r['variant'],r['mpus'],r['backend'],r['mpu_fraction']))


def plots(root,rows):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    workloads=sorted({(r['model'],r['target']) for r in rows})
    fig,axes=plt.subplots(2,3,figsize=(16,9),layout='constrained')
    plot_data=[]
    for ax,(model,target) in zip(axes.flat,workloads):
        for backend,color,marker in [('mpu','#b5453c','o'),('hybrid','#137c95','s'),('tiered','#9162ad','^')]:
            rs=sorted([r for r in rows if (r['model'],r['target'],r['backend'],r['variant'])==(model,target,backend,'main')],key=lambda r:r['mpus'])
            if not rs:continue
            x=[r['mpus'] for r in rs];y=[r['speedup_vs_PCIe'] for r in rs]
            lo=[r['PCIe_baseline_us']/r['max_block_us'] for r in rs]
            hi=[r['PCIe_baseline_us']/r['min_block_us'] for r in rs]
            label={'mpu':'MPU only','hybrid':'hybrid: nominal share','tiered':'Engram MPU + KV PCIe'}[backend]
            plot_data.append(dict(model=model,target=target,label=label,x=x,y=y,lo=lo,hi=hi))
            ax.plot(x,y,label=label,color=color,marker=marker)
            ax.fill_between(x,lo,hi,color=color,alpha=.12)
        confirmed=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'share-confirmation')]
        if confirmed:
            confirmed.sort(key=lambda r:r['mpus'])
            x=[r['mpus'] for r in confirmed]
            plot_data.append(dict(model=model,target=target,label='hybrid: confirmed share',x=x,
                y=[r['speedup_vs_PCIe'] for r in confirmed],fractions=[r['mpu_fraction'] for r in confirmed],
                lo=[r['PCIe_baseline_us']/r['max_block_us'] for r in confirmed],hi=[r['PCIe_baseline_us']/r['min_block_us'] for r in confirmed]))
            ax.plot(x,[r['speedup_vs_PCIe'] for r in confirmed],marker='*',markersize=10,color='#238442',
                    label='hybrid: confirmed share',zorder=5)
            ax.fill_between(x,[r['PCIe_baseline_us']/r['max_block_us'] for r in confirmed],
                            [r['PCIe_baseline_us']/r['min_block_us'] for r in confirmed],color='#238442',alpha=.12)
        ax.axhline(1,color='#333',ls='--',lw=1,label='PCIe baseline')
        ax.set_xscale('log',base=2);ax.set_xticks([1,2,4,8,16],labels=['1','2','4','8','16'])
        ax.set_xlabel('MPU count');ax.set_ylabel('PCIe time / strategy time')
        ax.set_title(SHORT[model]+' / '+TARGET[target]);ax.grid(alpha=.2)
    axes.flat[-1].axis('off')
    legend={}
    for ax in axes.flat[:-1]:
        handles,labels=ax.get_legend_handles_labels()
        legend.update(zip(labels,handles))
    axes.flat[-1].legend(legend.values(),legend.keys(),loc='upper left',frameon=False)
    transactional=rows[0].get('transport_model')=='transactional'
    setup=(rows[0]['PCIe_baseline_setup_ns'] or 0)/1000
    timing='Transactional PCIe + shared DDR5' if transactional else f'PCIe setup: {setup:g} us per request'
    if rows[0].get('hbm_policy')=='capped-max-min':timing+='\nCapped max-min HBM sharing'
    if rows[0].get('io_transport')=='AXI':
        precision='1 ns' if rows[0]['time_resolution_ns'] else '1 ps'
        wakeup='event wakeup' if rows[0]['io_event_driven'] else 'polling'
        timing+=f'\n{precision} precision; {wakeup}\n{rows[0]["interval_ns"]} ns issue / {rows[0]["visibility_ns"]} ns notification\nBest-effort packing; {rows[0]["mpu_max_inflight"]} AXI slots\n256 B requests; 200 ns MPU latency'
    axes.flat[-1].text(0,.54,f'16 XPU; 128 GB/s PCIe per chip\n{timing}\n190 GB/s per shared MPU\nB64; 32K; 2 representative blocks\nOther curves: 3 seeds; min-max band\nGreen: held-out seeds 2/3\n>1 means faster than PCIe\nNot full-model TPOT',va='top',fontsize=9.5)
    fig.suptitle('KV / Engram offload: coupled compute, memory and network DAG',fontsize=15)
    dump(root/'plot-data.json',plot_data)
    fig.savefig(root/'offload-speedup.svg');fig.savefig(root/'offload-speedup.png',dpi=160);plt.close(fig)
    return True


def html_table(rows, columns):
    def text(v):return f'{v:.3f}' if isinstance(v,float) else str(v)
    return '<div class="scroll"><table><thead><tr>'+''.join('<th>'+html.escape(label)+'</th>' for key,label in columns)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+html.escape(text(r[key]))+'</td>' for key,label in columns)+'</tr>' for r in rows)+'</tbody></table></div>'


def main():
    parser=argparse.ArgumentParser();parser.add_argument('root',type=Path);parser.add_argument('--skip-audit',action='store_true');parser.add_argument('--audit-workers',type=int,default=1)
    args=parser.parse_args();root=args.root.resolve()
    if (root.parent/'hardware-profile.json').exists():
        (root/'hardware-profile.json').write_bytes((root.parent/'hardware-profile.json').read_bytes())
    results=json.loads((root/'results.json').read_text())
    fair=any(r.get('hbm_profile') for r in results)
    if fair:assert all(r.get('hbm_profile')==results[0]['hbm_profile'] for r in results)
    transactional=any(r.get('pcie_profile') for r in results)
    if transactional:
        assert all(r.get('pcie_profile')==results[0]['pcie_profile'] for r in results)
    verification=json.loads((root/'verification.json').read_text()) if args.skip_audit else (parallel_audit(root,results,args.audit_workers) if args.audit_workers>1 else audit(root,results))
    rows=aggregate(results);csvwrite(root/'summary.csv',rows);dump(root/'summary.json',rows)
    plotted=plots(root,rows)
    main_rows=[r for r in rows if r['variant']=='main']
    comparison=[]
    for model,target in sorted({(r['model'],r['target']) for r in rows}):
        lookup={(r['backend'],r['mpus']):r for r in main_rows if (r['model'],r['target'])==(model,target)}
        for m in (1,2,4,8,16):
            a,b=lookup.get(('mpu',m)),lookup.get(('hybrid',m))
            if a and b:
                comparison.append(dict(model=model,target=target,mpus=m,PCIe_us=a['PCIe_baseline_us'],
                                       MPU_us=a['mean_block_us'],MPU_speedup=a['speedup_vs_PCIe'],
                                       hybrid_us=b['mean_block_us'],hybrid_speedup=b['speedup_vs_PCIe'],
                                       hybrid_MPU_traffic_pct=b['actual_MPU_payload_fraction']*100))
    csvwrite(root/'main-comparison.csv',comparison)
    report='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>MPU / PCIe 卸载扫描</title><style>body{font:16px/1.7 system-ui,sans-serif;max-width:1200px;margin:32px auto;padding:0 20px;color:#1b2c3a}h1,h2{line-height:1.3}table{border-collapse:collapse;font-size:14px;width:100%;margin:16px 0}th,td{padding:8px;border:1px solid #d4dfe5;text-align:right}th{background:#edf4f7;white-space:nowrap}.scroll{overflow:auto}pre{white-space:pre-wrap;overflow-wrap:anywhere;max-width:100%;overflow:auto}img{max-width:100%;height:auto}.scope{padding:16px;background:#fff3d7;border-left:4px solid #b9821c}a{color:#096c8c}select{font:inherit;padding:5px}section[data-workload]{margin-top:32px}</style><h1>逐步增加 MPU：KV / Engram / PCIe / 混合卸载</h1>'''
    report+=f'<p>已登记 {verification["planned"]} 条件；通过 {verification["passed"]}，未通过 {len(verification["failed"])}；核验 {verification["checked_tasks"]:,} 个任务。</p>'
    setup_values={r['pcie_setup_ns'] for r in main_rows}
    assert len(setup_values)==1
    setup_ns=next(iter(setup_values)) or 0
    setup_us=setup_ns/1000
    report+=f'<p><strong>本轮统一 PCIe 固定启动延时：{setup_us:g} µs/请求。</strong>纯PCIe、混合和按数据类型分工方案使用同一参数。启动等待发生在进入链路队列之前，不占用链路，可与其他请求重叠；它不等于每个代表块固定增加1µs。</p>'
    pure=[r for r in main_rows if r['backend']=='mpu']
    wins=[r for r in pure if r['speedup_vs_PCIe']>1]
    report+=f'<p><strong>本轮结果：</strong>纯MPU的 {len(pure)} 组工作负载/节点数配置中，{len(wins)} 组快于对应PCIe基线。各模型和混合比例的结果见下表；收益由仿真完成时间计算。</p>'
    report+='<h2>PCIe 与 MPU 延时建模对比</h2>'
    latency_model=[
        dict(component='固定启动延时',pcie=f'所有方案统一 {setup_us:g} µs/请求；排队前等待，不占链路',mpu='内存服务每批200 ns，还要加网络事务延时；Engram每16行一批'),
        dict(component='数据服务带宽',pcie='每个XPU独立单向128 GB/s',mpu='每个MPU单向190 GB/s，本轮效率设为1'),
        dict(component='排队范围',pcie='同一XPU的4个Die共用一个PCIe FIFO；不同XPU可并行',mpu='同一MPU的所有内存请求共用一个FIFO；不同MPU可并行'),
        dict(component='Host内存读取',pcie='未计入DDR随机访问延时和带宽争用',mpu='显式计入MPU内存服务'),
        dict(component='数据到达后',pcie='写入HBM，再由计算侧读取',mpu='同样写入HBM，再由计算侧读取')]
    report+=html_table(latency_model,[('component','延时组成'),('pcie','PCIe路径'),('mpu','MPU路径')])
    report+='''<p class="scope"><strong>结果口径：带计算、HBM、TP/EP 和卸载依赖的代表块，从开始到最终完成均由仿真器运行。</strong>连续两个步骤的总窗口除以2。没有完整逐层 attention QK/AV、KV 新增写回或 prefill，因此不是完整模型 TPOT/TTFT。MPU 网络是 SUE 逐包仿真；PCIe 是独立每芯片的事件队列，未实现 PCIe TLP/PHY。</p>
<p>固定16颗XPU，每颗4Die、PCIe单向128GB/s；同芯片4Die竞争同一PCIe队列，不同芯片并行。每MPU单向190GB/s、512GB，无效率折扣；全部访问者竞争该节点服务。MPU数量1/2/4/8/16。固定八轨、八台256口交换机，是明确的hw-N-M实验拓扑。此处增加MPU意味着增加硬件节点和接入端口，绝非将池带宽分配给每颗XPU。</p>
<p>batch64、上下文32768。GLM/DeepSeek固定TP4，Kimi因容量固定TP32；全部EP64。各模型内部token路由、计算量及读取字节严格配对，seed1/2/3。Kimi的KDA状态留本地，只卸载MLA KV。KV请求采用256KiB分块；各路径都计HBM写入与消费读取，热点及排队由真实任务调度产生。</p>
<p>PCIe基线假定host能本地提供所需数据，无额外host DDR瓶颈，DMA启动延时按上表统一设置。Engram在多个物理host上的复制容量和host分组没有给定，因此不推断节省多少host内存。所有容量指标仅是模型数据集的唯一字节数。</p>
<p>混合方案是互不重复的数据分片：MPU和PCIe请求并行、消费者等待两边完成。默认MPU比例190M/(16×128+190M)，图例 nominal share 指这个按标称带宽计算的比例。图例 confirmed share 指用seed1从固定网格选择、再用seed2/3验证的比例，验证不保证加速。另扫描8MPU下12.5%/25%/50%/75%的分流，并测试“Engram放MPU、KV放PCIe”。</p>'''
    report+='<p>页与Engram行按合成请求的字节/行计数分配，没有真实键地址、文本热度、缓存或预取trace。容量按相同比例估算。所有卸载数据集均不足512GB，增加MPU主要是增加并行服务资源，同时增加硬件与闲置容量；本轮未做价格评估。</p>'
    confirmed_rows=[r for r in rows if r['variant']=='share-confirmation']
    if confirmed_rows:
        report+='<h2>MPU＋PCIe：各节点数分别选比例，以独立种子确认</h2><p>seed1选择比例，seed2/3确认；下表双方使用相同确认种子。负值的延迟变化代表加速。</p>'
        report+=html_table(confirmed_rows,[('model','模型'),('target','卸载内容'),('mpus','MPU数'),('mpu_fraction','MPU比例'),('PCIe_baseline_us','PCIe µs'),('mean_block_us','混合 µs'),('latency_change_pct','延迟变化%')])
        report+='<p>'+''.join(
            f'{SHORT[r["model"]]} {TARGET[r["target"]]}：MPU分流{r["mpu_fraction"]*100:g}%，相对同种子PCIe延迟'
            f'{"降低" if r["latency_change_pct"]<0 else "增加"}{abs(r["latency_change_pct"]):.2f}%。'
            for r in confirmed_rows)+'</p>'
    if plotted:
        embedded=base64.b64encode((root/'offload-speedup.svg').read_bytes()).decode('ascii')
        report+='<p><a href="offload-speedup.svg">下载SVG</a> · <a href="offload-speedup.png">下载PNG</a></p><img src="data:image/svg+xml;base64,'+embedded+'" alt="不同MPU数量下的代表块速度比">'
    report+='<p>速度比 = PCIe基线平均耗时 / 当前方案平均耗时；<strong>大于1才是加速，小于1表示更慢</strong>。三种子的范围是重复实验范围，不是置信区间。模型之间TP和工作量不同，不能横向用这些微秒数比较模型能力。</p>'
    report+='<label>筛选工作负载：<select id="filter"><option value="all">全部</option>'
    workloads=sorted({(r['model'],r['target']) for r in rows})
    for i,(model,target) in enumerate(workloads):report+=f'<option value="w{i}">{SHORT[model]} / {TARGET[target]}</option>'
    report+='</select></label>'
    for i,(model,target) in enumerate(workloads):
        selected=[r for r in comparison if (r['model'],r['target'])==(model,target)]
        baseline=next(r for r in main_rows if (r['model'],r['target'],r['backend'])==(model,target,'pcie'))
        report+=f'<section data-workload="w{i}"><h2>{SHORT[model]}：{TARGET[target]}</h2><p>PCIe基线 {baseline["mean_block_us"]:.3f} µs/代表块；全部卸载方案释放HBM约 {baseline["HBM_freed_GB"]:.3f} GB。</p>'
        report+=html_table(selected,[('mpus','MPU数'),('MPU_us','MPU µs'),('MPU_speedup','MPU速度比'),('hybrid_us','混合 µs'),('hybrid_speedup','混合速度比'),('hybrid_MPU_traffic_pct','混合MPU载荷%')])
        tier=[r for r in main_rows if (r['model'],r['target'],r['backend'])==(model,target,'tiered')]
        if tier:
            report+='<p>按数据类型分工：Engram放MPU，KV放PCIe。</p>'+html_table(tier,[('mpus','MPU数'),('mean_block_us','µs/代表块'),('speedup_vs_PCIe','相对全PCIe速度比')])
        sensitivity=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'share-sensitivity')]
        report+='<details><summary>各MPU数量固定分流比例扫描（仅seed1）</summary>'+html_table(sensitivity,[('mpus','MPU数'),('mpu_fraction','指定MPU比例'),('mean_block_us','µs/代表块'),('actual_MPU_payload_fraction','实际MPU载荷比例')])+'</details>'
        confirmed=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'share-confirmation')]
        if confirmed:
            report+='<p><strong>分流比例的独立种子确认：</strong>用seed1从预登记网格选最快比例，再用seed2/3确认；下面的PCIe基线也只使用seed2/3。每个节点数选定的方案都确认，无论结果是否加速。</p>'
            report+=html_table(confirmed,[('mpus','MPU数'),('mpu_fraction','MPU比例'),('PCIe_baseline_us','PCIe µs'),('mean_block_us','混合 µs'),('min_block_us','最小µs'),('max_block_us','最大µs'),('speedup_vs_PCIe','速度比')])
        latency=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'dma-latency')]
        if latency:
            report+='<details><summary>PCIe启动延迟敏感性（仅seed1；不占链路）</summary>'+html_table(latency,[('backend','方案'),('mpus','MPU数'),('pcie_setup_ns','启动延时ns'),('mean_block_us','µs/代表块')])+'</details>'
        report+='</section>'
    report+='<h2>失败记录与证据</h2><pre>'+html.escape(json.dumps(verification['failed'],ensure_ascii=False,indent=2))+'</pre>'
    report+='''<p><a href="summary.csv">全部汇总CSV</a> · <a href="main-comparison.csv">主对比CSV</a> · <a href="results.json">每例原始结果及run路径</a> · <a href="verification.json">校验记录</a> · <a href="resource-diagnostics.json">每芯片PCIe/每MPU实际载荷与队列</a> · <a href="plan.json">运行前计划</a> · <a href="design.json">完整输入矩阵</a></p>
<p>端到端性能未按总带宽相除外推。所有收益由同一工作量的最终DAG完成时间计算。混合比例在预登记网格中用seed1选择、seed2/3确认；未通过的案例保留，不纳入均值。更换实际拓扑、访问局部性、缓存、预取或host内存体系会改变结论。</p>
<script>document.querySelector('#filter').addEventListener('change',e=>{document.querySelectorAll('[data-workload]').forEach(s=>s.hidden=e.target.value!=='all'&&s.dataset.workload!==e.target.value)})</script></html>'''
    if transactional:
        start=report.index('<p><strong>本轮统一 PCIe 固定启动延时：')
        end=report.index('<p>混合方案是互不重复的数据分片：',start)
        report=report[:start]+'''<p><strong>本轮全部条件使用事务级PCIe模型。</strong>固定16 XPU、每8卡一个Host，每Host双路12通道DDR5-6400、1TB DDR。每路DDR理论614.4 GB/s，效率0.75；4卡共享一个NUMA内存和512 GB/s上行，单卡端点128 GB/s。</p>
<p>有限DMA引擎/描述符队列、MRRS/MPS读请求与Completion、tag/在途字节和流控信用、共享上行排队、DDR访问、有限接收缓冲及HBM写入背压均参与事件调度。描述符提交间隔100 ns、引擎启动200 ns。KV连续读取，Engram合成随机gather；没有PHY或实测地址trace。参数是实验假设，详见保存的pcie_profile。</p>
<p>MPU仍为SUE逐包网络＋每节点190 GB/s共享内存服务、512 GB容量。固定8轨、8台交换机，MPU数量1/2/4/8/16。B64、上下文32768、两步代表块，GLM/DeepSeek TP4、Kimi TP32、全局EP64；同种子token路由、计算及请求字节严格配对。结果为窗口除以2，不是完整模型TPOT/TTFT。Host侧Engram按NUMA复制并计容量；唯一数据集容量不等于物理Host占用。</p>'''+report[end:]
    if fair:
        report=report.replace('另扫描8MPU下12.5%/25%/50%/75%的分流', '在每个MPU数量下扫描12.5%/25%/50%/75%，并把该数量的nominal share纳入seed1候选')
        report=report.replace('三种子的范围是重复实验范围，不是置信区间。', '主曲线使用seed1/2/3，绿色确认曲线及其PCIe分子仅使用seed2/3；阴影为固定基线均值除以该组最大/最小耗时，不是置信区间。连线只连接实测配置，不表示未测节点数或连续比例也经过验证。')
        report=report.replace('<h2>PCIe 与 MPU 延时建模对比</h2>', '<h2>PCIe 与 MPU 延时建模对比</h2>')
        report=report.replace('<p><strong>本轮全部条件使用事务级PCIe模型。</strong>', '<p><strong>本轮全部条件使用事务级PCIe与HBM capped max-min共享模型。</strong>')
        report=report.replace('<p>MPU仍为SUE逐包网络', '<p>每Die HBM总预算1.875 TB/s，专家读取、公共读取、KV/Engram消费、SUE/PCIe写入按访问源类别共享；限速源用不完的份额重新分配。SUE写入源上限200 GB/s/Die，PCIe写入源上限128 GB/s/Die，PCIe链路另受每芯片共享128 GB/s限制。SUE仍收齐消息后写HBM；PCIe有限接收缓冲保留到HBM写完。不是真实DRAM bank/行时序。</p><p>MPU仍为SUE逐包网络')
    if any(r.get('io_die') for r in results):
        precision='1 ns' if rows[0]['time_resolution_ns'] else '1 ps'
        wakeup='事件唤醒' if rows[0]['io_event_driven'] else '轮询'
        note=f'<p><strong>本轮统一配置：</strong>全局{precision}精度，{wakeup}，AXI请求产生与任务通知均1 ns；XPU/MPU端口单向接入各400 ns，交换机预算300 ns。IO Die启用尽力拼包；MPU逐256 B AXI处理，每节点{rows[0]["mpu_max_inflight"]}个在途槽位，200 ns访问延迟可重叠，带宽按FIFO逐周期分配。R可在后续AR仍在途时返回，整消息收齐后才写HBM。所有主对比与确认点采用相同配置。逻辑有效载荷比例不包含AXI字段、填充和SUE控制开销。</p>'
        report=report.replace('<h2>分流策略',note+'<h2>分流策略',1) if '<h2>分流策略' in report else report.replace('<h2>',note+'<h2>',1)
    retries=json.loads((root/'plan.json').read_text()).get('execution_retries',[])
    if retries:report=report.replace('<h2>失败记录与证据</h2>','<h2>执行重试与证据</h2><p>最终矩阵全部完成；此前有'+str(len(retries))+'例触发1200秒墙钟上限，延长至7200秒补跑。未改变仿真参数或输入字节，原记录保留。</p><pre>'+html.escape(json.dumps(retries,ensure_ascii=False,indent=2))+'</pre>')
    (root/'report.html').write_text(report)
    dump(root/'report-generation.json',dict(
        inputs={n:sha(root/n) for n in ('results.json','design.json','plan.json')},
        tools={str(p):sha(p) for p in (Path(__file__),Path(hbm_config.__file__),Path(pcie_config.__file__),Path(io_die_config.__file__),Path(__file__).with_name('trace_storage.py'),Path(__file__).with_name('run.py'))},
        outputs={n:sha(root/n) for n in ('summary.json','summary.csv','main-comparison.csv','verification.json','resource-diagnostics.json','offload-speedup.svg','offload-speedup.png','plot-data.json','report.html') if (root/n).exists()}))
    print(json.dumps({'verification':{k:v for k,v in verification.items() if k!='checks'},'plots':plotted,'main_comparison':comparison},ensure_ascii=False,indent=2))


if __name__=='__main__':main()
