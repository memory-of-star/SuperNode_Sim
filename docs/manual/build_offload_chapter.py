"""Integrate the transactional PCIe offload report into chapter 9 and its offline assets."""
from pathlib import Path
import hashlib
import json
import shutil

STUDY = 'results/sue/full-rerun-20261030-unified/offload'
SHORT = {'glm-5.3': 'GLM-5.3', 'kimi-k3': 'Kimi-K3', 'deepseek-v4.1-flash-1017': 'DeepSeek V4.1 Flash 1017'}
TARGET = {'kv': 'KV cache', 'engram': 'Engram', 'both': 'KV + Engram'}
ORDER = [('glm-5.3','kv'), ('kimi-k3','kv'), ('deepseek-v4.1-flash-1017','kv'), ('deepseek-v4.1-flash-1017','engram'), ('deepseek-v4.1-flash-1017','both')]


def table(rows, columns):
    def cell(v):
        return f'{v:.3f}' if isinstance(v, float) else str(v)
    return '\n'.join(['| ' + ' | '.join(title for key,title in columns) + ' |',
                      '| ' + ' | '.join('---' for _ in columns) + ' |'] +
                     ['| ' + ' | '.join(cell(r[key]) for key,title in columns) + ' |' for r in rows]) + '\n\n'


def build(here, root):
    expedited = here/'offload128'/'chapter.md'
    complete = root/STUDY/'analysis/results.json'
    has_complete = complete.exists() and len(json.loads(complete.read_text())) == 330
    if not has_complete and (root/STUDY).parent.joinpath('expedited-delivery.json').exists() and expedited.exists():
        (here/'09-mpu-pcie-offload.md').write_text(expedited.read_text())
        return
    source = root/STUDY/'analysis'
    rows = json.loads((source/'summary.json').read_text())
    comparisons = list(__import__('csv').DictReader((source/'main-comparison.csv').open()))
    comparisons = [{k: float(v) if k not in ('model','target') else v for k,v in r.items()} for r in comparisons]
    verification = json.loads((source/'verification.json').read_text())
    assert verification['passed'] == verification['planned'] == 330 and not verification['failed']
    assert all(r['transport_model']=='transactional' and r['pcie_setup_ns'] is None and r['hbm_policy']=='capped-max-min' for r in rows)
    dest = here/'offload'
    dest.mkdir(exist_ok=True)
    names = ['report.html','summary.csv','summary.json','main-comparison.csv','results.json','verification.json',
             'resource-diagnostics.json','plan.json','design.json','offload-speedup.svg','offload-speedup.png','report-validation.json','plot-data.json','report-generation.json','hardware-profile.json']
    for name in names:
        shutil.copy2(source/name, dest/name)
    (dest/'source-manifest.json').write_text(json.dumps({
        'source': str(source.relative_to(root)),
        'files': {n: hashlib.sha256((source/n).read_bytes()).hexdigest() for n in names},
        'scope': 'Frozen transactional PCIe experiment report and summaries; full per-task traces remain in original run directories.'
    }, ensure_ascii=False, indent=2))
    text = '''# 9. MPU与PCIe卸载对比

§9.1–9.8已使用事务级PCIe＋新的HBM带宽共享模型重新运行完整对比：固定16颗XPU，逐渐增加1/2/4/8/16个MPU，比较三种模型的KV cache、DeepSeek Engram及混合卸载。**PCIe基线、纯MPU、混合与分流确认曲线全部使用相同的PCIe与HBM配置；confirmed share在每个MPU数量下均实际测量，不由单点外推。** §9.9介绍模型机制及初始联动验证。

[独立交互报告](offload/report.html)保留工作负载筛选与下载；下文直接纳入主要图表、全部节点数对比、固定分流扫描和独立种子验证，离线手册无需联网阅读。

## 9.1 实验范围与统一配置

**结果版本：**本章330例采用同一冻结配置执行并补齐：事件唤醒、全局1 ps精度、任务注入/重试与依赖通知1 ns；XPU和MPU单向端口接入延迟各400 ns，交换机转发预算300 ns。IO Die启用尽力拼包，同端口、同目的SA、同VC、同事务类别合并；AW+W成对，AR/B/R各自合并。MPU以256 B AXI请求为并发单位，每节点128个在途槽位，200 ns访问延迟可重叠，带宽仍按FIFO逐周期分配。HBM采用公平共享。旧版结果独立保留，本章图表不混用历史基线。

共运行180个主对比条件、100个固定比例扫描条件，再运行50个独立种子确认条件，**330个最终结果全部通过，核验任务数与执行重试记录见9.8节**。

测量对象是带计算、HBM、TP/EP和卸载依赖的代表块，由仿真器从开始执行到最终任务完成；连续两个步骤的总窗口除以2，得到下文的“µs/代表块”。未包含完整逐层attention QK/AV、KV新增写回或prefill，**不是完整模型TPOT/TTFT，也不能乘层数当作整模型耗时**。每个代表块包含两张Engram表的完整查询，相关占比不能直接外推到全模型。

- 固定16颗XPU，每颗4个Die，PCIe端点单向128 GB/s；每8卡共享一个双路Host，每路CPU连接4卡。每Host 1 TB DDR，双路各12通道DDR5-6400。
- 每个MPU容量512 GB、单向190 GB/s，本轮`mpu_efficiency=1.0`；所有访问者共享该节点服务能力，不能把池带宽分配给每个XPU。
- 本轮所有MPU条件统一采用128条256 B AXI请求在途上限。仅按200 ns访问延迟计算，`128×256B/200ns=163.84 GB/s`仍低于190 GB/s；还需承担数据服务时间。覆盖190 GB/s的延迟至少约149个槽位（尚未计数据服务及其他开销），因此128槽位仍是有限并发配置，不能视为满带宽MPU。本轮不再混入64槽位对照。
- 固定八轨、八台256口交换机，使用明确的`hw-N-M`实验拓扑；增加MPU意味着增加节点和接入端口。
- Batch=64，上下文32768；GLM/DeepSeek固定TP4，Kimi因容量固定TP32，全部EP64。模型之间不能直接用耗时比较能力。
- HBM每Die有效总带宽1.875 TB/s；多个访问源按capped max-min共享，受限源用不完的份额重新分配。源内仍为FIFO；配置见[HBM JSON](examples/hbm-fair-share.json)。
- 主曲线使用seed1/2/3；同模型各方案配对token路由、计算量和读取字节。Kimi的KDA状态保留本地，只卸载MLA KV。
- KV先按256 KiB组织逻辑消息，再拆成256 B AXI；Engram按整行分配，同一目标MPU的行聚合后，按总返回字节拆成256 B AXI。后者尚不是逐行真实随机地址与对齐边界的AXI trace。使用确定性合成地址：KV连续DMA，Engram随机gather；不是实测文本热度或地址trace。

本章与第8章统一采用事件唤醒、1ps精度、1ns请求间隔、128条MPU在途和190GB/s有效服务带宽；第8章与第9章的数据放置及工作负载仍不同，不能直接混算加速比。

## 9.2 PCIe与MPU延时建模

| 延时组成 | PCIe路径 | MPU路径 |
| --- | --- | --- |
| DMA发起 | 每卡2引擎、64个驻留描述符；提交间隔100 ns、引擎启动200 ns | 每条AXI产生间隔1 ns；任务依赖通知1 ns |
| 事务并发 | MRRS 512 B、MPS 256 B；256 tags、256 KiB在途上限；有限信用 | 逐256 B AXI、尽力拼包、128个内存在途槽位，以及SUE的LLR/CBFC和发送队列 |
| 链路共享 | 单卡128 GB/s，每4卡共享512 GB/s NUMA上行，分方向排队 | 八轨200G端口，竞争交换机及端点链路 |
| 内存服务 | 每CPU理论614.4 GB/s，效率0.75，即460.8 GB/s；12通道队列、100 ns读取延时 | 每MPU单向190 GB/s、每AXI请求200 ns延迟，128槽位流水，带宽FIFO |
| 接收消费 | 每卡64 KiB接收缓冲，流水写HBM，写完释放缓冲；与专家等访问源共享HBM | 收齐消息后写HBM，再消费读取；写入源上限200 GB/s/Die，与其他源共享HBM |

MPU的128槽位只限制已准入的内存请求；尚未准入的AR可以在节点等待，当前没有把该等待队列容量耦合回SUE信用。已完成的内存结果等待网络发送时也不继续占用内存槽位。因此本轮不是完整的IO Die缓存/内存控制器背压模型。

本轮保留1ns全局网格以保证64→128槽位的单变量对照，因此小包序列化与亚纳秒IFG仍有量化误差；当前ps精度及设备空队列重新启动时的IFG限制见§8.8和IO说明。这轮对比不能当作严格PHY线时约束下的硬件性能证明。

上述非用户指定的事务参数是可修改的建模假设，不是硬件标定结果。配置见[PCIe JSON](examples/pcie-dual-socket-ddr5-6400.json)。事务级模型不包含PCIe PHY、真实DRAM时序及跨decode投机预取，完整边界见§9.9。

Host侧按NUMA存放KV分片及Engram副本，检查每CPU 500 GB容量。唯一数据集字节与Host物理复制容量不同；每例容量明细保存在results.json中。

## 9.3 分流策略与图例

- **MPU only**：全部卸载数据由MPU提供。
- **hybrid: nominal share**：按标称服务带宽分配，MPU比例为`190×M/(16×128+190×M)`，M为MPU数。8 MPU时约42.6%，是为对照而保留的预登记启发式，没有按共享DDR瓶颈或128个AXI槽位的吞吐上限重新优化；不是最优调度器。
- **hybrid: confirmed share**：对1/2/4/8/16 MPU分别用seed1从12.5%/25%/50%/75%和该节点数的nominal share中选最快比例，冻结后再用seed2/3验证。绿色曲线的25个点全部实测，无论是否加速均保留。“确认”不表示全局最优，也不保证在独立种子上优于nominal share。每个点的选定比例见§9.5。
- **Engram MPU + KV PCIe**：按数据类型分工，Engram全部放MPU，KV全部通过PCIe读取。

混合方案将互不重复的数据分片放在两个后端，请求并行执行，消费者等待两边都完成。速度比=PCIe基线平均耗时/当前方案平均耗时，**大于1才表示加速**。曲线阴影以同工作负载三种子PCIe均值作固定分子，分别除以方案最大/最小耗时得到范围，不是逐种子配对速度比范围，也不是置信区间；绿色确认曲线的分子和分母均仅使用确认种子2/3，阴影同样按该组最大/最小耗时计算。连线连接实测节点数，各点比例可能不同，不是同一固定比例的扩容曲线。

![事务级PCIe、共享DDR与共享HBM模型下，五种工作负载的速度比及全节点确认曲线](offload/offload-speedup.svg)

## 9.4 逐渐增加MPU：全部主对比结果

以下均为µs/代表块、seed1/2/3均值。“混合”使用nominal share；调整比例后的结果另见9.5节。

'''
    marker='## 9.4 逐渐增加MPU：全部主对比结果'
    highlights=[]
    for model,target in ORDER:
        candidates=[r for r in rows if (r['model'],r['target'],r['backend'],r['variant'])==(model,target,'hybrid','main')]
        best=min(candidates,key=lambda r:r['mean_block_us'])
        highlights.append(f"{SHORT[model]} / {TARGET[target]}：本次主曲线中最快混合点为{best['mpus']} MPU、{best['mean_block_us']:.3f} µs，相对新PCIe基线延迟变化{best['latency_change_pct']:+.2f}%。")
    text=text.replace(marker,'**读图结论（主曲线范围内）：**\n\n'+'\n'.join('- '+v for v in highlights)+'\n\n这些是有限节点数和固定启发式比例的扫描结果；候选比例的独立确认见§9.5，不代表全局最优。\n\n'+marker)
    for model,target in ORDER:
        selected = [r for r in comparisons if (r['model'],r['target']) == (model,target)]
        for r in selected:
            r['mpus'] = int(r['mpus'])
        base = next(r for r in rows if (r['model'],r['target'],r['backend'],r['variant']) == (model,target,'pcie','main'))
        text += f'### {SHORT[model]}：{TARGET[target]}\n\nPCIe基线 **{base["mean_block_us"]:.3f} µs**；全部卸载释放HBM约 **{base["HBM_freed_GB"]:.3f} GB**。\n\n'
        text += table(selected,[('mpus','MPU数量'),('MPU_us','纯MPU µs'),('MPU_speedup','纯MPU速度比'),('hybrid_us','混合 µs'),('hybrid_speedup','混合速度比'),('hybrid_MPU_traffic_pct','混合MPU载荷%')])
    pure=[r for r in rows if r['variant']=='main' and r['backend']=='mpu']
    text += f"纯MPU的{len(pure)}组配置中，{sum(r['speedup_vs_PCIe']>1 for r in pure)}组快于新PCIe基线。\n\n"
    text += """## 9.5 每个MPU数量的比例选择与独立验证

对五种工作负载、1/2/4/8/16 MPU分别选择比例。seed1候选为12.5%/25%/50%/75%与该节点数的nominal share；按代表块耗时最小选择，若相同则选较小比例。选定后不再根据seed2/3调整。每个点用seed2/3实测，PCIe基线也只用相同两个种子；负的延迟变化表示加速。候选网格有限，不代表全局最优。

"""
    for model,target in ORDER:
        text += f"### {SHORT[model]} / {TARGET[target]}\n\n"
        scans=[]
        for m in (1,2,4,8,16):
            row={'mpus':m}
            for v in rows:
                if (v['model'],v['target'],v['mpus'],v['variant'])==(model,target,m,'share-sensitivity'):
                    row[str(v['mpu_fraction'])]=v['mean_block_us']
            nominal=next(r for r in json.loads((source/'results.json').read_text()) if
                         (r['model'],r['target'],r['mpus'],r['backend'],r['seed'],r.get('study_variant','main'))==(model,target,m,'hybrid',1,'main'))
            row['nominal']=nominal['block_us'];scans.append(row)
        text += 'seed1候选耗时（µs/代表块）：\n\n'+table(scans,[('mpus','MPU数'),('0.125','12.5%'),('0.25','25%'),('0.5','50%'),('0.75','75%'),('nominal','nominal share')])
        confirmed=[dict(r,fraction_pct=r['mpu_fraction']*100) for r in rows if
                   (r['model'],r['target'],r['variant'])==(model,target,'share-confirmation')]
        confirmed.sort(key=lambda r:r['mpus'])
        text += '独立种子2/3确认：\n\n'+table(confirmed,[('mpus','MPU数'),('fraction_pct','选定MPU比例%'),('PCIe_baseline_us','PCIe µs'),('mean_block_us','混合 µs'),('min_block_us','最小µs'),('max_block_us','最大µs'),('speedup_vs_PCIe','速度比'),('latency_change_pct','延迟变化%')])
    ds_confirm=sorted([r for r in rows if (r['model'],r['target'],r['variant'])==('deepseek-v4.1-flash-1017','both','share-confirmation')],key=lambda r:r['mpus'])
    text += '**DeepSeek KV＋Engram确认曲线：**'+ ' → '.join(f"{r['mpus']} MPU：{r['mean_block_us']:.3f} µs" for r in ds_confirm)+'。'
    if all(b['mean_block_us']<=a['mean_block_us'] for a,b in zip(ds_confirm,ds_confirm[1:])):
        text += '这组确认均值随节点数下降，4 MPU不再是最慢点。'
    text += '各点分别选择分流比例，因此不能把这条曲线解释成只改变MPU数量的单变量消融，也不能据此断言旧曲线低谷完全由网络拥塞造成。\n\n'
    text += '## 9.6 Engram放MPU、KV放PCIe\n\n这里两类数据各用一条路径；以两类数据全部通过新PCIe读取为配对基线。\n\n'
    tier = sorted([r for r in rows if r['backend']=='tiered' and r['variant']=='main'],key=lambda r:r['mpus'])
    text += table(tier,[('mpus','MPU数量'),('mean_block_us','µs/代表块'),('speedup_vs_PCIe','相对全PCIe速度比'),('latency_change_pct','延迟变化%')])
    last=tier[-1]
    text += f"16 MPU时为{last['mean_block_us']:.3f} µs，相对全PCIe延迟变化{last['latency_change_pct']:+.2f}%。该分工与小比例字节/行分流不同。\n\n"
    text += '''## 9.7 容量价值与适用边界

DeepSeek两张Engram表合计202,758,032,400字节，约202.758 GB；单个512 GB MPU就能容纳。每token只查询48行、12,672字节，batch64合计约0.811 MB，不能把整表容量当成每步传输量。

本轮卸载释放的唯一数据集容量如下。所有数据集均小于512 GB，因此增加MPU主要是增加并行服务资源，也增加硬件和闲置容量；本轮没有成本评估。

'''
    capacity = []
    for model,target in ORDER:
        r = dict(next(r for r in rows if (r['model'],r['target'],r['backend'],r['variant']) == (model,target,'pcie','main')))
        r['workload'] = SHORT[model]+' / '+TARGET[target]
        capacity.append(r)
    text += table(capacity,[('workload','卸载数据集'),('HBM_freed_GB','释放HBM / 唯一数据集GB')])
    text += f"核验{verification['checked_tasks']:,}个DAG任务；{verification['passed']}/{verification['planned']}条件通过。\n\n"
    text += '''## 9.8 校验、原始证据与复现

逐例核验输入哈希、完整有效字节交付、DAG依赖、计算资源不重叠、MPU逐AXI延迟与在途上限、FIFO字节顺序与每周期带宽预算、HBM并发服务的字节积分和源/Die带宽上限，以及跨方案一致的token路由/计算/读取量。事务路径另核验描述符完成、请求/Completion线上字节、DDR与HBM写入字节守恒，以及tag、在途窗口、DMA队列、信用和接收缓冲上限。累计等待并不是端到端关键路径，不能直接相加。

- [本轮冻结的128条并发硬件JSON](offload/hardware-profile.json)。
- [全部汇总CSV](offload/summary.csv)与[主对比CSV](offload/main-comparison.csv)。
- [每例结果及原始run路径](offload/results.json)、[完整输入矩阵](offload/design.json)、[运行前计划与比例选择规则](offload/plan.json)。
- [全量校验记录](offload/verification.json)、[各芯片/MPU载荷与队列诊断](offload/resource-diagnostics.json)。
- [报告来源与文件SHA256](offload/source-manifest.json)、[生成器与输入输出哈希](offload/report-generation.json)、[图表SVG](offload/offload-speedup.svg)。

完整原始轨迹保存在`results/sue/full-rerun-20261030-unified/offload-primary/conditions/`及`offload-confirmation/conditions/`。每个条件保存可复用的统一JSON输入、程序哈希和原始统计。大型IO CSV无损gzip保存，SHA256按解压后字节计算。

先运行登记的280个主/扫描条件，按每个工作负载和MPU数量的seed1完成时间选择分流比例，再冻结25个选定比例并执行seed2/3的50个确认条件。独立种子不参与选参。完整步骤与校验记录位于该研究目录；输入分别为`inputs/offload-primary.json`和`inputs/offload-confirmation.json`，可用同一入口复现：

```bash
SUE_COMPRESS_TRACES=1 bin/sue run \
  --config results/sue/full-rerun-20261030-unified/inputs/offload-primary.json \
  --output results/sue/offload-primary-repeat
```

再次选参必须基于这次新运行的seed1结果；不能把旧确认配置冒充新一轮选参。所有案例使用独立进程与冻结的模型/硬件/行为输入；任何失败会保留并阻止整套结果发布。

本章表格和离线附件由`build_offload_chapter.py`直接读取该次已冻结的汇总生成，`build_manual.py`将其并入手册、图像内嵌并更新下载包。
'''
    adjustment=json.loads((source/'plan.json').read_text()).get('execution_adjustment')
    if adjustment:text=text.replace('## 9.8 校验、原始证据与复现','## 9.8 校验、原始证据与复现\n\n执行过程中将独立案例并行进程数由12调整为24，已完成案例保留，未完成尝试归档后重跑；该调整只影响墙钟耗时，单个仿真仍为单线程。执行记录见plan.json的execution_adjustment，归档位于matrix/interrupted-attempts。')
    retries=json.loads((source/'plan.json').read_text()).get('execution_retries',[])
    if retries:text=text.replace('## 9.8 校验、原始证据与复现','## 9.8 校验、原始证据与复现\n\n最终330例全部完成；其中'+str(len(retries))+'例此前触发1200秒墙钟上限，仅延长至7200秒后补跑。任务、资源、端口配置及所有仿真参数保持一致；原超时记录、补跑路径与逐文件一致性核验保存在plan.json的execution_retries及原始目录中。')
    supplement=here/'pcie-transaction-model.md'
    if supplement.exists():
        text+='\n'+supplement.read_text()+'\n'
    (here/'09-mpu-pcie-offload.md').write_text(text.rstrip()+'\n')


if __name__ == '__main__':
    here = Path(__file__).resolve().parent
    build(here, here.parents[1])
