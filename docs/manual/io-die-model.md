# IO Die AXI事务与拼包配置

本组件在外部SUE适配器中把通信任务拆成AXI事务单元，通过原有AFH、LLR和CBFC发送。它是事务尺寸、因果关系、有限队列和网络包级行为模型，不是RTL AXI总线或某款IO Die的周期精确模型。IO Die作为现有Die端口之前的逻辑适配层，不新增物理SA，也未另加片内NoC/交叉开关延迟。

## 启用

```bash
bin/sue build
bin/sue run --output results/sue/deepseek-io-packed \
  --model deepseek-v4.1-flash-1017 --chips 16 --tp 4 \
  --batch 1 --context 128 --mpus 2 --engram-remote --steps 1 \
  --io-die-profile configs/hardware/io-die/best-effort.json
```

将配置路径换成`configs/hardware/io-die/no-packing.json`即可关闭拼包但保留AXI事务层。完全不提供此参数则保留历史任务/确认模型，便于复现；它与“AXI不拼包”不是同一种模式。其他Python实验入口可设置环境变量`SUE_IO_DIE_PROFILE`，只影响新生成案例。每个案例保存`io-die.json`及编译后的`io-die.tsv`，冻结案例运行不再读取外部环境配置。

## AXI字段依据与默认大小

依据[Arm AMBA AXI/ACE规范 IHI0022H，A2、A3与A8](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/IHI0022H_amba_axi_protocol_spec.pdf)。规范描述信号，不规定AXI-over-SUE网络编码；地址、ID、USER位宽可配置。下面是本模型明确采用的序列化预算，不是协议规定的网络头长度。

- AXI4，64位地址、16位ID、128位数据总线，USER位宽0。
- 256 B W/R数据对应16个beat；不是一个2048位AXI beat。按完整256 B事务计费，末尾不足256 B的有效数据补齐，分别记录有效字节和传输字节。
- AW/AR：地址64、ID16、LEN8、SIZE3、BURST2、LOCK1、CACHE4、PROT3、QOS4、REGION4，合计109 bit，每通道向上取整14 B。
- W：256 B数据，WSTRB合计256 bit、WLAST共16 bit，额外34 B。AXI4 W通道不添加WID。
- B：BID16＋BRESP2，向上取整3 B。
- R：每beat包含RID16、RRESP2、RLAST1，16beat合计304 bit，额外38 B；未优化重复字段。
- VALID/READY属于本地握手，不随每个网络记录发送。USER默认关闭；改变USER位宽会增加通道成本。
- 每事务单元增加8 B自定义封装元数据预算（类型、长度、事务标识），每网络包增加4 B封装头；这些不是AXI标准字段。SUE头、LLR preamble、控制消息及链路时延由底层另计。

| 可拼单元 | AXI字段＋数据 | 加8 B记录头后 |
|---|---:|---:|
| AW＋W | 14＋34＋256 = 304 B | 312 B |
| AR | 14 B | 22 B |
| B | 3 B | 11 B |
| R | 38＋256 = 294 B | 302 B |

包长不能简单使用256 B乘记录数。每个SUE包的适配器载荷为`4 B + 同类记录数 × 记录大小`，受默认8192 B上限限制。默认`min_mac_frame_bytes=64`，短控制事务封包后补齐到至少64 B的MAC帧预算；LLR preamble另计。未额外引入PHY/FEC编码开销。

## 请求语义

普通跨芯片发送（MoE分发/返回、跨芯片集合通信等）映射为AW＋W写入，每256 B事务在接收后产生一个B；B沿原端口返回，任务在全部B返回后完成。接收依赖在全部数据到达时触发，不等待全部B。AW和W输入在该行为模型中假定同时就绪，不独立模拟AW先到/W后到。

既有`DAG.remote_read`请求→服务→返回链在`mpu_service_unit="axi"`时下沉为逐256 B事务：每条AR到达MPU就提交独立内存请求，取得槽位后开始固定200 ns访问延迟（由硬件`mpu_fixed_ns`配置）；所有请求共享每MPU的FIFO带宽服务器。该请求完成内存服务后即可将对应R放入IO发送队列，无需等待同批其他AR到齐或内存服务全部完成。尾请求即使有效数据不足256 B，内存服务及W/R线上数据也按完整256 B计费。

写向MPU的AW＋W同样按每256 B进入这套内存流水线；内存服务完成后才产生B。向XPU的普通网络写仍按接收完成产生B，不新增XPU内存写提交语义。AR、AW＋W共享每MPU的有限在途槽位和FIFO内存带宽；不同MPU独立。默认128槽位，延迟可以重叠；槽位从准入保留至内存服务结束，网络返回可以继续排队。暂未让已完成但等待R/B入队的结果反向占住MPU内存槽位。

新的两份IO配置均默认`mpu_service_unit="axi"`，不再应用Engram的`200 ns × ceil(行数/16)`批量延迟。旧配置缺少该字段（或显式设置`batch`）时保留批服务路径，供冻结案例复现。未启用IO层的历史资源入口仍以原资源任务为单位。

原DAG中的批服务和返回任务保留为统计汇总节点：服务开始取第一条AR到达，准入取最早准入，服务结束取最后一条内存完成；返回任务从第一条R就绪开始，全部R到达后完成。`io-die.json.axi_mpu`明确列出替代的两条批屏障边，校验器改为逐事务检查AR到达→准入→延迟到期→FIFO数据服务→对应R就绪，不能再对这两条汇总边要求整批结束后才启动。其他DAG依赖与HBM整任务收齐后写入的行为保留。`resource_statistics.csv.service_ns`是原任务预算，实际逐AXI时间以`io_mpu_requests.csv`为准。

当前逻辑地址/数据不逐字节存储，只模拟字段尺寸、事务序号、匹配关系和时序；不验证4 KiB边界、地址译码、真实AXI ID复用/乱序、原子操作或一致性。随机gather以已有每目的节点任务的总字节拆分，不是实际每行物理地址trace。

## 拼包规则

- `none`：一个AW＋W、AR、B或R单元一个网络包；AW＋W始终不分开。
- `best_effort`：发送机会到来时，取当前IO队列内可合并的同类单元，直到上限；不等待将来请求，不设置凑包定时器。
- 同一源物理端口、同一目的`chip.die`（SA）、同一VC、同一通道类别才能拼包。事务ID是匹配响应的标识，不是目的ID。目的侧代理Die也要求一致。
- AW＋W只与其他完整AW＋W拼；AR只拼AR、B只拼B、R只拼R。不跨端口、目的SA、VC或通道类别合并。
- 每端口最多4096个待封装单元，满时阻塞生产者并重试；已有设备出口队列和CBFC继续产生背压。默认每任务每1 ns产生一个单元，是可调输入速率假设，并非实测IO Die峰值。
- 用端口线速和包长计算封装发送机会间隔；实际SUE队列、信用和控制报文可造成额外发送延迟。拼包发生在设备队列之前，已封装进入设备队列的包不重新拆开合并。
- 小流量、空闲端口可能完全拼不起来，这是尽力拼包的预期行为。


## 全局ps精度与MPU的1 ns服务节拍

新生成的多Die案例默认`settings.time_resolution_ns=0`，ns-3使用PS分辨率，即1 ps。网络发送时间、帧间隔及HBM、PCIe、UCIe适配器事件不再统一量化到整数ns。200 Gbit/s下72 B线上帧的发送时间为2880 ps，12 B IFG的配置时长为480 ps。全局时间仍按离散事件跳转，不是每ps轮询所有设备。

AXI请求产生间隔、任务注入/重试间隔及DAG依赖通知默认仍为1 ns，分别由IO配置、`interval_ns`和`visibility_ns`控制；200 ns内存访问延迟、400 ns接入延迟等保持配置值。它们是服务/通知参数，与全局时钟精度分别设置。

AXI MPU数据服务窗口仍长1000 ps，每窗口可用字节数为`有效带宽 / 10^9`：190 GB/s对应190 B。按FIFO依次扣除请求剩余字节；当前请求完成后，本窗口余量立即给下一条已就绪请求。所有完成通知在窗口结束时发出，没有就绪请求时的预算不积攒。窗口起点可跟随到达事件落在非整数ns时刻，不强制与全局整数ns边界对齐。

`io_mpu_requests.csv`中的服务起止表示第一次获得服务的窗口起点和完成窗口终点；相邻事务的统计区间可能重叠一个窗口，这不表示并行各取一份带宽。FIFO顺序及总预算由`io_mpu_cycles.csv`核验；每窗口时长必须为1000 ps，但ps模式不要求起点为1000 ps的整数倍。

冻结输入显式设置`time_resolution_ns=1`时仍使用原NS分辨率；缺省字段或设置0时使用ps。历史归档不自动迁移；第8、9章现行实验已另行按1 ps与事件唤醒统一重跑。当前全局精度回归入口为`harness/v2/verify_time_resolution.py`。

已知独立限制：原有设备队列由空转为非空时，可以在IFG定时器结束前重新启动发送。因此恢复ps能保留480 ps的IFG时长，却不代表所有发送路径已严格强制该间隔；此调度问题另行记录，不在本次精度恢复中改动。

## 统计与验证

当前IO队列背压默认使用事件唤醒，保留1 ns请求间隔和MPU服务窗口；旧轮询对照为`settings.io_event_driven=0`。等待与唤醒机制、同时间戳顺序差异及实验结果见手册§3.14和[IO调度优化验证](io-wakeup.html)。

- `io_records.csv`：每个事务单元、所属任务、序号、通道、源/目的、端口、VC、有效字节、就绪和接收时间。AW＋W一行表示两个不可分离的AXI通道记录。
- `io_packets.csv`：拼好的网络包、成员记录、填充前后载荷长度、MAC帧长度、进入设备队列的时间`enqueued_ps`；该时间不是上线路径时间。
- `io_wire.csv`：底层链路真实发送事件，包含端点/交换机侧、物理端口、线上字节、忙碌时间。`packet=-1`表示非IO数据包，例如协议控制流量。一个转发包在两段链路各记录一次；统计交换机PPS应筛选`side=1`，不可把两跳总数当成一次交换机转发。
- `io_mpu_requests.csv`：每条AR或写向MPU的AW＋W对应的内存请求，其`id`直接关联`io_records.csv.record`，包含到达、准入、延迟就绪、带宽开始和完成时刻；`io_mpu_nodes.csv`记录并发峰值。
- `io_mpu_cycles.csv`：每周期按FIFO分配给各请求的字节数；同周期合计不得超过配置带宽的单周期预算，每请求累计必须恰好256 B。
- `io-mpu.tsv`及`io-mpu-writes.tsv`：冻结的服务参数和读写映射；JSON是权威配置。
- `summary.json.io_die`：记录数、包数、平均每包记录数及封装载荷总量。
- `io_scheduler.csv`：实际等待注册/恢复、端口唤醒、出口检查与出口释放通知次数。`pending=0`且`parked=resumed`表示所有被暂停的生产者均已恢复；事件次数不是网络报文数量。

```bash
python3 simulators/sue/harness/v2/verify_io_die.py results/sue/io-die-verification
```

测试覆盖拼包/不拼包、混合目的/端口/VC、读写、MPU返回、跨Die代理、小队列背压、孤立包立即发送。校验有效字节、AW/W与B匹配、AR与R匹配、原端口返回及包长上限。不把这些结果宣称为真实芯片标定或完整AXI协议一致性测试。

## 历史1ns网格机制验证（2026-10-30）

12个IO功能案例、7个逐AXI内存/时钟机制案例及9个MPU兼容回归案例均通过。机制测试包含读写共用FIFO、64槽位/1槽位、尾包补齐、剩余周期预算复用、HBM公平共享与事务级PCIe，并逐列检查所有输出CSV中的有效`_ps`时间戳为1000的整数倍。

在64 KiB、256条AR的无拼包测试中，第一条R在2304 ns就绪，最后一条AR在2867 ns才到达；这直接验证了不再等待整批AR。所有请求各自固定200 ns延迟，未使用按Engram行数倍增的延迟。

证据目录：`results/sue/axi-mpu-cycle-final-20261030`、`results/sue/axi-mpu-cycle-io-final-20261030`及`results/sue/axi-mpu-cycle-legacy-final-20261030`。

```bash
python3 simulators/sue/harness/v2/verify_axi_memory.py results/sue/axi-memory-check
```

DeepSeek集成冒烟：16卡、TP4、2 MPU、batch1、上下文128、一个代表块；新结果位于`results/sue/axi-mpu-cycle-deepseek-20261030`与`results/sue/axi-mpu-cycle-deepseek-none-20261030`。这不是完整模型TPOT。第9章当前矩阵采用逐AXI模型、1 ps精度、1 ns请求间隔及128条在途请求；此处历史机制快照不能替代第9章完整工作负载结果。
