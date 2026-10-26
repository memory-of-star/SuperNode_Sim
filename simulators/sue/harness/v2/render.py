#!/usr/bin/env python3
"""Standalone, offline v2 report with actual packet-run data and chart guides."""
import json,csv,html,sys,shutil
from markdown_it import MarkdownIt
from report_charts import aggregate
from topology_diagrams import generate, comparison
from pathlib import Path
from collections import defaultdict,Counter
from run import HERE,ROOT,TOPOLOGIES,dump,csvread,csvwrite,sha

def render(dest):
    dest=Path(dest).resolve();raw=json.loads((dest/'results.json').read_text());results=[];details={}
    order=list(TOPOLOGIES)
    for r in sorted(raw,key=lambda r:(order.index(r['topology']) if r.get('topology') in order else 99,r.get('model',''),r.get('window',''))):
        if 'run' not in r:results.append(r);continue
        run=Path(r['run']).resolve();r=dict(r);r['key']='|'.join(r[k] for k in ('topology','model','window'))
        r['evidence']=str(run.relative_to(dest));results.append(r)
        stats=csvread(run/'task_statistics.csv') if (run/'task_statistics.csv').exists() else []
        inputs=json.loads((run/'tasks.json').read_text());labels={t['id']:t['label'] for t in inputs}
        matrix=[[0]*4 for _ in range(4)];lat=[];timeline=[];stages=defaultdict(lambda:dict(tasks=0,bytes=0,start=None,end=None))
        for row in stats:
            i=int(row['id']);n=int(row['bytes']);s=int(row['start_ps']);e=int(row['end_ps']);ready=int(row['ready_ps'])
            if n:
                matrix[int(row['src_die'])][int(row['dst_die'])]+=n
                if e>=0:lat.append((e-s)/1e6)
            timeline.append(dict(id=i,label=labels[i],src=f"{row['src_chip']}.{row['src_die']}",dst=f"{row['dst_chip']}.{row['dst_die']}",
                                 start=(s if n else ready)/1e6,end=e/1e6,hops=int(row['ucie_hops']),wait=not bool(n)))
            z=stages[labels[i]];z['tasks']+=1;z['bytes']+=n
            if s>=0:z['start']=s/1e6 if z['start'] is None else min(z['start'],s/1e6)
            if e>=0:z['end']=e/1e6 if z['end'] is None else max(z['end'],e/1e6)
        lat.sort();quantiles=[[p,lat[min(len(lat)-1,int((len(lat)-1)*p/100))]] for p in range(101)] if lat else []
        portstats=csvread(run/'port_statistics.csv') if (run/'port_statistics.csv').exists() else []
        sw=defaultdict(lambda:dict(bytes=0,max_busy_ps=0,ports=0))
        for p in portstats:
            if p['side']=='1':
                z=sw[int(p['switch'])];z['bytes']+=int(p['wire_bytes']);z['max_busy_ps']=max(z['max_busy_ps'],int(p['tx_busy_ps']));z['ports']+=1
        simps=r.get('sim_end_us',1)*1e6
        details[r['key']]=dict(die_bytes=matrix,latency_quantiles=quantiles,
             timeline=sorted(timeline,key=lambda a:(a['start'],a['id']))[:120],timeline_total=len(timeline),stages=dict(stages),
             switches=[dict(switch=k,**v,max_tx_busy_pct=v['max_busy_ps']/simps*100) for k,v in sorted(sw.items())],
             workload=json.loads((run/'experiment.json').read_text())['workload'],
             visuals=aggregate(stats,portstats,csvread(run/'token_routes.csv'),r))
    diagrams=generate(dest)
    comparison_md=comparison(results)
    comparison_html=MarkdownIt('commonmark').enable('table').render(comparison_md)
    import re
    guide=(HERE/'chart_guide.md').read_text()
    parts=re.split(r'(?m)^### ',guide)
    guide_keys=['architecture','topology','makespan','paths','cdf','heatmap','switches','timeline','cardmatrix','activity','portheat','experts']
    assert len(parts)==len(guide_keys)+1
    md=MarkdownIt('commonmark').enable('table')
    guide_html={key:md.render('### '+part) for key,part in zip(guide_keys,parts[1:])}
    guide_intro=md.render(parts[0])+('<p>每张图旁可展开“怎么测、怎么看、当前结论与边界”；'
        '说明中的数字为本次固定参考案例。<a href="report.md">下载完整文字版</a>。</p>')
    payload=dict(results=results,details=details,topologies=order,topology_diagrams=diagrams)
    passed=sum(r.get('status')=='passed' for r in results)
    verification_dir=Path(json.loads((HERE/'validation/latest.json').read_text())['directory'])
    v=json.loads((verification_dir/'verification.json').read_text())
    dump(dest/'verification.json',v);dump(dest/'report-data.json',payload)
    shutil.copy2(HERE/'v2-regression.log',dest/'v2-regression.log')
    flat=[]
    for r in results:
        flat.append({k:(json.dumps(v,ensure_ascii=False) if isinstance(v,(dict,list)) else v) for k,v in r.items() if k not in ('run','key')})
    keys=list(dict.fromkeys(k for r in flat for k in r));csvwrite(dest/'summary.csv',[{k:r.get(k,'') for k in keys} for r in flat])
    # Known failed development attempt, preserved instead of silently deleted.
    failures=[]
    for p in (HERE/'validation').glob('*/xpu18/runs/*/summary.json'):
        row=json.loads(p.read_text())
        if row['status']=='failed':failures.append(dict(reason='Original PORT_TO_VRF_MAP had a 256-entry bound; extended to 1024 before the successful matrix.',summary=str(p),evidence=str(p.parent/'stdout.log')))
    dump(dest/'development-failures.json',failures)
    totalbytes=sum(r.get('received_bytes',0) for r in results)
    features=(HERE/'features.md').read_text()
    features_html=MarkdownIt('commonmark').enable('table').render(features)
    architecture=(HERE/'architecture.svg').read_text()
    shutil.copy2(HERE/'architecture.svg',dest/'architecture.svg')
    import re
    features_html=re.sub(r'<img src="architecture.svg"[^>]*>',lambda _:architecture,features_html)
    report=f'''# SUE Scale-Up Ethernet 仿真平台 v2：新增特性与模型通信实验

{features}

{comparison_md}

## 实验结果概览

- 16种拓扑 × Kimi K3 / GLM 5.3 × prefill / decode / train，共96个逐包SUE实验；本次结果 **{passed}/{len(results)}通过**。
- 共交付 {totalbytes:,} 字节应用数据；所有成功项均验证任务收齐、DAG因果关系、源口组、目的代理Die及响应原口返回。
- v2功能验证记录见 verification.json；包含16种拓扑、错误组选路/跨池拒绝、LLR重传、UCIe限速与缓冲上限、DAG及重复运行一致性。通用任务入口的11项测试通过。

## 模型来源与实验口径

Kimi K3使用本地冻结的官方配置，revision `f831ab66814297da540d832a5235f8e904f29d06`；GLM 5.3使用本次下载的官方配置，revision `aca966e4e02791568aa6a4ced368624b3d897f42`。两者的config、来源URL与SHA-256保存在inputs目录。没有使用改名后的GLM 5.2流量。

- [Kimi K3 配置](https://huggingface.co/moonshotai/Kimi-K3/blob/f831ab66814297da540d832a5235f8e904f29d06/config.json)：896个路由专家，top-16，通信宽度3584；假设latent投影先于dispatch。
- [GLM 5.3 配置](https://huggingface.co/zai-org/GLM-5.3/blob/aca966e4e02791568aa6a4ced368624b3d897f42/config.json)：256个路由专家，top-8，通信宽度6144。

均采用BF16通信，每条专家记录额外8B元数据；这是网络输入假设，不由权重量化精度决定。生成的是单个代表MoE层的配置推导＋合成路由窗口，并非实测trace或完整模型运行。没有注意力、TP/PP/DP、优化器、共享专家计算、动态batching或GPU资源排队。

prefill为全网128 token的代表层；decode为全网64 token、连续4步，专家集合以0.75概率沿用；train为全网64 token的代表层前向＋反向4轮EP通信。固定种子1。各拓扑保持同一模型的token数、专家抽样、通信精度和网络参数，但专家/源token位置以及本地通信消除随卡数改变，不能作为完整模型部署的强扩展或硬件排名结论。

专家放置：`chip=floor(expert*N_XPU/experts), die=expert mod 4`；token放置：`chip=floor(token*N_XPU/token_count), die=token mod 4`。所有物理端口均建模，不代表短窗口能压满所有端口；活跃卡数单列。MPU没有被虚构为模型offload节点：混合拓扑中MoE仅在XPU间通信，MPU可达性由独立双向功能流量验证。

## 固定网络与计算假设

SUE外链200 Gbit/s、单向传播20ns、交换机转发预算300ns；CBFC每VC512个256B信用单元；端点每VC出口64KiB；分片2048B；哈希种子1；DAG通知20ns；每个专家端点每轮compute固定等待5000ns。实际参数见每个case快照，以上不是硬件测量值。UCIe假设、连接器lane映射、Gearbox和芯片地址限制仍需与实机校准。仿真中的576张卡全部为逻辑活跃可寻址设备，没有假装解决原文512 chip-id的硬件限制。

主实验无主动错误注入。重传是否为零仍由运行统计决定，不能预设。独立故障测试验证LLR能恢复。不会把CBFC信用检查失败次数解释成丢包或阻塞时长。

## 阅读交互图

1. **完成窗口柱状图**：横轴为µs，纵轴为拓扑；最早任务开始到最后响应完成，包含中间计算等待和依赖通知。越短只表示当前窗口越早完成，不是TTFT、TPOT或完整训练step。
2. **路径字节构成**：按请求有效载荷分成0/1/2次目的侧UCIe；百分比不是吞吐或带宽份额。不包含64B响应和LLR控制。
3. **消息时延分布**：网络任务从源开始发送到响应回源的CDF，含源队列、网络、UCIe及回程；每个消息等权，排除计算等待任务。
4. **Die通信热力图**：行源Die、列目的Die，数字为所有卡汇总的请求MiB。它表示输入通信需求，不是物理链路利用率。
5. **交换机最忙出口**：每台交换机选择发送占用时间最大的出口，除以从仿真0时刻到停止的完整时段；统计真实链路发送时间，含控制与重传、排除IFG和排队，并裁剪停止时刻尚未结束的发送。不是交换机平均利用率。
6. **任务时间线**：按开始时间取最早120项；网络任务从start到响应完成，计算等待从依赖ready到等待结束，颜色对应环跳数或计算等待。显示子集，不可用横线数量推算全网并发。

7. **卡间通信矩阵**：行源XPU、列目的XPU；超过32卡时按连续编号分组，每格为组间请求字节总量，悬停显示编号范围与MiB。颜色采用log1p缩放，色标按当前实验自适应；不跨实验直接比颜色，不表示带宽。
8. **源卡任务并发热力图**：横轴从仿真0时刻至停止，等分64桶；纵轴为源卡组。每格为该桶内未完成网络任务数的时间加权平均，由start→end区间与桶的重叠时长累加再除桶宽。含队列、传输和等确认；不含尚未start的依赖等待，不是GPU利用率或交换机队列深度。
9. **交换机端口组负载**：行交换机，列连续16个物理口。每格为已接线出口在整个仿真窗口内发送占用率的算术平均，固定0–100%色标；灰色表示无统计，面板末尾未接线端口未展示。包含协议控制流量；组平均可能掩盖单口热点，需结合最忙出口图和原始CSV。
10. **专家分配负载**：按token_routes.csv统计各专家被选择的次数，包含本地分配、decode重复步骤，每步每token每专家一条记录。展示最忙24个专家；它描述合成路由输入，不是实际计算耗时，也不会把训练反向通信额外计为新路由分配。

每图均有数据口径和解读说明。筛选器只筛选已完成实验，不会在浏览器里重新仿真。

## 证据与复现

每个case/runs目录保存experiment.json、tasks.json、traffic.csv、物理端口表、path_plan.csv、sue.conf、运行命令及哈希、退出码/耗时、stdout、task_statistics.csv、port_statistics.csv、ring_statistics.csv和summary.json。`summary.csv`汇总全部实验，`report-data.json`是交互图数据。端口wire_bytes统计开始发送的字节，沿途每条链路分别累计，含控制/重传，不是应用有效载荷。

一次开发验证曾在18卡576口档因VRF配置表边界失败，见development-failures.json。修复后重跑验收和整个矩阵；错误MPU组及跨池测试是预期失败，保留原始结果，不计入96个合法模型实验的通过率。

```bash
cd simai-scaleup-main
python3 harness/v2/run.py build
python3 harness/v2/verify.py
python3 harness/v2/run.py matrix reports/v2-repeat --workers 2
python3 harness/v2/render.py reports/v2-repeat
```

这些结果证明v2能按所列抽象执行这些拓扑与通信窗口；单种子、固定计算与未校准UCIe参数不足以给出真实模型或硬件性能保证。
'''
    lookup={(r.get('topology'),r.get('model'),r.get('window')):r for r in results}
    measured='## 本次测量结果\n\n| 拓扑 | Kimi decode µs | GLM decode µs | Kimi 经UCIe字节% | GLM 经UCIe字节% | 活跃XPU（Kimi/GLM） |\n|---|---:|---:|---:|---:|---:|\n'
    for topo in order:
        a=lookup.get((topo,'kimi-k3','decode'),{});b=lookup.get((topo,'glm-5.3','decode'),{})
        def share(r):return 100*sum(v for k,v in r.get('task_path_bytes',{}).items() if k!='0')/max(1,r.get('logical_bytes',0))
        measured+=f"| {topo} | {a.get('makespan_us')} | {b.get('makespan_us')} | {share(a):.2f} | {share(b):.2f} | {a.get('active_xpu')}/{b.get('active_xpu')} |\n"
    a=lookup.get(('xpu8','kimi-k3','decode'),{});b=lookup.get(('xpu64','kimi-k3','decode'),{})
    c=lookup.get(('xpu128-parity','kimi-k3','decode'),{});d=lookup.get(('xpu128-rotate','kimi-k3','decode'),{})
    findings=(f"固定全网token的Kimi decode窗口中，8卡为{a.get('makespan_us')}µs，64卡为{b.get('makespan_us')}µs。卡数增加改变了竞争和放置，但每个计算等待任务在全部卡数和两个模型中都固定为5µs，不随工作量缩放。收益趋缓受到该假设影响，不能据此断言真实计算扩卡收益趋缓。"
              f"同为128卡16交换机，奇偶分面为{c.get('makespan_us')}µs，分法轮转为{d.get('makespan_us')}µs；当前种子下轮转略短，不能据此否定原方案对确定性可达和接线的偏好。"
              "pure-rail跨Die流量虽然多经过1–2次UCIe，但其完成窗口没有按跳数成倍增加：固定5µs计算等待、消息并发和环链路竞争共同决定结果。"
              "576卡GLM decode仅有253张卡参与该合成窗口，Kimi有546张；专家放置和短窗口限制了活跃范围，不应宣称已测得576卡全负载容量。")
    measured+='\n'+findings+'\n\n'
    report=report.replace('## 模型来源与实验口径',measured+'## 模型来源与实验口径')
    report+='\n## 16种拓扑接线示意图\n\n每列展开一条轨，D0–D3为同一代表卡的各Die；同编号SW在多列出现时仍是同一台物理交换机。实际物理台数与占用单独列出，图中VRF采用配置中的1起编号。颜色仅区分轨，直达判据为同SW且同VRF。混合双池形态各展开一张奇/偶MPU，其余形态展开一张代表MPU。图下方展示六对跨Die的直接轨及4口组保障。全部图从实际端口映射生成。\n\n'
    for name,d in diagrams.items():
        report+=f"### {d['title']}\n\n![{d['title']}](topologies/{name}.svg)\n\n"
    report=re.sub(r'## 阅读交互图\n[\s\S]*?(?=## 证据与复现)',lambda _:guide+'\n\n',report)
    (dest/'report.md').write_text(report)
    template=(HERE/'report_template.html').read_text()
    template=template.replace('@@GUIDE_INTRO@@',guide_intro)
    for key,section in guide_html.items():
        template=template.replace('@@GUIDE_'+key+'@@',section)
    data=json.dumps(payload,ensure_ascii=False,separators=(',',':')).replace('<','\\u003c')
    template=template.replace('@@FEATURES@@',features_html).replace('@@COMPARISON@@',comparison_html).replace('@@FINDINGS@@',html.escape(findings)).replace('@@DATA@@',data).replace('@@PASSED@@',str(passed)).replace('@@TOTAL@@',str(len(results)))
    (dest/'report.html').write_text(template)
    return dest/'report.html'
if __name__=='__main__':print(render(sys.argv[1]))
