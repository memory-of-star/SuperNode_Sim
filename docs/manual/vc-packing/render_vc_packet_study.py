"""Render measured chapter-8 additions into the existing manual, with CSV evidence."""
import argparse
import json
import shutil
import statistics
import zipfile
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties
from run import csvread, dump

B=('MoE','KV','Engram');COLORS=['#177da5','#df8a32','#7659ad'];MODES=('best_effort','none')
FONT=FontProperties(fname='/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
plt.rcParams.update({'font.family':FONT.get_name(),'axes.unicode_minus':False,'svg.fonttype':'none','font.size':10})


def save(fig,path):
    fig.savefig(path.with_suffix('.svg'),bbox_inches='tight')
    fig.savefig(path.with_suffix('.png'),dpi=150,bbox_inches='tight');plt.close(fig)


def table(headers,rows):
    return '\n'.join(['| '+' | '.join(headers)+' |','|'+'---|'*len(headers)]+['| '+' | '.join(str(c) for c in row)+' |' for row in rows])+'\n\n'


def main():
    p=argparse.ArgumentParser();p.add_argument('study',type=Path);p.add_argument('manual',type=Path);a=p.parse_args()
    study=a.study.resolve();source=study/'analysis';dest=a.manual/'vc-packing';dest.mkdir(exist_ok=True)
    for f in source.rglob('*'):
        if f.is_file():
            out=dest/f.relative_to(source);out.parent.mkdir(exist_ok=True,parents=True);shutil.copy2(f,out)
    for name in ('registered-plan.json','verification.json'):
        shutil.copy2(study/name,dest/('run-'+name if name=='verification.json' else name))
    for name in ('vc_packet_study.py','analyze_vc_packet_study.py','render_vc_packet_study.py'):
        shutil.copy2(Path(__file__).parent/name,dest/name)
    metrics=json.loads((source/'metrics.json').read_text())
    assert len(metrics)==6 and all(m['time_resolution_ns']==0 for m in metrics),'This update requires a complete ps study'
    verification=json.loads((study/'verification.json').read_text())
    assert verification['status']=='passed' and verification['time_resolution_ns']==0
    audit=json.loads((source/'seed1-none/line-time-audit.json').read_text())
    fabric_gaps=next(r for r in audit['short_frame_gaps'] if r['side']==1)
    by={(m['seed'],m['mode']):m for m in metrics};packed=by[1,'best_effort'];unpacked=by[1,'none']
    # Three denominators deliberately displayed independently.
    fig,axes=plt.subplots(1,3,figsize=(12,3.5),layout='constrained')
    for ax,field,title in zip(axes,('logical_data_bytes','frames','consumed_credit_units'),('业务有效数据占比','交换机数据帧数量占比','累计发送信用单位占比')):
        v=np.array([x[field] for x in packed['vc']]);v=v/v.sum()*100
        ax.bar(B,v,color=COLORS);ax.set_title(title);ax.set_ylim(0,100);ax.set_ylabel('%')
        for i,x in enumerate(v):ax.text(i,x+2,f'{x:.2f}%',ha='center')
    fig.suptitle('25% KV 卸载 · 三业务分 VC · 开启拼包 · seed1')
    save(fig,dest/'01-traffic-share')
    fig,axes=plt.subplots(1,2,figsize=(12,4),layout='constrained')
    for ax,mode in zip(axes,MODES):
        hs=csvread(source/f'seed1-{mode}'/'packet-lengths.csv')
        for kind,color in zip(('AW_W','B','AR','R'),('#177da5','#47996b','#df8a32','#7659ad')):
            counts={}
            for r in hs:
                if r['kind']==kind:counts[int(r['mac_bytes'])]=counts.get(int(r['mac_bytes']),0)+int(r['frames'])
            sizes=sorted(counts);y=np.cumsum([counts[x] for x in sizes])/sum(counts.values())*100
            ax.step(sizes,y,where='post',label=kind,color=color);ax.scatter(sizes,y,s=8,color=color)
        ax.set_xscale('log');ax.set_xlim(55,10000);ax.set_ylim(0,105);ax.set_xlabel('MAC 帧长（B，不含 8B preamble）');ax.set_ylabel('帧数累计占比（%）')
        ax.set_title('尽力拼包' if mode=='best_effort' else '关闭拼包：每帧一个 AXI 记录');ax.grid(alpha=.2);ax.legend()
    save(fig,dest/'02-frame-lengths')
    fig,axes=plt.subplots(1,2,figsize=(12,4),layout='constrained')
    for ax,mode in zip(axes,MODES):
        width=.19;x=np.arange(3)
        for i,role in enumerate(('XPU-to-switch','MPU-to-switch','switch-to-XPU','switch-to-MPU')):
            peaks=[]
            for vc in range(3):
                values=[int(r['peak_credit']) for seed in (1,2,3) for r in csvread(source/f'seed{seed}-{mode}'/'vc-ports.csv') if r['role']==role and int(r['vc'])==vc]
                peaks.append(max(values,default=0))
            ax.bar(x+(i-1.5)*width,peaks,width,label=role)
        ax.set_xticks(x,B);ax.set_ylabel('每端口/VC 信用占用峰值（500ns采样）');ax.set_title('开启拼包' if mode=='best_effort' else '关闭拼包');ax.set_ylim(0,550);ax.axhline(512,color='#888',ls='--',lw=.8);ax.text(2.45,518,'上限512',ha='right',fontsize=8)
    fig.legend(*axes[0].get_legend_handles_labels(),loc='outside lower center',ncol=4,fontsize=9)
    save(fig,dest/'03-credit-peaks')
    fig,axes=plt.subplots(1,2,figsize=(12,4),layout='constrained')
    x=np.arange(8)
    for i,mode in enumerate(MODES):
        values=np.array([[m['switches'][sw]['data_mpps'] for sw in range(8)] for m in metrics if m['mode']==mode]);mean=values.mean(axis=0)
        axes[0].bar(x+(i-.5)*.36,mean,.36,yerr=[mean-values.min(axis=0),values.max(axis=0)-mean],capsize=2,label='开启拼包' if i==0 else '关闭拼包',color=COLORS[i])
        times=csvread(source/f'seed1-{mode}'/'pps-timeseries.csv')
        axes[1].plot([float(r['time_us']) for r in times],[sum(float(r[b]) for b in B) for r in times],lw=.9,label='开启拼包' if i==0 else '关闭拼包',color=COLORS[i])
    axes[0].set_xticks(x,[f'SW{i}' for i in x]);axes[0].set_ylabel('数据转发 Mpps（全窗口均值）');axes[0].set_title('三种子均值及 min–max');axes[0].legend()
    axes[1].set_xlabel('仿真时间（μs）');axes[1].set_ylabel('8 台交换机合计数据 Mpps');axes[1].set_title('seed1，固定 500ns 时间桶');axes[1].legend()
    fig.suptitle('1ps全局精度重新运行；IFG调度限制与硬件包率解释见§8.8',fontsize=11)
    save(fig,dest/'04-switch-pps')
    total_data=sum(v['logical_data_bytes'] for v in packed['vc']);total_wire=packed['wire_bytes'];total_credit=sum(v['consumed_credit_units'] for v in packed['vc'])
    shares=table(['业务 / VC','有效数据 B（占比）','线上数据帧字节 B（占比）','帧数（占比）','累计信用单位（占比）'],[
        [f"{v['business']} / VC{v['vc']}",f"{v['logical_data_bytes']:,}（{v['logical_data_bytes']/total_data:.2%}）",f"{v['wire_bytes']:,}（{v['wire_bytes']/total_wire:.2%}）",f"{v['frames']:,}（{v['frames']/packed['data_frames']:.2%}）",f"{v['consumed_credit_units']:,}（{v['consumed_credit_units']/total_credit:.2%}）"] for v in packed['vc']])
    details=table(['业务','AXI通道','单记录 B','记录数','拼包后帧数','MAC长度 min / P50 / P95 / max B','平均记录/帧'],[
        [r['business'],r['kind'],r['axi_record_bytes'],f"{r['records']:,}",f"{r['frames']:,}",f"{r['mac_min']} / {r['mac_p50']} / {r['mac_p95']} / {r['mac_max']}",f"{r['mean_records_per_frame']:.2f}"] for r in packed['packets']])
    peakrows=[]
    for vc,b in enumerate(B):
        group=[m['vc'][vc] for m in metrics if m['mode']=='best_effort']
        peakrows.append([f'{b} / VC{vc}',max(v['max_frame_credit'] for v in group),max(v['peak_credit'] for v in group),max(v['peak_switch_credit'] for v in group),f"{max(v['peak_queue_bytes'] for v in group)/1024:.2f}",sum(v['credit_exhausted'] for v in group)])
    credit_table=table(['业务 / VC','最大单帧信用','两侧最忙端口峰值','交换机侧峰值','队列峰值 KiB','信用不足检查次数'],peakrows)
    rates=table(['seed','拼包','两步窗口 μs','交换机转发帧数','8交换机合计 Mpps','合计峰值 Mpps（500ns）','最忙单口峰值 Mpps（500ns）','信用不足检查'],[
        [m['seed'],'开' if m['mode']=='best_effort' else '关',f"{m['window_us']:.3f}",f"{m['data_frames']:,}",f"{m['data_mpps']:.3f}",f"{m['aggregate_peak_500ns_mpps']:.0f}",f"{max(s['peak_port_500ns_mpps'] for s in m['switches']):.0f}",f"{m['credit_exhausted']:,}"] for m in metrics])
    swtable=table(['交换机','开拼包帧数','关拼包帧数','开拼包均值 Mpps','关拼包均值 Mpps','开/关峰值 Mpps（500ns）'],[
        [f'SW{s}',f"{packed['switches'][s]['data_frames']:,}",f"{unpacked['switches'][s]['data_frames']:,}",f"{packed['switches'][s]['data_mpps']:.3f}",f"{unpacked['switches'][s]['data_mpps']:.3f}",f"{packed['switches'][s]['peak_500ns_mpps']} / {unpacked['switches'][s]['peak_500ns_mpps']}"] for s in range(8)])
    plain=table(['AXI通道','记录长度 B','不拼包 MAC长度 B','线上长度 B','每帧信用单位'],[
        [r['kind'],r['axi_record_bytes'],r['mac_min'],r['mac_min']+8,math_ceil(r['mac_min']/256)] for r in unpacked['packets'] if r['business'] in ('MoE','KV')])
    reduction=1-packed['data_frames']/unpacked['data_frames'];wire_reduction=1-packed['wire_bytes']/unpacked['wire_bytes']
    source_run=Path(packed['run']);placement=json.loads((source_run/'placement.json').read_text())
    packedchecks=sum(m['credit_exhausted'] for m in metrics if m['mode']=='best_effort')
    plainchecks=sum(m['credit_exhausted'] for m in metrics if m['mode']=='none')
    no_credit_table=table(['业务 / VC','关拼包信用占用峰值','关拼包信用不足检查（三种子合计）'],[[f'{b} / VC{vc}',max(m['vc'][vc]['peak_credit'] for m in metrics if m['mode']=='none'),f"{sum(m['vc'][vc]['credit_exhausted'] for m in metrics if m['mode']=='none'):,}"] for vc,b in enumerate(B)])
    text=f'''<!-- BEGIN VC PACKING EXPERIMENTS -->
## 8.7 实验一：部分KV卸载与三个VC的信用需求

**本节与第8、9章其余主实验使用同一基准重新运行**：事件唤醒、1 ps全局精度、1 ns请求/依赖通知、每MPU 128条在途、单向有效带宽190 GB/s。拼包开关及VC分流是本节的对照变量。

本实验沿用§8.6的16卡、TP4×DP16、EP64、2MPU、B64、32K上下文和两步代表块；新增**25% KV静态驻留MPU、75%留在本地HBM**，按均匀访问假设取同样的读取比例。每个Die的远程读取量向下对齐256B，按256KiB块轮转分布到两个MPU。该配置释放约{placement['kv_capacity_moved_bytes']/1e9:.3f} GB完整KV驻留容量；容量与两步实际传输量分别计量。

远程KV走AR→MPU流水服务→R→本地HBM写入→HBM消费，消费者等待完成。本地75%仍计入公共HBM读取。没有PCIe KV路径；本例不包含KV新token写回、动态换入换出和预取，也不是完整模型TPOT。Engram采用本章原有集中读取/写入DAG。TP4的Attention AllReduce与Output AllGather仍走卡内UCIe，不进入下表的SUE流量。

| 业务 | VC | 承载的AXI通道 |
|---|---|---|
| MoE dispatch与combine | VC0 | AW＋W、B；combine是另一笔写事务 |
| KV读取 | VC1 | AR、R |
| Engram查表 | VC2 | AR、R |

请求和响应使用同一业务VC，IO Die不会跨VC、AXI通道或目的端拼包。VC3–7没有业务数据。各VC仍配置512个信用单位，每单位256B；本实验测量需求，没有将信用预先按流量占比重新分配。LLR/CBFC控制消息走独立控制队列，不归到某一业务VC。

### 业务占比与报文数量

以下为seed1开启拼包结果。有效数据仅累加AW＋W中的有效业务数据与R返回数据；不将AR的逻辑请求描述大小当成KV数据。线上字节包含MAC帧和8B preamble，排除IFG与独立控制消息。所有数据帧在**交换机出口只计一次**，避免两个链路重复累计。

{shares}![业务字节、报文数量与信用消耗占比](vc-packing/01-traffic-share.svg)

### 各种报文有多长、多少个

AXI记录是模型封装，包含配置的字段和8B记录头；它不是以太网帧。每个网络包另有4B封装头、17B SUE/MAC头，MAC最小填充到64B，线上再加8B preamble。实际MAC长度为`max(64, 17 + 4 + 记录数 × 单记录长度)`；当前模型没有在此之外额外计入FCS，数字应按模型口径使用。

{details}![按AXI通道区分的MAC报文长度分布](vc-packing/02-frame-lengths.svg)

完整离散长度直方图、逐业务/通道数量以及dispatch/combine分项保存在[指标JSON](vc-packing/metrics.json)与[seed1长度计数CSV](vc-packing/seed1-best_effort/packet-lengths.csv)。每个长度的出现次数都保留，没有只保留均值。

### 如何用于CBFC信用分配

当前计费公式是`max(1, ceil(MAC帧长 / 256))`，PktOvhd=0，8B preamble不计入信用。上表“累计信用”是整个窗口反复消耗并归还的总量，**不是同时在途的信用窗口**。小包每帧至少占1个单位，因此字节占比、包数占比与信用消耗占比不同。

下表取开启拼包的三个种子最大值：信用占用CU与出口队列为500ns被动采样，可能漏过更短峰值；“两侧”包含端点发送侧与交换机发送侧，每一格都是某个实际端口/VC的最大值，不是全网CU之和。信用不足检查次数来自累计计数器。

{credit_table}![每类端口和VC的信用占用峰值](vc-packing/03-credit-peaks.svg)

**KV占主要流量，但不能按总字节比例把Engram信用压得很小。** Engram的集中请求突发也需要独立窗口；其访问阶段、单帧大小和CF返回等待与平均流量占比不同。信用是逐端口、逐方向、逐VC的接收缓冲承诺：XPU侧与MPU侧应分别看[逐端口证据](vc-packing/seed1-best_effort/vc-ports.csv)，不能把8台交换机合计比例直接当成每个端口的分配比例。

开启拼包的三种子累计信用不足检查为{packedchecks}，512/VC的均分配置在这些条件下未暴露信用耗尽。关闭拼包则不同：

{no_credit_table}关拼包三种子累计发生{plainchecks:,}次信用不足检查，KV出现信用窗口耗尽，但最终全部交付，六例均无重传和丢弃。检查次数不是丢包数、阻塞包数或阻塞时长；同一帧可以被检查多次。信用需求依赖拼包模式，因此两种模式必须分别评估。可用“最大单帧信用”和“实测CU峰值”作为缩小窗口的初筛依据，并为采样漏峰和突发留余量；不能将采样峰值直接认定为安全最小值。最终分配比例仍需在固定接收缓冲总预算下扫描各VC窗口，并复核完成时间、CU及阻塞。本次提供评估输入，不宣称已经找到最优比例。

## 8.8 业务流量对交换机转发包率的需求

当前IO Die采用**发送机会驱动的尽力拼包（`best_effort`）**：将已经就绪、可以一起发送的AXI事务记录合并为一个SUE数据帧。拼包位于端点IO发送队列与SUE设备出口队列之间，XPU请求和MPU返回均使用这套规则。

1. **事务入队。** 业务数据按256 B AXI事务拆分，写请求的AW＋W作为一个完整单元，读请求AR、写响应B、读响应R各自形成记录；每个源物理端口维护一个待封装队列。
2. **按队首确定拼包类别。** 每次封装以队首记录为基准，按入队顺序扫描当前队列，只选择源物理端口、目的`chip.die`、目的侧代理Die、VC和AXI通道类别均相同的记录。扫描时跳过不兼容记录，因此可以合并队列中不相邻的兼容记录；事务ID用于响应匹配，不要求相同，也允许跨任务合并。AW＋W只与AW＋W拼，AR、B、R分别同类合并。
3. **达到载荷上限即停止。** 本例每帧适配器载荷上限为8192 B，包含4 B封装头及各条记录的字段、数据和8 B记录头。下一条兼容记录放不下时结束本帧；外层再添加SUE/MAC头，短帧补齐到64 B MAC长度。
4. **有发送机会就提交。** 不等待未来请求，也没有等待凑满的定时器；当前只有一条记录时也会发送。出口队列无法接收时保留记录并重试；提交成功后移除所选记录，按帧长度和端口线速安排下一次封装机会。已经进入设备出口队列的帧不再重新合并，实际出线仍受SUE队列和CBFC信用约束。每端口待封装队列最多4096条记录，满时向事务生产者施加背压。

例如，同一发送机会下已有10条兼容AR记录，每条22 B，合并后的适配器载荷为`4 + 10 × 22 = 224 B`，加17 B SUE/MAC头和8 B preamble后为249 B线上数据帧。关闭拼包时，这10条AR各自形成72 B线上帧，共720 B、10次包转发。实际能拼入多少条取决于发送机会到来时的队列积累，不保证每帧装满。

使用§8.7同一份三业务、三VC输入，seed1/2/3分别做尽力拼包与关闭拼包对照。成对的任务、资源工作量、专家路由、HBM配置、AXI记录数和有效数据量一致，IO配置只改变`mode`。**关闭拼包仍保留256B AXI事务和全部通道，一个记录单独成帧**；开启拼包则在发送机会到来时合并兼容的已排队记录，没有等待凑满定时器。

### 包率的定义与结果

包率只统计交换机出口实际开始发送的业务数据帧，AW＋W、B、AR、R都计入。全窗口均值为`帧数 / 仿真结束时间`，包含最初1μs初始化和计算/内存空闲区间；“两步窗口”列扣除了最初1μs。峰值使用从0开始对齐的完整500ns桶，尾部不足500ns的桶不参与峰值。合计是8台交换机相加；不是单台交换机包率。

{rates}seed1开启拼包把帧数从{unpacked['data_frames']:,}降为{packed['data_frames']:,}，减少**{reduction:.2%}**；对应线上数据帧字节减少{wire_reduction:.2%}。关闭拼包的包率更高，表示同一业务需要更多次包转发，并不表示应用完成得更快。两组任务有资源竞争和反馈，不能假定所有包的注入时刻相同。

![开启和关闭拼包时各交换机包率与突发](vc-packing/04-switch-pps.svg)

### 小包线时与帧间隔审计

**线时审计：**200Gb/s下72B线上帧为2880ps、331B为13240ps、341B为13640ps，9B控制发送为360ps；原先截断到整数ns的问题已消除。12B IFG的配置时长是480ps，但原有队列由空转非空时仍可能绕过待完成的IFG定时器。本轮seed1关闭拼包在交换机出口观察到的“72B帧结束到下一数据帧开始”最小间隔为{fabric_gaps['min_ps']}ps，其中{fabric_gaps['below_480ps']:,}个间隔短于480ps；[逐例线时审计](vc-packing/line-time-audit.json)保存原始计数。**这些结果仍受该既有调度限制影响，不应作为严格以太网IFG约束下的硬件极限。**该既有机制保持不变；本轮不是严格IFG实现的性能验证。

### 每台交换机的包率（seed1）

每台交换机接66个端口（64个XPU Die端口＋2个MPU端口），下表为这些出口之和。完整三种子数据可从[汇总CSV](vc-packing/summary.csv)及对应种子的`switch-rates.csv`读取。

{swtable}### 关闭拼包后的小包结构

{plain}seed1关闭拼包的线上平均数据帧长度为{unpacked['wire_bytes']/unpacked['data_frames']:.2f} B，开启时为{packed['wire_bytes']/packed['data_frames']:.2f} B。B和AR短记录单独发送时都填充到64B MAC帧，即72B线上帧，但仍各占1个信用单位。拼包对包率和信用取整开销的作用应同时考察。

种子1开拼包另观测到交换机侧9B控制发送{int(packed['control_sizes'].get('9',0)):,}次、64B控制发送{int(packed['control_sizes'].get('64',0)):,}次；关拼包分别为{int(unpacked['control_sizes'].get('9',0)):,}和{int(unpacked['control_sizes'].get('64',0)):,}次。它们包含本地LLR/CBFC信令，不能混入数据交换转发pps，控制消息的模型线时也应与MAC数据帧区分。

这里测到的是**该工作负载要求的实际转发包率**。当前交换机具有链路串行化、队列、固定转发预算及CBFC，但未设置独立ASIC包处理能力上限，因此不能由本实验宣称某颗交换芯片的最大硬件Mpps。

### 复现与验收

两组共6例，加上同一原始DAG的ps精度基线和采样关闭对照，共8例。所有案例均验证为ps精度，使用同一二进制；基线任务、资源输入及任务时间戳与本轮通信案例逐字节一致。seed1开关采样的任务/端口/UCIe/资源四类统计逐字节一致。逐包分析校验每记录交付一次、每帧两跳各出现一次、各端口帧数/字节守恒、VC映射与CU范围，以及配对AXI记录/有效数据量一致。

[运行计划](vc-packing/registered-plan.json) · [运行验收](vc-packing/run-verification.json) · [统计验收](vc-packing/verification.json) · [图表、CSV与复现脚本下载](vc-packing/evidence.zip)

```bash
python3 simulators/sue/harness/v2/vc_packet_study.py \\
  results/sue/full-rerun-20261030-unified/communication \\
  results/sue/vc-packing-repeat --time-resolution-ns 0
python3 simulators/sue/harness/v2/analyze_vc_packet_study.py \\
  results/sue/vc-packing-repeat results/sue/vc-packing-repeat/analysis
```

原始运行目录为`{study}`。每例保留输入、命令/二进制哈希和原始CSV；大文件为无损gzip。下载包包含汇总、完整帧长计数、逐端口VC与包率统计及证据哈希，原始逐事务大文件保留在运行目录中。
<!-- END VC PACKING EXPERIMENTS -->

'''
    chapter=a.manual/'08-deepseek-16chip-case.md';existing=chapter.read_text()
    begin='<!-- BEGIN VC PACKING EXPERIMENTS -->';end='<!-- END VC PACKING EXPERIMENTS -->'
    if begin in existing:
        start=existing.index(begin);finish=existing.index(end)+len(end)
        existing=existing[:start]+text.rstrip()+existing[finish:]
    else:
        marker='## 8.7 可以直接复制的运行命令'
        assert marker in existing
        existing=existing.replace(marker,text+'## 8.9 可以直接复制的运行命令',1)
    chapter.write_text(existing)
    with zipfile.ZipFile(dest/'evidence.zip','w',zipfile.ZIP_DEFLATED) as z:
        for f in sorted(dest.rglob('*')):
            if f.is_file() and f.name!='evidence.zip':z.write(f,f.relative_to(dest))
    print('Chapter 8 experiments, plots and evidence updated')


def math_ceil(v):
    import math
    return math.ceil(v)

if __name__=='__main__':main()
