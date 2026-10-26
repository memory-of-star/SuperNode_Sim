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
        assert sha(p/'tasks.json')==r['tasks_sha256']
        assert sha(p/'resources.json')==r['resources_sha256']
        tasks=json.loads((p/'tasks.json').read_text())
        resources=json.loads((p/'resources.json').read_text())
        stats={int(x['id']):x for x in csvread(p/'task_statistics.csv')}
        rss={int(x['id']):x for x in csvread(p/'resource_statistics.csv')}
        queues=defaultdict(list);busy=Counter();units=Counter();compute=Counter()
        for t in tasks:
            s=stats[t['id']];task_count+=1
            assert s['status']=='complete' and int(s['received_bytes'])==t['bytes']
            assert not int(s['invalid_frames'])
            assert all(int(s['start_ps'])>=int(stats[d]['end_ps'])+(20+t['delay_ns'])*1000 for d in t['deps'])
        assert abs((max(int(s['end_ps']) for s in stats.values())-1000000)/1e6-r['window_us'])<1e-7
        for q in resources:
            s=rss[q['id']];kind=q['kind'];t=tasks[q['id']]
            a,b=int(s['service_start_ps']),int(s['end_ps'])
            if kind==3:
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
        requests=json.loads((p/'offload-requests.json').read_text())
        payload=Counter();by_xpu=Counter();by_mpu=Counter()
        for req in requests:
            payload[req['target']+'_'+req['backend']]+=req['bytes']
            if req['backend']=='pcie':
                by_xpu[req['chip']]+=req['bytes']
                setup=r.get('pcie_setup_ns',0)
                if setup:
                    t=tasks[req['first_task']]
                    assert t['label'].endswith(':dma-setup')
                    assert t['delay_ns']==setup+(1000 if not t['deps'] else 0)
                    s=stats[t['id']]
                    if t['deps']:
                        expected=max(int(stats[d]['end_ps']) for d in t['deps'])+(20+setup)*1000
                        assert int(s['start_ps'])==expected
            else:by_mpu[req['bank']]+=req['bytes']
            assert int(stats[req['consumer_task']]['end_ps'])>=int(stats[req['first_task']]['end_ps'])
        assert dict(payload)==r['offload_bytes']
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
    dump(root/'verification.json',verification);dump(root/'resource-diagnostics.json',details)
    return verification


def aggregate(results):
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
                         mpu_fraction=fraction,pcie_setup_ns=setup,seeds=len(rs),
                         mean_block_us=statistics.mean(times),min_block_us=min(times),max_block_us=max(times),
                         PCIe_baseline_us=base,PCIe_baseline_setup_ns=next(iter(baseline_setup)),speedup_vs_PCIe=base/statistics.mean(times),
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
    fig,axes=plt.subplots(2,3,figsize=(15,8),layout='constrained')
    for ax,(model,target) in zip(axes.flat,workloads):
        for backend,color,marker in [('mpu','#b5453c','o'),('hybrid','#137c95','s'),('tiered','#9162ad','^')]:
            rs=sorted([r for r in rows if (r['model'],r['target'],r['backend'],r['variant'])==(model,target,backend,'main')],key=lambda r:r['mpus'])
            if not rs:continue
            x=[r['mpus'] for r in rs];y=[r['speedup_vs_PCIe'] for r in rs]
            lo=[r['PCIe_baseline_us']/r['max_block_us'] for r in rs]
            hi=[r['PCIe_baseline_us']/r['min_block_us'] for r in rs]
            label={'mpu':'MPU only','hybrid':'hybrid: nominal share','tiered':'Engram MPU + KV PCIe'}[backend]
            ax.plot(x,y,label=label,color=color,marker=marker)
            ax.fill_between(x,lo,hi,color=color,alpha=.12)
        confirmed=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'share-confirmation')]
        if confirmed:
            r=confirmed[0]
            ax.scatter([r['mpus']],[r['speedup_vs_PCIe']],s=110,marker='*',color='#238442',
                       label='hybrid: confirmed share',zorder=5)
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
    setup=next(r['PCIe_baseline_setup_ns'] for r in rows)/1000
    axes.flat[-1].text(0,.46,f'16 XPU; 128 GB/s PCIe per chip\nPCIe setup: {setup:g} us per request\n190 GB/s per shared MPU\nB64; 32K; 2 representative blocks\nCurves: 3 seeds, shaded min-max\nStars: share confirmed on seeds 2/3\n>1 means faster than PCIe\nNot full-model TPOT',va='top',fontsize=11)
    fig.suptitle('KV / Engram offload: coupled compute, memory and network DAG',fontsize=15)
    fig.savefig(root/'offload-speedup.svg');fig.savefig(root/'offload-speedup.png',dpi=160);plt.close(fig)
    return True


def html_table(rows, columns):
    def text(v):return f'{v:.3f}' if isinstance(v,float) else str(v)
    return '<div class="scroll"><table><thead><tr>'+''.join('<th>'+html.escape(label)+'</th>' for key,label in columns)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+html.escape(text(r[key]))+'</td>' for key,label in columns)+'</tr>' for r in rows)+'</tbody></table></div>'


def main():
    parser=argparse.ArgumentParser();parser.add_argument('root',type=Path);parser.add_argument('--skip-audit',action='store_true')
    args=parser.parse_args();root=args.root.resolve()
    results=json.loads((root/'results.json').read_text())
    verification=json.loads((root/'verification.json').read_text()) if args.skip_audit else audit(root,results)
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
    report='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>MPU / PCIe 卸载扫描</title><style>body{font:16px/1.7 system-ui,sans-serif;max-width:1200px;margin:32px auto;padding:0 20px;color:#1b2c3a}h1,h2{line-height:1.3}table{border-collapse:collapse;font-size:14px;width:100%;margin:16px 0}th,td{padding:8px;border:1px solid #d4dfe5;text-align:right}th{background:#edf4f7;white-space:nowrap}.scroll{overflow:auto}img{max-width:100%;height:auto}.scope{padding:16px;background:#fff3d7;border-left:4px solid #b9821c}a{color:#096c8c}select{font:inherit;padding:5px}section[data-workload]{margin-top:32px}</style><h1>逐步增加 MPU：KV / Engram / PCIe / 混合卸载</h1>'''
    report+=f'<p>已登记 {verification["planned"]} 条件；通过 {verification["passed"]}，未通过 {len(verification["failed"])}；核验 {verification["checked_tasks"]:,} 个任务。</p>'
    setup_values={r['pcie_setup_ns'] for r in main_rows}
    assert len(setup_values)==1
    setup_ns=next(iter(setup_values))
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
        report+='<h2>8MPU＋PCIe：选定分流比例后，以独立种子确认</h2><p>seed1选择比例，seed2/3确认；下表双方使用相同确认种子。负值的延迟变化代表加速。</p>'
        report+=html_table(confirmed_rows,[('model','模型'),('target','卸载内容'),('mpu_fraction','MPU比例'),('PCIe_baseline_us','PCIe µs'),('mean_block_us','混合 µs'),('latency_change_pct','延迟变化%')])
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
        report+='<details><summary>8MPU固定分流比例敏感性（仅seed1）</summary>'+html_table(sensitivity,[('mpu_fraction','指定MPU比例'),('mean_block_us','µs/代表块'),('actual_MPU_payload_fraction','实际MPU载荷比例')])+'</details>'
        confirmed=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'share-confirmation')]
        if confirmed:
            report+='<p><strong>分流比例的独立种子确认：</strong>用seed1从预登记网格选最快比例，再用seed2/3确认；下面的PCIe基线也只使用seed2/3。所有候选均确认，无论结果是否加速。</p>'
            report+=html_table(confirmed,[('mpus','MPU数'),('mpu_fraction','MPU比例'),('PCIe_baseline_us','PCIe µs'),('mean_block_us','混合 µs'),('min_block_us','最小µs'),('max_block_us','最大µs'),('speedup_vs_PCIe','速度比')])
        latency=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'dma-latency')]
        if latency:
            report+='<details><summary>PCIe启动延迟敏感性（仅seed1；不占链路）</summary>'+html_table(latency,[('backend','方案'),('mpus','MPU数'),('pcie_setup_ns','启动延时ns'),('mean_block_us','µs/代表块')])+'</details>'
        report+='</section>'
    report+='<h2>失败记录与证据</h2><pre>'+html.escape(json.dumps(verification['failed'],ensure_ascii=False,indent=2))+'</pre>'
    report+='''<p><a href="summary.csv">全部汇总CSV</a> · <a href="main-comparison.csv">主对比CSV</a> · <a href="results.json">每例原始结果及run路径</a> · <a href="verification.json">校验记录</a> · <a href="resource-diagnostics.json">每芯片PCIe/每MPU实际载荷与队列</a> · <a href="plan.json">运行前计划</a> · <a href="design.json">完整输入矩阵</a></p>
<p>端到端性能未按总带宽相除外推。所有收益由同一工作量的最终DAG完成时间计算。混合比例在预登记网格中用seed1选择、seed2/3确认；未通过的案例保留，不纳入均值。更换实际拓扑、访问局部性、缓存、预取或host内存体系会改变结论。</p>
<script>document.querySelector('#filter').addEventListener('change',e=>{document.querySelectorAll('[data-workload]').forEach(s=>s.hidden=e.target.value!=='all'&&s.dataset.workload!==e.target.value)})</script></html>'''
    (root/'report.html').write_text(report)
    print(json.dumps({'verification':{k:v for k,v in verification.items() if k!='checks'},'plots':plotted,'main_comparison':comparison},ensure_ascii=False,indent=2))


if __name__=='__main__':main()
