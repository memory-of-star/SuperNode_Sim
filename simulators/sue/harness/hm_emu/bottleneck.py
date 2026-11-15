"""Measured second-step local activity and exposed wait for the current tree profile."""
import csv,json,pathlib,numpy as np
from run import load,plan_costs
from execution_analysis import analyze as execution

def analyze(rows):
 r=next(x for x in rows if x['mode']=='TP' and x['chips']==8 and x['batch']==1 and x['duplex']=='sum' and x['variant']=='base')
 assert r['collective_algorithm']=='tree' and r['branch_schedule']=='moe_parallel'
 d=pathlib.Path(r['path']);root=d.parent.parent;impl=json.loads((root/'implementation.json').read_text())
 config=next(pathlib.Path(k) for k in impl['input_files'] if pathlib.Path(k).suffix=='.json' and json.loads(pathlib.Path(k).read_text()).get('schema')=='hm-emu-1')
 c,m=load(config);tiles=np.load(root/'routes/expert-tiles-m1.npy');cost,metrics,_=plan_costs(m,1,8,'TP',tiles,r['partition']);frozen=np.fromfile(d/'service.bin',dtype='<f8').reshape(r['service_shape']);assert np.allclose(cost*1e12,frozen,rtol=1e-14,atol=1e-4)
 v=metrics[1,0,0];a=execution(d);a.update(strategy='TP8（8卡）',hbm_ms=float(v[2]*1000),d2d_ms=float(v[3]*1000),compute_busy_ms=float(v[1]*1000),hbm_GB=float(v[4]/1e9),variants={x['variant']:x['tpot_ms'] for x in rows if x['mode']=='TP' and x['chips']==8 and x['batch']==1 and x['duplex']=='sum'})
 return a

def section(rows,table):
 a=analyze(rows)
 s='<section id="bottleneck"><h2>3. 单请求瓶颈：B=1、TP8</h2><p>本章只分析当前树形AllReduce＋MoE双分支并行方案。固定B=1、整卡TX64/RX64 GB/s、固定网络路径2 μs，统计连续两步中第二步的93层decode。</p>'
 s+=table(['TPOT / ms','至少一张卡有本地服务 / ms','无本地服务的暴露等待 / ms','本地服务与集合通信重叠 / ms'],[[f'{a[k]:.4f}' for k in ('tpot_ms','local_active_ms','exposed_wait_ms','local_communication_overlap_ms')]])
 s+='<p>前两类时间区间按事件轨迹取并集，互不重叠，相加等于TPOT。暴露等待主要来自网络依赖及通知；它不是所有消息延时之和。集合通信与本地服务可以重叠，不能再把每次AllReduce的耗时全部相加作为应用等待。集合通信区间包含树上归约处理，本地服务也包含这些处理，因此“重叠”不是纯链路传输隐藏量。</p>'
 s+=f'<h3>为什么单请求仍需约{a["tpot_ms"]:.2f} ms？</h3><p>模型算子本身每Die的HBM服务预算约{a["hbm_ms"]:.2f} ms，读取约{a["hbm_GB"]:.2f} GB。每Die有效HBM带宽562.5 GB/s；latent down/up在各Die复制，92层合计约9.45 GB/Die/token，路由器约1.18 GB。这部分不会随TP卡数继续均分。以上不含树中间节点新增的向量归约读取；其服务时间已加入事件执行，并参与资源竞争。</p>'
 s+=table(['算子HBM预算 / ms','片内D2D预算 / ms','算子计算忙碌预算 / ms'],[[f'{a[k]:.4f}' for k in ('hbm_ms','d2d_ms','compute_busy_ms')]])
 s+='<p>预算按一个Die计，不能直接相加：算子内部有双缓冲重叠。两条MoE分支就绪后同时提交本地工作，活跃工作平分每卡的归一化本地服务能力；工作完成后释放份额。此模型维持有限服务预算，但不是分别仲裁每个HBM通道、SM和D2D链路的GPU指令级模型。</p><h3>集合通信如何建模</h3><p>278次调用保持不变：93次Attention、1次Dense、92次共享专家、92次路由专家。卡间AllReduce采用堆式二叉树：rank r的父节点为floor((r−1)/2)，子节点为2r＋1、2r＋2。子树向上Reduce，根节点再Broadcast。每个内部节点只等本次操作中自己的子节点收齐向量，再进行归约和上送；广播收到后立即向自己的子节点转发。没有逐轮全体rank屏障。</p><p>每条树边发送完整归约向量，按四Die分stripe，再由SUE分包；每次AllReduce共2(N−1)条逻辑消息，这个数是消息总数，不是串行轮数。内部节点的每个子输入按每Die两次读、一次写及BF16加法计本地服务，和算子共享预算。树根多子节点流量的竞争由原SUE出口队列、CBFC与链路序列化处理，不额外再加“每轮2 μs”。</p><p>同一MoE层在Attention结果就绪后，同时推进共享分支与路由分支；分支计算完成后各自发起归约，两者可与另一分支本地工作或通信重叠。两分支最终汇合后才进入下一层。latent up仍在下一层依赖段中执行，未将不同维度的共享/路由归约合并；跨层不做无依据的抢跑。</p><p>当前是整向量树转发，不是NCCL完整实现：没有slice级流式归约、双树、NVLS或自动算法选择；算子阶段仍按卡组就绪推进，卡内归约保留四Die环的行为预算，未细分与卡间阶段的交叠。这些边界影响预测，尤其大消息时树根可能成为瓶颈。</p><h3>当前方案的带宽与延时敏感性</h3>'
 s+=table(['当前配置 ms','带宽×2 ms','固定路径2→1 μs ms'],[[f'{a["variants"][k]:.4f}' for k in ('base','bandwidth_x2','latency_half')]])
 s+='<p>对照只改变相应网络参数，计算服务和路由保持一致。结果是当前行为模型下的预测，不是芯片实测。</p></section>'
 return s
