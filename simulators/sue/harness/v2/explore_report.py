#!/usr/bin/env python3
"""Paired evidence analysis and offline report for the registered exploration."""
import json,csv,sys,statistics,hashlib,html,shutil,zipfile
from pathlib import Path
from collections import defaultdict,Counter
from markdown_it import MarkdownIt
from run import csvread,csvwrite,dump,sha,HERE

STUDIES={'topology':'接线 × 负载 × 计算等待','placement':'Die亲和放置','ring_rate':'UCIe带宽','ring_buffer':'UCIe共享缓冲','ring_delay':'UCIe逐跳延迟','congestion':'信用与自动出口缓冲联合扫描','credit_fixed_buffer':'固定出口缓冲的信用对照','packetization':'消息分片大小','scheduling':'突发与错峰调度','reliability':'LLR确定性错误注入'}

def analyze(root):
    root=Path(root).resolve();results=json.loads((root/'results.json').read_text());design=json.loads((root/'design.json').read_text());assert len(results)==len(design)
    paired=defaultdict(dict);groups=defaultdict(list);inputs=0;allbinary=set();semantic={};repeat_signatures={};repeat_checks=0
    corrected=0;recovered=0
    for r in results:
        if r['status']=='runner_error' and r.get('case'):
            case=Path(r['case'])
            if (case/'latest.json').exists():
                snapshot=Path(json.loads((case/'latest.json').read_text())['run'])
                if (snapshot/'summary.json').exists():
                    r['aggregation_error']=r.get('error');r.update(json.loads((snapshot/'summary.json').read_text()));recovered+=1
                    r['failure_class']='wall_timeout' if r.get('exit_code')==124 else 'simulation_failed'
        if r['status']=='passed' and r.get('logical_bytes'):
            value=100*sum(v for k,v in r.get('task_path_bytes',{}).items() if int(k)!=0)/r['logical_bytes']
            if r.get('ucie_share_pct')!=value:corrected+=1
            r['ucie_share_pct']=value
    if corrected or recovered:
        if not (root/'runner-results-original.json').exists():shutil.copy2(root/'results.json',root/'runner-results-original.json')
        dump(root/'results.json',results)
        dump(root/'derived-statistics-correction.json',dict(corrected_cases=corrected,recovered_failure_records=recovered,field='ucie_share_pct',reason='Runtime Counter keys are integers; old helper excluded string 0 only. Recomputed from immutable per-hop payload counts using int(key)!=0; simulation data unchanged.'))
    for r in results:
        if r.get('run'):
            path=Path(r['run']);r['evidence']=str(path.relative_to(root));man=json.loads((path/'command-manifest.json').read_text());allbinary.add(man['binary_sha256'])
            for f,digest in man['inputs'].items():assert sha(path/f)==digest;inputs+=1
        if r['status']=='passed':
            assert r['completed']==r['tasks'] and r['received_bytes']==r['logical_bytes'] and not r['failures']
            spec=json.loads((Path(r['run'])/'experiment.json').read_text())
            signature=(r['topology'],r['task_sha256'],json.dumps(spec['settings'],sort_keys=True),sha(Path(r['run'])/'sue.conf'))
            values=tuple(r[k] for k in ['epoch_completion_us','latency_p99_us','ring_mean_queue_ns','ring_peak_bytes','replay_frames','credit_exhausted_checks'])
            if signature in repeat_signatures:
                assert values==repeat_signatures[signature],('nonrepeatable control',r['study'],r['variant'])
                repeat_checks+=1
            else:repeat_signatures[signature]=values
            ts=csvread(Path(r['run'])/'task_statistics.csv')
            assert min(int(t['start_ps']) for t in ts if int(t['bytes']))==1000000,'shared start epoch changed'
            paired[r['study'],r['model'],r['variant'],r['seed']][r['topology']]=r
            groups[r['study'],r['model'],r['variant'],r['topology']].append(r)
            if r['study']=='placement':
                routes=csvread(Path(r['run'])/'token_routes.csv')
                key=(r['model'],r['seed'],r['compute'],r['topology'])
                identity=[tuple(x[k] for k in ['step','token','expert','src_chip','src_die','dst_chip']) for x in routes]
                if key in semantic:assert identity==semantic[key]
                else:semantic[key]=identity
    assert len(allbinary)==1
    pairs=[]
    for (study,model,variant,seed),pair in paired.items():
        if len(pair)!=2:continue
        a,b=pair['xpu128-parity'],pair['xpu128-rotate'];assert a['task_sha256']==b['task_sha256'];assert a['routes_sha256']==b['routes_sha256']
        pairs.append(dict(study=study,model=model,variant=variant,seed=seed,parity_us=a['epoch_completion_us'],rotate_us=b['epoch_completion_us'],
            delta_pct=100*(b['epoch_completion_us']/a['epoch_completion_us']-1),delta_us=b['epoch_completion_us']-a['epoch_completion_us']))
    metrics=['epoch_completion_us','makespan_us','latency_p99_us','ring_mean_queue_ns','ring_peak_bytes','ring_pool_blocked_polls','ucie_share_pct','max_port_busy_pct','replay_frames','credit_exhausted_checks','queue_blocked_polls','logical_bytes','card0_task_us']
    aggregate=[]
    for key,rows in groups.items():
        z=dict(zip(('study','model','variant','topology'),key));z['n']=len(rows)
        for metric in metrics:
            vals=[r[metric] for r in rows];z[metric]=statistics.mean(vals);z[metric+'_min']=min(vals);z[metric+'_max']=max(vals)
        aggregate.append(z)
    verification=dict(status='passed',planned=len(design),executed=len(results),passed=sum(r['status']=='passed' for r in results),failures=sum(r['status']!='passed' for r in results),input_hashes_checked=inputs,complete_pairs=len(pairs),binary_versions=len(allbinary),identical_control_repeats=repeat_checks,checks=['all planned cases accounted for','all input SHA256','successful complete delivery and DAG analyzer','identical topology-pair tasks/routes','common 1us network release epoch','placement preserves logical expert identities and expert chip','identical controls repeat exactly on selected performance counters'])
    dump(root/'verification.json',verification);dump(root/'paired-results.json',pairs);dump(root/'analysis.json',dict(results=results,groups=aggregate,pairs=pairs,studies=STUDIES))
    fields=['study','model','variant','seed','topology','status']+metrics+['run']
    csvwrite(root/'summary.csv',[{k:r.get(k,'') for k in fields} for r in sorted(results,key=lambda r:(r['study'],r['model'],r['variant'],r['seed'],r['topology']))]);csvwrite(root/'paired-results.csv',pairs)
    return results,aggregate,pairs,verification


def make_report(root,analyzed=None):
    root=Path(root).resolve();results,groups,pairs,v=analyzed if analyzed is not None else analyze(root)
    lookup={(r['study'],r['model'],r['variant'],r['topology']):r for r in groups}
    def get(study,variant,model='kimi-k3',topo='xpu128-parity'):return lookup.get((study,model,variant,topo))
    # Full descriptive evidence table; no unsupported significance claims.
    sections=[]
    for study,title in STUDIES.items():
        text=f'## {title}\n\n'
        text+='| 模型 | 条件 | parity µs（均值；min–max） | rotate µs（均值；min–max） | 配对变化%均值（min–max） | 有效配对 |\n|---|---|---:|---:|---:|---:|\n'
        keys=sorted({(r['model'],r['variant']) for r in results if r['study']==study})
        for model,variant in keys:
            a,b=get(study,variant,model),get(study,variant,model,'xpu128-rotate');pp=[p for p in pairs if (p['study'],p['model'],p['variant'])==(study,model,variant)]
            def cell(z):return '无成功样本' if not z else f"{z['epoch_completion_us']:.3f}（{z['epoch_completion_us_min']:.3f}–{z['epoch_completion_us_max']:.3f}；n={z['n']}）"
            changes=[p['delta_pct'] for p in pp];value=f'{statistics.mean(changes):+.2f}（{min(changes):+.2f}–{max(changes):+.2f}）' if changes else '无有效配对'
            text+=f'| {model} | {variant} | {cell(a)} | {cell(b)} | {value} | {len(pp)}/3 |\n'
        sections.append(text)
    intro=f'''# SUE v2：受控探索实验报告

设计并执行 {v['executed']} 项实验，成功 {v['passed']}，失败/未完成 {v['failures']}；完整拓扑配对 {v['complete_pairs']} 组。初始348项设计与后续18项追加对照见[plan.md](plan.md)，追加原因见[credit-addendum-plan.md](credit-addendum-plan.md)，逐项指标见[summary.csv](summary.csv)，配对原始差值见[paired-results.csv](paired-results.csv)，校验见[verification.json](verification.json)。原来的96项实验保留不变。

## 先看比较口径

本次全部采用128卡，固定外链200G和SUE协议实现。Kimi/GLM为decode四步的合成代表层，非完整模型；人工incast用于压力与机制验证。主结论以种子1/2/3配对比较；每个条件的min–max只反映三个种子的范围，不是95%置信区间。种子同时改变模型路由与口哈希，不能单独识别两者贡献。

主完成指标为最后任务完成减共同的1µs网络发起基准，包含错峰或分批人为引入的等待，避免通过延迟发流伪造加速；原始makespan_us同时保留。0ns计算是网络敏感性对照，仍包含DAG通知、发流间隔和确认回程；5000ns是每端点固定等待，均不模拟GPU算力或计算资源排队。

hotspot80表示每token约80%路由槽位选前32专家：Kimi为13/16，GLM为6/8，比例并不完全相同，不能直接当作两模型等强度热点。community80用固定逻辑专家社区；affinity仅将专家Die位置按0/1/2/3→0/2/1/3置换，chip和逻辑专家选择不变，但本地通信消除可能变化。无GPU资源模型，不能据此声称亲和摆位没有计算热点代价。

配对变化定义为100×(rotate/parity−1)：负值是rotate较短，正值是parity较短。失败不计性能均值；缺失配对明确标记，不把仿真截止或墙钟超时当成完成。每次运行保存独立输入、二进制/库哈希、统计与失败台账。

## 图怎么读

1. **完成时间对比图**：分类条件用双柱，数值扫描用曲线；每个条件显示成功种子的均值和min–max，详表标明样本数。对数横轴在图下明确标注；连线只连接已测点，不是拟合或硬件速度排名。
2. **逐种子配对差值图**：每个条件的三个点从上至下为seed1/2/3，对应同一输入与种子在两种拓扑的差值百分比，零线两侧表示胜负反转。不能只看均值掩盖种子差异。
3. **环链路平均排队图**：每例先用ring_statistics.csv累计排队ps除以累计帧跳数再转ns，然后跨种子平均；含请求及确认。无环流量为0，不能误读为UCIe无限快。
4. **证据表**：P99是任务start到确认返回；端口占用是整段0→停止的最忙交换机出口发送占用；信用检查/队列轮询是事件次数，不是时间或丢包数；共享环池计数按卡去重，不重复累加8条环边。

下方各实验表给出每个条件的完整描述统计。定性结论另见结论小节；LLR注错限定在源卡0接入交换机的入口，badEvery是周期性数据帧损坏，不是全网随机BER。

## 实验设计与条件标签

| 问题 | 改变什么 | 其余关键控制 |
|---|---|---|
| 接线遇到什么负载更合适 | 64/256 token、uniform/hotspot80、0/5000ns等待 | 同seed两拓扑使用完全一致的任务及逻辑路由 |
| 专家摆位是否有价值 | spread/affinity、0/5000ns等待 | community80、256token，专家chip和逻辑选择不变 |
| 环资源何时成为瓶颈 | 100/200/400/800G；4/16/64/256KiB；20/100/500ns | 三项单独扫描；其余固定400G、64KiB、20ns，uniform256、计算0 |
| 信用与分片如何影响压力流量 | 16/64/512信用；512/2048/8192B分片 | 32源向一目标汇聚；追加信用对照固定每VC出口128KiB |
| 如何平滑发送 | 同时突发、相邻源错开2µs、每批8源 | 32源×256KiB；分批等待上一批全部确认 |
| 局部损坏会不会拖慢任务 | 关闭/每100/每20数据帧损坏 | 只注入卡0接入口；其他流量和LLR算法不变 |

例如`n256-hotspot80-c5000`表示每步256token、热点路由、每端点5000ns固定计算等待；`affinity-c0`表示亲和摆位、零计算等待。资源条件的数字单位依次为Gbit/s、B、ns；信用条件为256B信用单元数；分片条件为有效载荷B。所有主实验种子为1/2/3。调度与可靠性使用人工incast，不代表Kimi或GLM完整执行。
'''
    findings=(root/'findings.md').read_text() if (root/'findings.md').exists() else '## 结论整理中\n\n全部数据和配对统计见下表。\n'
    report=intro+'\n'+findings+'\n'+'\n'.join(sections)+'\n## 失败台账\n\n'
    failures=[r for r in results if r['status']!='passed']
    report+='无失败。\n' if not failures else '\n'.join(f"- {r['study']} / {r['model']} / {r['variant']} / seed{r['seed']} / {r['topology']}：{r.get('failure_class',r['status'])}，exit={r.get('exit_code')}，wall={r.get('wall_s')}s，{r.get('failures',r.get('error'))}；[执行记录]({r.get('evidence','cases')}/execution.json)" for r in failures)+'\n'
    (root/'report.md').write_text(report)
    md=MarkdownIt('commonmark').enable('table');payload=json.loads((root/'analysis.json').read_text())
    template=(HERE/'explore_template.html').read_text().replace('@@INTRO@@',md.render(intro)).replace('@@FINDINGS@@',md.render(findings)).replace('@@TABLES@@',md.render('\n'.join(sections)+'\n## 失败台账'+report.split('## 失败台账')[1]))
    template=template.replace('@@SCRIPT@@',(HERE/'explore_visuals.js').read_text())
    template=template.replace('@@DATA@@',json.dumps(payload,ensure_ascii=False,separators=(',',':')).replace('<','\\u003c'))
    (root/'report.html').write_text(template)
    print(json.dumps(v,ensure_ascii=False,indent=2))

if __name__=='__main__':
    from explore_findings import write
    analyzed=analyze(sys.argv[1]);write(sys.argv[1]);make_report(sys.argv[1],analyzed)
