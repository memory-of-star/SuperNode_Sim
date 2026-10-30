"""Refresh chapter 8 measurements from the current complete, audited rerun."""
import csv,json,re,shutil,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
STUDY=ROOT/'results/sue/full-rerun-20261030-unified/deepseek'
COMM=ROOT/'results/sue/full-rerun-20261030-unified/communication/report'
def main():
 rows={r['study']:r for r in csv.DictReader((STUDY/'study-summary.csv').open())}
 assert json.loads((STUDY/'verification.json').read_text())['passed']==18
 metrics=json.loads((COMM/'metrics.json').read_text());m=metrics[0]
 p=HERE/'08-deepseek-16chip-case.md';s=p.read_text()
 s=re.sub(r'§8\.4–8\.6保留此前[^\n]*\n', '', s)
 s=re.sub(r'§8\.7–8\.8的KV/VC与拼包实验已按1ps[^\n]*\n', '', s)
 s=re.sub(r'MPU的64槽位如今[^\n]*', 'MPU的128槽位对应128条256 B事务；仅按200 ns延迟计算的覆盖上界为163.84 GB/s，仍低于190 GB/s有效服务带宽；实际还受数据服务、请求到达及网络依赖限制。', s)
 s=s.replace('19个对照/敏感性实验','18个对照/敏感性实验').replace('本轮实验另外采用`mpu_efficiency=0.5`，因此模型中的95GB/s是效率假设的结果，而非单/双向换算；这一假设与硬件规格分开记录。','本轮`mpu_efficiency=1.0`，模型中的单向有效服务带宽为190GB/s，不再施加0.5折扣。')
 note='本章统一采用事件唤醒、1 ps全局精度、1 ns请求产生/重试及DAG通知；XPU/MPU端口单向接入各400 ns；IO Die尽力拼包（不拼包对照除外）；MPU按256 B AXI事务流水，128个在途槽位、200 ns可重叠访问延迟、内部带宽FIFO，单向有效带宽190 GB/s（效率1.0）；HBM采用公平共享。MPU带宽×2实验已取消，其他敏感性对照保留。'
 if note not in s:s=s.replace('完整模型参数仅用于容量与解析预算。','完整模型参数仅用于容量与解析预算。\n\n'+note,1)
 labels=[('tp4-local','TP4 × DP16，Engram本地'),('tp4-mpu2','TP4 × DP16，2MPU'),('tp4-mpu4','TP4 × DP16，4MPU'),('tp1-mpu2','TP1 × DP64，2MPU'),('tp8-mpu2','TP8 × DP8，2MPU')]
 table='| 部署 | 两步窗口均值（μs） | 三种子范围（μs） | 本地全模型容量（GB） | 远端容量（GB） |\n|---|---:|---:|---:|---:|\n'
 for key,label in labels:
  r=rows[key];table+=f"| {label} | **{float(r['mean_us']):.3f}** | {float(r['min_us']):.3f}–{float(r['max_us']):.3f} | {float(r['total_local_GB']):.2f} | {float(r['remote_capacity_GB']):.2f} |\n"
 a=s.index('| 部署 |');b=s.index('\n## 8.5',a)
 t4=float(rows['tp4-mpu2']['mean_us']);local=float(rows['tp4-local']['mean_us']);t8=float(rows['tp8-mpu2']['mean_us']);m4=float(rows['tp4-mpu4']['mean_us'])
 s=s[:a]+table+f'''
当前结果支持：TP4比TP8的两步窗口短{(1-t4/t8)*100:.2f}%；2MPU相对本地Engram增加{(t4/local-1)*100:.2f}%时延，但释放202.76GB HBM；4MPU相对2MPU缩短{(1-m4/t4)*100:.2f}%。增加MPU的收益与容量投入需要分别比较，不能将有限对照称为全局最优。

实际验收目录：`results/sue/full-rerun-20261030-unified/deepseek/`，18例全部完成。本节图表由本轮重新执行的输入和统计生成。本轮统一了事件唤醒、精度、MPU并发与有效带宽，不把与旧结果的差异单独归因于某一个参数。
''' +s[b:]
 s=s.replace('每个请求：`64+96×8=832B`。','DAG记账的请求有效载荷：`64+96×8=832B`；不是当前AXI线上报文大小。')
 s=s.replace('每组发往2个MPU，16组同时访问；每个MPU每步需要服务16个请求。','每组向每个MPU读取25,344B，拆为99条256B AXI读请求；16组并发，每个MPU每步服务1,584条AXI请求。AR记录22B、R记录302B（含各自字段与记录头），尽力拼包后再加包头和SUE开销，实际线上字节见io_packets.csv和io_wire.csv。')
 s=s.replace('两个SUE事务分别等待接收完成响应，随后公共计算才能继续。','每条AR到达MPU即进入内存流水，完成后即可返回对应R，不等待整批AR到达；整组R收齐后写HBM，消费者再继续。')
 a=s.index('\n\n',s.index('## 8.6 '))+2;b=s.index('**[打开交互通信看板]',a)
 s=s[:a]+'''本节重新运行16卡、TP4×DP16、EP64、2MPU、B64、32K上下文、两步代表块，保留种子1/2/3及seed1关闭采样控制组。采用本轮§8.4相同配置；三种子时序与对应部署案例一致，seed1开关采样的任务、端口、环与资源统计逐字节一致。

'''+s[b:]
 keys=[('两步代表块窗口（μs）','window_us'),('完成的网络任务数','network_tasks'),('交换机出口最大队列快照（KiB）','max_sampled_switch_queue_KiB'),('交换机发送侧最大信用占用（%）','max_sampled_switch_credit_pct'),('最忙端点全窗口忙碌率（%）','max_host_port_average_busy_pct'),('信用不足检查次数','credit_exhausted_checks'),('重传帧','replay_frames'),('丢弃','discard'),('MPU等待事务数峰值（采样）','max_mpu_waiting_tasks'),('最忙Die的HBM忙碌率（%）','max_hbm_busy_pct'),('最忙Die的计算忙碌率（%）','max_compute_busy_pct')]
 table='| 指标 | seed1 | seed2 | seed3 |\n|---|---:|---:|---:|\n'
 for label,key in keys:table+='| '+label+' | '+' | '.join(f'{x[key]:.3f}' if isinstance(x[key],float) else str(x[key]) for x in metrics)+' |\n'
 a=s.index('| 指标 | seed1');b=s.index('\n这些数字支持',a);s=s[:a]+table+s[b:]
 figures=json.loads((COMM/'figure-data-seed1.json').read_text())
 cdf=next(x for x in figures if x['key']=='cdf')
 quant=lambda t,q:next(x for x,y in zip(t['x'],t['y']) if y>=q)
 cdf_note='；'.join(f"{t['name']} {quant(t,.5):.3f}/{quant(t,.99):.3f} μs" for t in cdf['data'])
 observations=[
 f"seed1累计交付{m['app_bytes']:,}B任务有效载荷。此矩阵保留DAG逻辑字节口径；AXI记录、尾部填充、协议和控制开销不在其中，不能当作线速字节。",
 '同一TP4组的4个Die发送相同token-expert集合的不同特征片，形成4行一组的结构；这不是四份完整激活。',
 f"seed1最忙端点全窗口忙碌率为{m['max_host_port_average_busy_pct']:.2f}%。此值包含控制帧，不代表MoE突发阶段的峰值。",
 f"seed1最热采样区间的单轨66个出口平均忙碌率为{m['max_rail_average_busy_pct']:.2f}%；端口平均会稀释单口热点。",
 f"seed1出口队列采样峰值{m['max_sampled_switch_queue_KiB']:.2f}KiB，seed3为{metrics[2]['max_sampled_switch_queue_KiB']:.2f}KiB。短队列峰值不能独自解释整个任务图的完成时长。",
 f"seed1交换机侧最大采样信用占用为{m['max_sampled_switch_credit_pct']:.2f}%；三个种子的信用不足检查、重传及丢弃均为0。当前证据不支持将窗口延迟归因于信用耗尽。",
 f'seed1的P50/P99：{cdf_note}。纵轴是聚合任务完成分布。AXI模式中AR/R与内存服务流水重叠，R聚合任务从首条R就绪计时，等待后续R的时间也包含在内；不能把它当作一次纯网络trip。',
 '一个点代表一个逻辑消息任务，可能对应多条256B AXI和多个SUE包；点数不等于报文数。',
 f"seed1各owner的token-expert分配数为{m['expert_assignments_min']}～{m['expert_assignments_max']}，平均12；跨步复用概率0.75使热点可能延续。",
 '逐跳累计包含卡内TP集合通信及MoE搬运；两个UCIe跳会计两次，不能与端到端有效载荷直接相加。',
 f"seed1最忙Die的HBM占用{m['max_hbm_busy_pct']:.2f}%，计算占用{m['max_compute_busy_pct']:.2f}%。HBM按非空服务区间并集计时，多源并发不重复累加；忙碌不代表全部带宽均已用满；两项最大值未必来自同一Die。",
 f"seed1最多{int(m['max_mpu_waiting_tasks'])}条AXI等待进入带宽服务，包含等待内存槽位及200ns访问延迟，不只是带宽FIFO排队。128是已准入的在途上限；节点外等待准入的请求可超过128。此图不能再按旧的聚合任务数量解释。"]
 matches=list(re.finditer(r'\*\*本次观察：\*\*[^\n]*',s));assert len(matches)==12
 for match,value in reversed(list(zip(matches,observations))):s=s[:match.start()]+'**本次观察：**'+value+s[match.end():]
 s=s.replace('每Die服务时间之和 / 仿真结束时间；计算和HBM是独立FIFO资源，可以重叠，排队与网络等待不算服务时间。','计算服务时间之和及HBM非空服务区间并集分别除以仿真结束时间；HBM取hbm_dies.csv的busy_ps，多个源并发不重复累加。')
 s=s.replace('MPU服务队列中的等待任务','MPU内存事务的等待数量').replace('按资源任务[start_ps,service_start_ps)区间重建每个MPU的FIFO等待数，绘制500ns采样；正在服务的任务不计入。','逐AXI请求按[submitted_ps,bandwidth_start_ps)统计，包含等待准入、200ns访问延迟及等待带宽；500ns采样，不含已经开始带宽服务的事务。')
 s=s.replace('results/sue/deepseek16-communication/','results/sue/full-rerun-20261030-unified/communication/').replace('results/sue/deepseek-16chip-case-study \\','results/sue/full-rerun-20261030-unified/deepseek \\')
 if '--hbm-profile configs/hardware/hbm/fair-share.json' not in s:s=s.replace('bin/sue run \\','bin/sue run \\\n  --io-die-profile configs/hardware/io-die/best-effort.json \\\n  --hbm-profile configs/hardware/hbm/fair-share.json \\')
 if 'SUE_HBM_PROFILE=' not in s:s=s.replace('python3 simulators/sue/harness/v2/deepseek_case_study.py \\','SUE_IO_DIE_PROFILE="$PWD/configs/hardware/io-die/best-effort.json" \\\nSUE_HBM_PROFILE="$PWD/configs/hardware/hbm/fair-share.json" \\\npython3 simulators/sue/harness/v2/deepseek_case_study.py \\')
 # The replay reconstructs the same IO profile through the registered environment.
 p.write_text(s)
 shutil.copytree(COMM,HERE/'communication',dirs_exist_ok=True)
 print('Chapter 8 refreshed')
if __name__=='__main__':main()
