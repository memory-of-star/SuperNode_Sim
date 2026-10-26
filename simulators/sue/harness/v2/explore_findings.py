"""Evidence-backed descriptive findings; generated only after the matrix completes."""
import json,statistics
from pathlib import Path

def write(root):
    root=Path(root);raw=json.loads((root/'analysis.json').read_text());groups=raw['groups'];pairs=raw['pairs'];runs=raw['results']
    index={(r['study'],r['model'],r['variant'],r['topology']):r for r in groups}
    def g(study,model,variant,topo='xpu128-parity'):return index.get((study,model,variant,topo))
    def num(r,k,scale=1):return '无成功样本' if not r else f'{r[k]/scale:.3f}'
    def change(a,b,k='epoch_completion_us'):
        return '无成功样本' if not a or not b else f'{100*(b[k]/a[k]-1):+.2f}%'
    text='''## 本轮可以支持的结论

以下为预定义实验组的描述统计，不是硬件规格建议。每个均值使用成功运行；完整通过数和逐种子范围见下方全条件表。若跨拓扑结论涉及胜负，以同seed配对变化为准。

### 1. 拓扑排序需要附上通信分布和计算等待条件

| 模型 | token/步 | 分布 | 每端点等待ns | rotate相对parity变化均值 | 三种子范围 | rotate更短的配对数 |
|---|---:|---|---:|---:|---:|---:|
'''
    for model in ('kimi-k3','glm-5.3'):
        for tokens in (64,256):
            for profile in ('uniform','hotspot80'):
                for compute in (0,5000):
                    variant=f'n{tokens}-{profile}-c{compute}';pp=[p['delta_pct'] for p in pairs if (p['study'],p['model'],p['variant'])==('topology',model,variant)]
                    if pp:text+=f'| {model} | {tokens} | {profile} | {compute} | {statistics.mean(pp):+.2f}% | {min(pp):+.2f}～{max(pp):+.2f}% | {sum(x<0 for x in pp)}/{len(pp)} |\n'
    text+='''
这个表用于定位“同一接线对比何时稳定、何时翻转”。热专家负载和uniform不是相同的任务图；它们是有意改变的流量分布。0与5000ns对照隔离固定专家等待的影响，但任务通知、注入间隔和确认仍存在；不能把两者时间差解释成全部GPU计算时间。只有3个种子，不能宣称覆盖全部业务分布。

### 2. Die亲和摆位必须同时核对绕行、完成窗口和字节变化

community80下，expert chip与逻辑专家选择不变，仅置换Die；下表为零计算等待。经UCIe比例为请求字节加权，完成时间和占比都是各seed均值。

| 模型 | 拓扑 | spread→affinity 完成µs | 完成变化 | 经UCIe请求%：前→后 | 网络字节变化 |
|---|---|---:|---:|---:|---:|
'''
    for model in ('kimi-k3','glm-5.3'):
        for topo in ('xpu128-parity','xpu128-rotate'):
            a,b=[g('placement',model,v,topo) for v in ('spread-c0','affinity-c0')]
            text+=f"| {model} | {topo} | {num(a,'epoch_completion_us')}→{num(b,'epoch_completion_us')} | {change(a,b)} | {num(a,'ucie_share_pct')}→{num(b,'ucie_share_pct')} | {change(a,b,'logical_bytes')} |\n"
    text+='''
这组实验能说明给定逻辑社区下的放置敏感性，不是任意模型都可以无代价迁移专家。端点本地消除会随Die置换改变，表中单列其字节影响；GPU排队、专家权重迁移和容量成本均未建模。若亲和降低绕行却没有明显缩短窗口，应进一步看热点与DAG关键路径，不能直接宣布失败或成功。

### 3. UCIe资源曲线提供容量扫描依据，不直接给出芯片规格

固定uniform、256token/步、零计算。速率扫描固定64KiB与20ns；缓冲扫描固定400G与20ns；时延扫描固定400G与64KiB，三组分开变化。表中值为完成µs均值，带宽为每条方向环链路速率。

| 模型 | 拓扑 | 100G | 200G | 400G | 800G | 400→800G变化 |
|---|---|---:|---:|---:|---:|---:|
'''
    for model in ('kimi-k3','glm-5.3'):
        for topo in ('xpu128-parity','xpu128-rotate'):
            rr=[g('ring_rate',model,str(rate),topo) for rate in (100,200,400,800)]
            text+=f"| {model} | {topo} | "+' | '.join(num(x,'epoch_completion_us') for x in rr)+f' | {change(rr[2],rr[3])} |\n'
    text+='\n| 模型 | 拓扑 | 4KiB | 16KiB | 64KiB | 256KiB | 64→256KiB变化 |\n|---|---|---:|---:|---:|---:|---:|\n'
    for model in ('kimi-k3','glm-5.3'):
        for topo in ('xpu128-parity','xpu128-rotate'):
            rr=[g('ring_buffer',model,str(cap),topo) for cap in (4096,16384,65536,262144)]
            text+=f"| {model} | {topo} | "+' | '.join(num(x,'epoch_completion_us') for x in rr)+f' | {change(rr[2],rr[3])} |\n'
    text+='''
若较高档位继续加资源只带来很小收益，可以把它视为当前负载的候选平台区间；不能把它外推为所有负载的最低配置。本轮资源扫描未与hotspot80做全因子交叉，因此不能直接给出热专家情况下的UCIe推荐容量。

### 4. 信用窗口与分片影响需要用压力流量观察

人工incast为32个Die0源，每源256KiB，向同一卡Die1汇聚，共8MiB请求；强制组0但组内仍哈希。信用单位256B，分片与消息大小不同，CBFC算法始终开启，未做PFC/OFF对照。原congestion组自动出口缓冲随信用16/64/512联动为4/16/128KiB，不能解释为纯信用效应。下表采用追加的credit_fixed_buffer组，显式固定交换机每VC出口128KiB。

| 拓扑 | 信用单元数 | 完成µs | P99µs | 信用耗尽检查次数均值 | 环池阻塞轮询均值 |
|---|---:|---:|---:|---:|---:|
'''
    for topo in ('xpu128-parity','xpu128-rotate'):
        for credit in (16,64,512):
            a=g('credit_fixed_buffer','incast',str(credit),topo)
            text+=f"| {topo} | {credit} | {num(a,'epoch_completion_us')} | {num(a,'latency_p99_us')} | {num(a,'credit_exhausted_checks')} | {num(a,'ring_pool_blocked_polls')} |\n"
    text+='''
计数只用于确认触发了门控/重试，不能当作阻塞时长。即使增加信用改善结果，也不是禁用流控的证据。分片扫描512/2048/8192B时，固定的10ns注入间隔及协议开销同时随帧数发挥作用，因此不能归因为纯粹的MTU效应。

### 5. 错峰是否值得，要把主动等待算进去

| 拓扑 | 策略 | 共同发起基准到完成µs | 消息P99µs | 信用耗尽检查次数均值 |
|---|---|---:|---:|---:|
'''
    for topo in ('xpu128-parity','xpu128-rotate'):
        for schedule in ('burst','stagger','batched'):
            a=g('scheduling','incast',schedule,topo)
            text+=f"| {topo} | {schedule} | {num(a,'epoch_completion_us')} | {num(a,'latency_p99_us')} | {num(a,'credit_exhausted_checks')} |\n"
    text+='''
消息P99从任务start计时，通常不包含主动推迟发起的时间；主完成指标包含这些等待。只有P99下降而窗口不降时，是单消息等待与总完成时间的取舍，不能宣称整体加速。分批增加了DAG依赖，是调度策略干预，保持的是总字节与端点，不是相同DAG。

### 6. LLR成本：区分受损流与整个任务窗口

| 拓扑 | 每N个卡0入口数据帧损坏一次（0关闭） | 完成µs | 重传帧均值 | 卡0任务时延µs均值 |
|---|---:|---:|---:|---:|
'''
    for topo in ('xpu128-parity','xpu128-rotate'):
        for every in (0,100,20):
            a=g('reliability','incast',str(every),topo)
            text+=f"| {topo} | {every} | {num(a,'epoch_completion_us')} | {num(a,'replay_frames')} | {num(a,'card0_task_us')} |\n"
    text+='''
即便受损流变慢，整个窗口也可能由其他流结束时间决定。周期性损坏与随机误码不同，本轮只干预卡0接入口，不支持全网BER、链路故障切换或真实硬件可靠性预测。

### 7. 512B分片的异常边界：保留失败，暂不判断死锁

parity在512B分片的三个种子均达到60秒墙钟限制，退出码124，没有完整任务统计；rotate对应案例完成。因此这一档没有有效的拓扑性能配对，不能将超时写成完成时延，也不能用成功样本推断parity有多慢。

在主矩阵外，以seed1保持任务与物理参数不变，把仿真截止从5ms缩短至500µs，再缩短至50µs；分别仍达到60秒、30秒墙钟限制，未得到完整任务统计。两次诊断只改变截止及墙钟限制，不能据此区分事件处理过慢、重试循环或协议停滞，更不能称为真实硬件死锁。下一步应增加事件进度与队列状态采样。诊断不计入366项主实验和性能均值，原始记录位于[诊断计划](diagnostics/plan.md)及[50µs诊断](diagnostics/plan-50us.md)。

### 后续优先补测

对本轮出现的优劣翻转条件增加种子和重复负载；把UCIe资源扫描与热专家压力交叉；对亲和放置补真实计算/内存代价。MPU服务模型和GPU工作量相关计算时间仍未实现，不能由本轮结果推断offload收益或真实扩卡收益。

分析修正记录：运行辅助脚本在内存中接收到整数跳数字典键，而初版占比辅助字段只排除了字符串0，导致中间ucie_share_pct错误。报告已从冻结的task_path_bytes按整数键重新计算；不影响仿真、路径字节、完成时间或队列统计。原始中间结果及修正台账保留在runner-results-original.json与derived-statistics-correction.json。
'''
    # Keep the main decisions ahead of the detailed measurement tables.
    def pairmean(model,variant):
        return statistics.mean(p['delta_pct'] for p in pairs if (p['study'],p['model'],p['variant'])==('topology',model,variant))
    ka,kb=[g('placement','kimi-k3',v) for v in ('spread-c0','affinity-c0')]
    ga,gb=[g('placement','glm-5.3',v) for v in ('spread-c0','affinity-c0')]
    burst,batch=[g('scheduling','incast',v,'xpu128-rotate') for v in ('burst','batched')]
    summary=f'''## 先看实验发现

1. **没有始终更快的接线。** 256token/步、热点、零计算等待时，rotate相对parity的逐种子完成时间变化均值为Kimi {pairmean('kimi-k3','n256-hotspot80-c0'):+.2f}%、GLM {pairmean('glm-5.3','n256-hotspot80-c0'):+.2f}%；但GLM的64token热点条件为{pairmean('glm-5.3','n64-hotspot80-c0'):+.2f}%，方向翻转。因此选型必须带上负载规模、热点和摆位条件。
2. **亲和摆位能改变拓扑排名。** 社区流量、零计算等待下，parity的Kimi/GLM平均完成时间分别变化{change(ka,kb)} / {change(ga,gb)}；两模型的均值排名均从rotate较短变为parity较短。Kimi经UCIe请求字节比例从{num(ka,'ucie_share_pct')}%降到{num(kb,'ucie_share_pct')}%。摆位还轻微改变本地通信消除，不能完全归因于绕行减少。
3. **当前uniform负载中，加UCIe资源出现收益平台。** 400→800G时，四组模型×拓扑平均完成时间变化约−2.68%～+1.51%；64→256KiB缓冲变化约0～+0.21%。这支持继续找瓶颈，不支持认定400G/64KiB对所有业务足够。
4. **信用并非越多越快，瓶颈会转移。** 固定交换机出口128KiB后，rotate从64增到512信用，平均完成时间维持125.329µs；parity只从181.163降到176.495µs，同时环池阻塞轮询增加。轮询计数不能换算成阻塞时长。
5. **只优化P99可能牺牲总完成时间。** rotate从突发改为每批8个消息后，P99由{num(burst,'latency_p99_us')}降至{num(batch,'latency_p99_us')}µs，但整个完成窗口由{num(burst,'epoch_completion_us')}增至{num(batch,'epoch_completion_us')}µs（{change(burst,batch)}）。应把任务完成窗口与消息P99一起作为调度目标。

上述结论来自合成代表层和人工压力流量，三种子只用于观察本轮变化范围。LLR测试证明局部重传可能被其他关键流隐藏；512B/parity的3个超时案例是需要定位的仿真边界，未用于性能结论。

'''
    text=summary+text
    (root/'findings.md').write_text(text)
    return text

if __name__=='__main__':
    import sys
    write(sys.argv[1])
