# hm_emu：Kimi K3 多 Die / 单交换机 TP、PP 实验

当前默认 `study.json` 使用 **SUE 逐包后端**：四 Die 各有独立链路端点，连接同一台交换机，调用原 `SueNetDevice`、`SueChannel`、AFH、LLR、CBFC。原协议核心没有修改。卡内四 Die 环及算子/HBM的资源模型保留。

`study-message.json` 使用较快的消息级对照；它不能输出真实协议行为。两种后端都使用 ns-3 core，必须依据配置和原始运行目录区分。卡间物理拓扑均为单层交换机星型，Ring AllReduce只表示逻辑集合通信算法。

## 使用

命令从仓库根目录执行。Python需要NumPy；全新克隆先 `bin/sue build` 建好原ns-3/SUE库。

```bash
# 短程实现验证：2卡、B=1/2、2步decode
bin/sue hm-emu check --config configs/experiments/hm-emu/sue-validation.json
bin/sue hm-emu run --config configs/experiments/hm-emu/sue-validation.json --output results/sue/sue-validation-new

# 完整decode：2/4/8卡，TP和PP，B=1，每请求256步，两种方向带宽
bin/sue hm-emu run --config configs/experiments/hm-emu/sue-full-decode.json --output results/sue/sue-full-new

# 真正的CBFC背压和LLR错误恢复测试
python3 simulators/sue/harness/hm_emu/test_sue_backend.py results/sue/protocol-test-new

# 消息级全矩阵参考（不是逐包结果）
bin/sue hm-emu run --config configs/experiments/hm-emu/study-message.json --output results/sue/message-new
```

`study.json`定义完整SUE矩阵：576个条件，其中522个通过容量准入、54个容量拒绝。当前采用已完成的2步逐包矩阵；结果状态以对应输出目录的verification.json为准；报告渲染拒绝混用消息级结果或存在失败的矩阵。每次输出目录必须不存在；workers是独立案例进程数，不是单场仿真内部MTP。

## 硬件和链路

- 每芯片4Die：D0↔D1↔D2↔D3↔D0，每条边双向合计32 GB/s，方向各16 GB/s。
- 所有卡的四Die聚合接入端口连到一台SUE交换机；无芯片中继、无交换机级联。
- SUE主配置采用固定对称全双工：每Die每方向16 GB/s，整卡TX64/RX64 GB/s。对照每Die每方向32 GB/s，整卡TX128/RX128。
- 消息参考的主模型是整卡TX+RX动态共享128 GB/s，允许单向独占128；与固定64/64有差别。比较PP时不能把差异全部归因于协议。
- 沿用v2基线：链路传播20ns、交换机转发预算300ns（含出口线时），两侧XPU各加830ns接入开销，实际通道各850ns，固定路径2μs。入口序列化、排队、控制帧和重传另算；2048B单帧stripe在主配置下连同两次1ns通知约2.132μs。时延减半对照仅将XPU额外接入改为每侧330ns，链路和交换机保持不变。
- D2D固定每步10ns为未校准假设；事件时钟1ps，注入/依赖通知1ns。

## SUE profile

`modeling.backend="sue"` 与 `modeling.topology="single_switch"` 选择逐包路径。`modeling.sue_profile`提供：

- `payload:2048`：每帧最多有效载荷字节，不是完整线上帧大小。
- `credits:512`：每VC信用数，credit为256B，窗口128KiB；交换机出口每VC容量与窗口一致。
- `link_propagation_ns:20`、`switch_forwarding_ns:300`：v2链路及交换机基线；两侧XPU额外时延自动按 `(scaleup_hop_ns - 2*link_propagation_ns - switch_forwarding_ns)/2` 计算，不得为负。
- `bad_every:0`：默认不注错；正数在卡0 Die0的交换机入口每N帧注错，交由原LLR恢复。

主机每VC出口64KiB。当前业务都走VC0，队列满后停放，出口释放钩子唤醒，不做空轮询。接收完整数据后唤醒依赖，不等待LLR ACK返回。

本研究profile用原引擎支持的逐帧ACK、逐帧CF退还；关闭周期CC/CF刷新，避免长计算空隙产生大量周期空闲事件。CtlOS保持400B间隔门，IFG保持12B。此profile不同于原默认周期ACK；未声称匹配某芯片实际SDK配置。保留8个VC，但没有新增业务VC分配或AXI拼包。

## 并行和执行

TP数字是芯片数，实际rank=4×芯片数。PP每阶段一芯片，内部TP4；没有EP或DP副本。可切矩阵和注意力头切分，小投影、router、latent投影等复制；MLA压缩KV按head-shard rank复制。

模型按93层依赖图执行，有限权重tile双缓冲、token tile32、同微批专家复用，未模拟真实GPU指令或实测kernel效率曲线。片内通信在局部依赖段中折叠；跨卡逐包完成直接控制后续算子/归约步，非事后加一个通信常数。

## 代码与证据

- `model.py`：官方库存、算子/HBM/D2D、分片、容量和PP分层。
- `engine.cc`：完整decode调度；消息后端或编译时选择SUE后端。
- `sue_backend.h`：四Die端点、SUE交换机、分片、收齐回调、出口事件唤醒和协议统计；不替换协议核心。
- `run.py`：统一JSON、输入冻结、路由、执行和失败台账。
- `run_selected_sue.py`：将指定消息参考的冻结算子输入逐字节复制，使用SUE重跑；`sue-pp-b8.json`定义本次PP B=8对照选择。
- `test_model.py`、`test_network.cc`：算子和消息资源测试。
- `test_sue_backend.py`、`test_sue_switch.cc`：真实信用节流、重传和非邻卡经交换机直达验证。
- `report.py`只接收完整SUE逐包矩阵，统一生成第1–7章；保留用户指定标题与卡内TP4说明。`add_sue_report.py`仅用于历史报告，不用于当前报告。
- `replay_sue_matrix.py`：逐字节复用冻结算子输入，全量重新执行网络；从不复用旧性能值。
- `test_sue_latency.cc`：两种线速、三种帧长、两个固定时延目标的隔离测试。
- `publish_sue_report.py`：等待全量完成，核验原始证据，再生成报告和离线包。

输出含input/implementation/validation/results/verification及逐token完成时刻。SUE另有`engine-sue-ports.csv`（LLR/CBFC/阻塞唤醒计数）和`engine-sue-wire.csv`（每卡线上字节与帧数，含控制报文；不含IFG空隙）。协议计数不是拥塞时间。容量拒绝、超时和失败不可作为性能点。

当前主报告按用户要求统一为2步decode：TPOT取第2次与第1次输出的完成间隔，首次decode完成另列；没有prefill，不能称TTFT。吞吐为B×2除完整运行时间，包含启动与排空，不是长时间稳态吞吐。256步配置仅作为study-256steps.json保留。计算/HBM可能重叠，活跃时间不能相加为端到端耗时。

历史物理Ring报告保留在 `docs/reports/hm-emu/archive-ring/`。有INVALID标记的首轮错误结果不得引用。新运行冻结source_snapshot，避免后续修改代码后丢失对应执行证据。

## 更新HTML报告

完整重建（新克隆无需历史运行）：

```bash
bin/sue hm-emu check --config configs/experiments/hm-emu/study.json
bin/sue hm-emu run --config configs/experiments/hm-emu/study.json --output results/sue/hm-emu-packets-new
python3 simulators/sue/harness/hm_emu/publish_sue_report.py --run results/sue/hm-emu-packets-new --output docs/reports/hm-emu
```

若已有相同的冻结算子输入，可避免重新生成输入，但所有网络条件仍逐包重跑：

```bash
python3 simulators/sue/harness/hm_emu/replay_sue_matrix.py --config configs/experiments/hm-emu/study.json --source-run results/sue/hm-emu-switch-20261113 --output results/sue/hm-emu-packets-new
```

输出目录必须不存在。长矩阵的每个条件结束后写入ledger.jsonl和progress.json；不使用消息级数据补点。发布器只有在全部条件完成且核验通过后才替换正式HTML和ZIP。

## 报告当前章节与MTP情景评估

当前报告依次为：部署、2/4/8卡性能曲线、B=1 TP8瓶颈、B=1 TP8 MTP情景评估、原始数据。16卡条件不参与报告曲线或分析；已完成历史证据保留，未完成EP案例按用户要求停止。

MTP在这里指多token预测/推测解码，不是ns-3的多线程MTP。单请求由一层草拟代理串行生成候选，主模型验证depth+1个位置，网络仍执行原SUE逐包协议。草拟权重和接受率未经测量；代理采用MLA+MoE块、特征融合和输出头。验证按位置计KV/KDA访问，加保守状态恢复预算。接受率用解析期望换算有效token耗时，不伪称实际接受了候选。

```bash
python3 simulators/sue/harness/hm_emu/mtp_run.py --config configs/experiments/hm-emu/study-mtp.json --output results/sue/mtp-new
python3 simulators/sue/harness/hm_emu/mtp_audit.py results/sue/mtp-new
python3 simulators/sue/harness/hm_emu/publish_sue_report.py --run results/sue/hm-emu-sue-matrix-smoke-20261114 --mtp-run results/sue/mtp-new --output docs/reports/hm-emu
```

输入JSON可修改草拟深度、条件接受率、草拟本地成本倍率；主矩阵和MTP原始记录分别保留。MTP报告包含逐包周期与假设期望两种口径，不能把摊销ms/token当作平滑逐token到达间隔。
