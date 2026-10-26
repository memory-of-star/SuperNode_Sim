# SUE Scale-Up 配置指南

本指南从**外部视角**说明 SUE 仿真平台当前支持的配置项:每个配置项的作用、如何调整、以及如何观察仿真行为。设计基准与保真度说明见 [`DESIGN.md`](../DESIGN.md)(§9 配置设计 / §6.2 LLR 引擎 / §12 决策记录)。

---

## 1. 配置文件与拓扑文件路径约定

| 文件 | 路径(仓库内) | 作用 |
|---|---|---|
| 默认配置(参考) | `ns-3-alibabacloud/simulation/conf/sue-default.conf` | 全量带注释默认配置 |
| 样例拓扑 | `ns-3-alibabacloud/simulation/conf/topo-3node.top` | 3 节点:host0 -- sw1 -- host2(LLR+CBFC) |

配置经**入口函数**传入(构建 SUE 网络时自动 `SueReadConf` + 逐设备 `ApplySueConfig`):

```cpp
SueEntryDispatch (topoFile, confFile, &onTraffic);   // ENABLE_SCALEUP!=0 时建网
```

或独立解析、逐设备应用:

```cpp
SueReadConf (confFile);                              // 解析进 GlobalValue 注册表
SetupSueNetwork (topoFile, confFile);                // 或先建网再 Apply
ApplySueConfig (dev);                                // 每个 SueNetDevice 应用配置
```

### 配置语法(SimAI.conf 风格)

- `#` 起始为注释;空行跳过。
- 每行 `KEY VALUE`(空格分隔);数值支持十进制 / `0x` 十六进制。
- **未知键静默跳过**(不报错)——因此旧 conf 里的 `LLR_ENABLE` 等已移除键可以原样保留。

### 拓扑语法

```
type=SUE
nodes 3
switch 1
link 0 1 100Gbps 1 CBFC 0
```

`link` 行:`link <src> <dst> <rate> <llr_ena> <mode> <err_rate>`。

- `llr_ena`:0|1,**决定两端是否构造 LLR 引擎**(引擎存在与否以拓扑为准,§12.6)。
- `mode`:CBFC | PFC | OFF(链路级流控)。
- `err_rate`:写入 `LINK_ERROR_RATE_PER_LINK` 记录值(随机注入留 S5;当前用确定性 corruptor)。

---

## 2. 配置项总表

### 2.1 全局

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `ENABLE_SCALEUP` | 0 | 入口分发:1=构建 SUE 网络路径,0=走 scale-out 外部 setup | `SueGetNNodes()` 等访问器;是否建出 SUE 节点 |
| `ENABLE_SCALEUP_WORKLOAD` | 0 | 0=内置生成器(SueLiteEndpoint,§6.12);1=对接 astra-sim workload(接口预留 `sim_send/recv/schedule`,未接线——Start() 记 warn 并**回退内置生成器**,非 inert) | `SueLiteEndpoint` 发包统计;`NS_LOG` warn;`SueLiteEndpointTestCase` 断言两态 |

### 2.2 SUE EtherType 与 shim(§5.2)

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `SUE_ETH_TYPE_0` / `SUE_ETH_TYPE_1` | 0xFCF1 / 0xFCF2 | 对称 SUE 协议标识;帧按其中一种识别 | `SueConfig::HasShim()`、头解析往返 |
| `SUE_ETH_TYPE_0_HAS_SHIM` / `SUE_ETH_TYPE_1_HAS_SHIM` | 0 / 1 | 该 EtherType 的帧**按带 shim 解析**与否(每类型独立) | `SueConfig::HasShim (etype)` 读回 |

### 2.3 IFG(§5.4)

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `SUE_IFG_BYTES` | 12 | 帧间隔线字节数;按已设链路速率折算线时间 | 增大→链路有效吞吐下降 |

### 2.3.5 链路/转发时延(全铜缆互联场景;0=关闭保持基线)

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `SUE_LINK_PROPAGATION_NS` | 0 | 每条链路单程传播延迟(10ns/跳,铜缆;控制帧同享 → MTP lookahead=10ns);0=零传播基线 | 端到端延迟增加;MTP 分区 lookahead(§10.5) |
| `SUE_SWITCH_FWD_BUDGET_NS` | 0 | 交换机每跳转发预算:转发时延 + 出口 wire ≤ 预算;0=直发无处理时延(基线);拥塞排队另算 | 端到端延迟增加 |

### 2.4 LLR(UEC §5.1.4 两相使能 + 三行为)——本平台的重点配置面

原单键 `LLR_ENABLE` 已**移除**,按 UEC Table 5-9 拆为以下键:

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `LLR_MODE_LOCAL` | 1 | **RX 相使能**:1=可收 LLR_INIT(重同步 + 回 INIT_ECHO)、eligible 帧按 seq 过滤(缺序丢 + NACK);0=RX 直通不过滤 | `GetModeLocal()`;`rx_init_ctl_os`、`tx_init_echo_ctl_os`、`tx_nack_ctl_os` 计数 |
| `LLR_MODE_REMOTE` | 1 | **TX 相使能**:1=可发 LLR_INIT、eligible 帧分配 seq + 入 replay + gating;0=TX 走 BEST_EFFORT(不发 INIT、不 seq、不 replay,帧仍带 **ineligible preamble**) | `GetModeRemote()`;`tx_init_ctl_os` 计数;线上 preamble 为 eligible(B1=0x7+seq)还是 ineligible(B1=0x55) |
| `LLR_INIT_BEHAVIOR` | BLOCK | INIT 态对 LLR-desired 帧的处理:`DISCARD`(取走丢弃计 tx_discard)/ `BLOCK`(取帧排队、无丢包)/ `BEST_EFFORT`(直发不 seq) | `GetInitBehavior()`、`GetTxBehavior()`;INIT 期间 `tx_discard` 是否增长 |
| `LLR_FLUSH_BEHAVIOR` | BLOCK | 同上,FLUSH 态 | `GetFlushBehavior()`、`GetTxBehavior()` |
| `LLR_RE_INIT_ON_DISCARD` | 1 | FLUSH 之后:1=自动重入 INIT(发 INIT 等 INIT_ECHO);0=**停留 FLUSH**,管理方调 `Start()` 重入 | FLUSH 后 `tx_init_ctl_os` 是否 +1;`GetTxState()` 为 INIT 还是 FLUSH |
| `LLR_OUTSTANDING_SEQ_MAX` | 65536 | outstanding seq 上限,超限阻塞新帧 | `CanTx()` 为 false |
| `LLR_OUTSTANDING_DATA_MAX` | 1048576 | outstanding 数据量上限(B);UEC §5.1.6 **SHOULD = 链路 BDP** | 同上 |
| `LLR_REPLAY_BUF_MAX` | 1048576 | **重传缓冲存储容量(可缓存帧数)**,超限阻塞(不得截断 eligible 帧,UEC L303);须 ≥ 在途数据 + 闪断期发送帧量 | `CanTx()` 为 false;闪断选型见 `sue-llr-replay-buffer` |
| `LLR_REPLAY_TIMER_MAX_NS` | 0 | **replay_timer 周期(ns)= ACK 超时检测器**(UEC §5.1.4:新/重发帧 TX 时 arm,ACK 部分释放缓冲帧时重置=规则 4,DESIGN §12.24;到期重传未 ACK 帧;data_age_timer 才是 FLUSH 判决)。**默认 0 = 自动折算 10× LLR RTT**(`2×wire + 2×prop + ackPeriod`,DESIGN §12.25/§12.27,按链路速率缩放;100G≈22µs、400G≈20µs);显式 `>0` 则固定。随仓库 conf 预置按**计算后推荐值**直写(默认档 100G 21830ns、200G 20915ns、400G 20458ns,§2.7;公式与原理注明于各 conf 键注释;1G 档已随 §12.36 移除)。**勿设静态默认**:固定值 ≈ ACK RTT 时无谓重传成倍占带宽(1G 历史实证 0.73 Gbps,§12.25);固定值 < RTT 时每周期无谓重传 → replay_ct 耗尽 FLUSH 级联(s6-entry 1G 历史回归,§12.27) | 链路丢帧时 `tx_replay` 增长节奏(1G 历史:固定 50µs 时 `tx_replay`≈发送帧数) |
| `LLR_REPLAY_CT_MAX` | 5 | replay 次数上限;超限 → FLUSH;**全 1(255)= 永不触发**(仅 data_age / pcs_lost 可 FLUSH,UEC Table 5-10 `replay_ct_is_max`) | 持续丢帧时进入 FLUSH(`GetTxState()`) |
| `LLR_PCS_LOST_STATUS_TIMER_MAX_NS` | 0 | pcs_lost_status_timer_max(ns):PCS_status FALSE 持续超过该值 → **FLUSH**(即使 replay 缓冲为空)+ INIT 重同步。0=立即过期(UEC "0=immediate expiration");设非 0 阈值可区分**短/长闪断**(UEC Table 5-10,最小 PCS 模型见 scratch `sue-llr-pcs-lost`) | 闪断期间引擎 `GetPcsLostTimerExpired()`、`tx_discard`、`GetTxState()` |
| `LLR_INIT_TIMER_MAX_NS` | 0 | INIT 握手重试超时(独立于 replay 定时器);默认 0 = 自动折算 10×LLR RTT(同 `LLR_REPLAY_TIMER_MAX_NS`,DESIGN §12.27);显式 `>0` 则固定 | 无对端引擎时按 retry 上限超时 |
| `LLR_INIT_RETRY_MAX` | 3 | INIT 重发上限;超限仍无对端引擎 → GoAdvance 兜底 | 告警日志 |
| `LLR_DATA_AGE_TIMER_MAX_NS` | 50000 | 独立 buffer 驱动数据老化定时器;最老帧超龄 → FLUSH | 缓冲停滞时进入 FLUSH |
| `LLR_CTLOS_TARGET_SPACING` | 2048 | **引擎 ACK 周期**(线字节),按数据率折算;**取值钳制到 UEC Table 5-9 范围 [400,16384]**(400=最大速率界;16384<17296B 最小速率界)。UEC §5.1.6:以首个 INIT_ECHO 发出 = init_echo_sent 为进入 SEND_ACKS 的触发源,之后每周期一条累积 ACK,空闲周期重复末 seq;NACK_SENT 抑制。0 亦被钳到 400,不再表示"不节流"(旧 `LLR_CTLOS_MIN_INTERVAL_NS` 键已删除)。**设备 CtlOS pacing 门已与 ACK 周期解耦**(DESIGN §12.26):门固定 UEC §5.2.11.2 的 400B floor,不由本键驱动——ACK 流不再独占 pacing FIFO、低优先级 CF_UPDATE 不被 ACK 积压 | ACK 由 per-frame 变 periodic(累积 `next_rx_seq-1`);CtlOS pacing 门恒 400B floor |

> **默认值语义**:默认(两相都开 + BLOCK/BLOCK + 自动重入)精确复刻原 `LLR_ENABLE=1` 行为——**上层只见一条无丢包链路**。DISCARD / BEST_EFFORT / re_init=0 是留作可配置的选项。
>
> **线级保真(§5.4)**:数据帧**一律带 8B LLR preamble**——`LLR_ACTIVE` 发 eligible(B1=0x7+seq),BEST_EFFORT/`LLR_MODE_REMOTE 0` 发 ineligible(B1=0x55);RX 先剥 8B 再按 `LLR_MODE_LOCAL` 门控。仅 **CtlOS 控制帧**(ACK/NACK/INIT/INIT_ECHO、CF_Update)不装 preamble(经 `SueCtlTag` 分发先于 LLR)。**"前 400 字节不插入 CtlOS" 的 UEC 线级限制**:本平台 store-and-forward——CtlOS 在整帧之间原子发送(高优先 `m_ctlQueue`),**不可能插入帧内**,故帧内 2kB 间隔结构性 N/A;可仿真部分即 **400B 最小 CtlOS 间隔(最大速率界)**,由 TARGET_SPACING 钳制下限强制。
>
> **acked_seq(UEC Table 5-10)**:20 位寄存器 = 最近一次成功接收的 LLR_ACK/LLR_NACK 中确认的序列号(可经引擎 `GetAckedSeq()` 观测)。初值全 1(`SEQ_MOD-1`);`LLR_MODE_REMOTE` **改变时**重置全 1(**且 `tx_seq` 清零、replay 缓冲清空**);每次发 `LLR_INIT` 置 `init_seq-1`(init_seq=0 时即全 1)。因此首帧 seq=0 的 ACK 不会被误判为重复(不再有 `m_anyAcked` 兜底)。**outstanding_seq(UEC Table 5-10)** = `((tx_seq-1) - acked_seq) mod 2^20`(序列空间量,非缓冲长度;可经引擎 `GetOutstandingSeq()` 观测),因 mode 改变时 tx_seq/acked_seq/缓冲同步重置,该公式与 replay buffer 大小在**所有**状态下一致。

### 2.5 流控(CBFC / PFC)

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `FC_MODE` | CBFC | 流控总开关:`OFF` / `CBFC` / `PFC` | `GetFcController()` 返回的 facade 类型 |
| `CBFC_CREDIT_SIZE` | 256 | 信用粒度(B) | 帧所需信用数 = ceil(帧长/粒度) |
| `CBFC_VC_CREDIT_LIMIT` | 0:512,…,7:512 | 每 VC 信用上限(`vc:limit,...`);影响 `CanSend(vc, bytes)` | 发送被阻塞的帧长阈值 |
| `CBFC_PKT_OVHD` | 0 | MAC 帧额外开销(B,可负) | 信用扣减口径 |
| `CBFC_CC_MSG_TIMER_US` | 102 | CC_Update 生成周期(us;0=关闭);THU 对齐 9,790/s=102.1µs(§12.37);注册默认 100 | `SueCbfcCounters` |
| `CBFC_CF_THRESHOLD_BYTES` | 2075 | 数据量触发(SDK credit_threshold 对等物):累计释放信用 ≥ N 字节即上报,逐量子(帧长≥阈值时一次 drain 可触发多条,余量结转);CF 计数≈流量字节/N,与端口速率无关。THU 对齐:2048(SDK th=4×512B)+~27B 消息自身线时=2074.7B 实测等效 → 2075,100G 6,024,096/s vs THU 6,024,685(§12.37);0=关闭。注意 N ≤ 信用窗口(CL×creditSize)否则发送端在攒够阈值前停摆 | `SueCbfcCounters` |
| `CBFC_CF_MIN_TIMER_BYTES` | 0 | CF_min_timer(线字节)→ 按数据率折算(UEC §5.2.6.1 触发 a:`R_VC_CF` 更新且 CF_min one-shot 到期 → `SendCfUpdateBatch` 批量上报脏 VC);0=无引擎地板,阈值触发即时发送(THU 默认档行为;设备 400B 门封顶);>0 时 CF 节奏=1/CF_MIN(§12.37) | `SueCbfcCounters`;one-shot fire 时刻取绝对 `max(Now, last+CF_min)`、`arm-Now` 相对调度 → 与线程数无关 |
| `CBFC_CF_MAX_TIMER_BYTES` | 1250000 | CF_max_timer(触发 b:周期全量上报,防丢失;自 rearm + Cancel 未决 one-shot);线速下叠加阈值流 +0.66%;0=关闭 | `SueCbfcCounters` |
| `CBFC_EGRESS_BUF_BYTES` | 0 | 交换机 CBFC 端口出口 per-VC 缓冲深度门(B);**0=auto=creditLimit[vc]×creditSize**(每 VC 至多一个信用窗);>0=统一 per-VC 上限。出口→入口背压耦合(§6.7/§12.30):出口 VC 队列满 → 交换机挂起帧不退还入口信用 → 发送端被出口拥塞真实节流(2:1 incast 验证,§12.29/§12.30) | `SUESwitchNode` `GetCntPendPkts/GetCntPendMax`(挂起帧);发送端 CBFC 信用耗尽计数(incast 场景) |
| `PFC_PAUSE_QUANTA` | 0xFFFF | pause 帧 quanta(0xFFFF=暂停至显式 resume,§5.10) | PFC 恢复延迟 |
| `PFC_DYNAMIC_THRESHOLD` | 1 | 共享池动态阈值开(`PFC_SHARED_POOL_SIZE>0` 生效) | — |
| `PFC_BUFFER_SIZE` | 65536 | 接收缓冲(B) | 拥塞触发阈值 |
| `PFC_SHARED_POOL_SIZE` | 32768 | 共享池(B);0=关 | — |
| `PFC_HEADROOM_SIZE` | 8192 | 共享池 headroom(B) | 回退余量 |
| `PFC_RESUME_OFFSET` | 4096 | 共享池 resume 偏移(B) | — |
| `PFC_ALPHA_SHIFT` | 0 | 动态阈值 alpha 移位 | — |
| `PFC_PAUSE_THRESHOLD` | 32768 | 简单模式单 VC pause 阈值(B)(DYNAMIC_THRESHOLD=0 时用) | — |
| `PFC_RESUME_THRESHOLD` | 16384 | 简单模式单 VC resume 阈值(B) | — |

### 2.5.1 VC 映射(§7,通用流类别映射;端到端同一张表)

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `DSCP_TO_VC_MAP` | `0:0,1:1,…,63:7`(直映) | 64 项 shim DSCP→VC 映射表(`dscp:vc,...`)。**默认逐项直映 = `dscp&0x07`(8:0,9:1,…,63:7)**;**部分覆盖**:未列出的 dscp 保持直映;值 >7 钳到 7。host 发送/接收与交换机从同一张表解出同一 VC | 交换机 `GetCntVcRx(vc)`;host `GetRxByVc(vc)` |
| `PCP_TO_VC_MAP` | `0:0,…,7:7`(恒等) | 8 项 VLAN PCP→VC 映射表(`pcp:vc,...`);部分覆盖同上 | 同上 |
| `DEFAULT_VC` | 0 | 无 shim 且无 VLAN 的帧回退 VC | `SueConfig::DefaultVc()` |
| `DSCP_TO_VRF_MAP` | 空(全 VRF0) | 交换机 VRF 转发域(§12.40):shim DSCP→VRF 映射表(`dscp:vrf,...`);三源优先级 **DSCP > VLAN > 端口**,未命中试下一源,全部未命中 = VRF0;帧不携带 VRF(线上无字段) | `SUESwitchNode::GetCntVrfRx/Miss(vrf)`;帧按 VRF 走对应分段路由表 |
| `VLAN_TO_VRF_MAP` | 空(全 VRF0) | VLAN vid→VRF 映射表(`vid:vrf,...`,vid 0..4095);优先级次之 | 同上 |
| `PORT_TO_VRF_MAP` | 空(全 VRF0) | 入口端口→VRF 映射表(`portIdx:vrf,...`,portIdx = 交换机 AddPort 序 0..255);优先级最低 | 同上 |

> **端到端一致(§7)**:映射表必须主机与交换机遵守同一份配置——`sue-incast-2to1` 的多 VC/非恒等运行证明 host2 的 `GetRxByVc` 与交换机 `GetCntVcRx` 从线头解出同一 VC(§4.3)。
> **示例(非恒等)**:`DSCP_TO_VC_MAP 3:7,5:7` 把 DSCP 3 与 5 都汇到 VC7(默认表其余项仍直映);`DEFAULT_VC 5` 让无 shim/VLAN 的帧落 VC5。

### 2.6 多线程(S5a,§10.5)

| 键 | 默认 | 作用 | 如何观察 |
|---|---|---|---|
| `SUE_MTP_THREADS` | 1 | 线程数:1=单线程(不启用 `NS3_MTP`);>1=front-end 启用单进程多线程模拟器(`MtpInterface::Enable(n)` + `MultithreadedSimulatorImpl`,自动分区,每节点一逻辑进程/LP)。**sue 模块本身零 mtp 依赖**——仅注册并读回本键;真正启用 MTP 由 front-end(如 scratch `sue-mtp-equivalence`)在建网前完成 | 基线(threads=1)vs MTP-N 两次运行的**聚合结果 hash 逐字节一致**;`Simulator::Run()` 后各节点 `GetSystemId()` 分区表;`./run-s8-mtp.sh [N]` 双跑 diff hash → `S8 MTP EQUIVALENCE PASS` |

### 2.7 端口速率预置(100G/200G/400G)

仓库随附三个速率预置 conf:`conf/sue-default.conf`(100G 默认档)、`conf/sue-200g.conf`、`conf/sue-400g.conf`。**端口速率在拓扑 `link` 行**的 rate 字段,不在 conf(见 §1);预置 conf 只承载速率耦合键的推荐值。

**核心语义(UEC 字节键,§5.7/§9/§12.17)**:`LLR_CTLOS_TARGET_SPACING`、`CBFC_CF_THRESHOLD|MIN|MAX_TIMER_BYTES`、`SUE_IFG_BYTES` 都是**线字节**阈值,`ApplySueConfig` 按链路速率折算线时间——**控制开销比恒定、与速率无关**,因此三个预置的数值一致(唯一例外:400G 档 ACK 间距 2050B,NS 网格对齐 THU 实测,§12.37),各速率间只差拓扑速率与折算出的时间间隔:

| 键(线字节) | 100Gbps(默认档) | 200Gbps | 400Gbps |
|---|---|---|---|
| `LLR_CTLOS_TARGET_SPACING` 2048(ACK 周期;400G 档 2050) | 163.84ns | 81.92ns | 41.00ns |
| `CBFC_CF_THRESHOLD_BYTES` 2075(THU 默认档对齐) | 166.00ns | 83.00ns | 41.5→41ns |
| `CBFC_CF_MIN_TIMER_BYTES` 0(threshold 主导) | — | — | — |
| `CBFC_CF_MAX_TIMER_BYTES` 1250000(周期全量) | 100µs | 50µs | 25µs |
| `SUE_IFG_BYTES` 12 | 960ps | 480ps | 240ps |
| CtlOS 9B 线时 | 720ps | 360ps | 180ps |

(2026-08-13:ScaleOut 最小端口速率 100G,1G 列已移除,§12.36。2026-08-14:THU-SPEC 三速率对齐——CF 阈值 2075/CF_MIN 0/CC 102µs/400G ACK 2050,§12.37。)

要点:
- **子 ns 线时需 PS 分辨率**:100G+ 下 IFG/CtlOS 均 <1ns,入口 `SueEntryDispatch` 建网前自动升 PS(§6.8);直接调 `ApplySueConfig` 不经入口时须自行 `Time::SetResolution(Time::PS)`。
- **CF 数据量阈值主导(THU 默认档行为)**:`CBFC_CF_THRESHOLD_BYTES=2075`(2048 SDK 默认 + ~27B CF 消息自身线时)线速打流下 CF 速率 = 流量字节率/2075(100G 6.02M/s,THU 实测 6,024,685/s);`CF_MIN=0` 让阈值流不被地板钳制(>0 时 CF 节奏 = 1/CF_MIN);突发上限仍由设备固定 400B CtlOS 门封顶(2% 帽)。`CF_MAX=1.25MB` 周期全量防丢失,线速下叠加仅 +0.66%。
- **LLR 定时器预置按计算后推荐值直写**:三个预置的 `LLR_REPLAY_TIMER_MAX_NS`/`LLR_INIT_TIMER_MAX_NS` 均为**计算后的推荐值**(默认档 100G 21830ns、200G 20915ns、400G 20458ns = **10× LLR RTT** = `2×wire + 2×prop + ackPeriod`,DESIGN §12.25/§12.27,按链路速率缩放:100G≈22µs、200G≈21µs、400G≈20µs;各 conf 键注释注明公式与原理)。亦可填 `0`=代码自动折算(效果等同)。**replay_timer 是 ACK 超时检测器**(UEC §5.1.4 规则 2/3/4/6;ACK 部分释放缓冲帧时重置,§12.24),固定值必须随 RTT 关联:固定值 ≈ ACK RTT 时无谓重传成倍占带宽(1G 历史实证 0.73 Gbps,§12.25);固定值 < RTT 时每周期无谓重传 → replay_ct 耗尽 FLUSH 级联(旧 1000ns 默认的 s6-entry 1G 回归,§12.27)。显式 `>0` 仍可固定(仅当你有明确理由)。
- **高速率事件率注意(400G 最甚)**:2050B 的 ACK 节奏=41.0ns≈2439 万 ACK/秒/链路(sim 时间)。UEC 字节上限 16384B 只能放到 327ns(≈305 万/秒)。512 端口规模对事件率敏感时,把 `LLR_CTLOS_TARGET_SPACING` 提到 16384,或评估新增时间键方案(§9,偏离 UEC 字节语义)。CtlOS 线时(180ps@400G)≪间隔,无线上碰撞。
- **`LLR_OUTSTANDING_DATA_MAX` 默认 1MB**:400Gbps@1µs RTT 的 BDP=50KB,默认远超;µs 级 RTT 无需调。

---

## 3. LLR UEC §5.1.4 实验矩阵(推荐先跑这几组)

| 场景 | 改动 | 预期可观察现象 |
|---|---|---|
| 默认(零回归) | 不改 | 丢帧→重传→恢复,`tx_replay>0` 但**无上层丢包**;`rx_ok` 最终等于发送帧数 |
| 关闭 TX 相 | `LLR_MODE_REMOTE 0` | 不发 INIT(`tx_init_ctl_os==0`)、线上数据帧带 **ineligible preamble**(B1=0x55,不带 seq);帧不入 replay——链路丢帧时直接丢,`rx_ok < 发送帧数`。对端 RX 仍先剥 8B 再按 mode_local 门控(ineligible 不过滤直通),解析不再错位 |
| 关闭 RX 相 | `LLR_MODE_LOCAL 0` | 收 INIT 不回 INIT_ECHO(`tx_init_echo_ctl_os==0`)、错误帧不再 NACK(`tx_nack_ctl_os==0`),帧直通 |
| INIT 丢弃 | `LLR_INIT_BEHAVIOR DISCARD` | INIT 期间帧被取走丢弃,`tx_discard` 增长;握手完成后恢复 |
| INIT 直通 | `LLR_INIT_BEHAVIOR BEST_EFFORT` | INIT 期间帧不带 seq 直发(瞬时不保护窗口) |
| FLUSH 丢弃 | `LLR_FLUSH_BEHAVIOR DISCARD` | FLUSH 期间队列被排空丢弃 |
| 不自动重入 | `LLR_RE_INIT_ON_DISCARD 0` | 触发 FLUSH 后 `GetTxState()` 停留在 FLUSH、`tx_init_ctl_os` 不增长;调用 `Start()` 才重入 INIT |
| ACK 周期 | `LLR_CTLOS_TARGET_SPACING 4096`(或更大) | ACK 由 per-frame 变 **periodic 累积**(seq=`next_rx_seq-1`):以 init_echo_sent 为触发源,连续收 N 帧时 `tx_ack_ctl_os` 增长远小于 N(≈N×ACK周期/帧到达间隔);空闲周期重复末 seq(发送端 dup 跳过);NACK 仍即时单发、`NACK_SENT` 期间抑制 ACK |
| 短闪断(< timer_max) | `LLR_PCS_LOST_STATUS_TIMER_MAX_NS` 设非 0(如 100000) | PCS_status FALSE 时长 < timer_max:**无 FLUSH**,丢帧经 replay 恢复(`tx_discard==0`、`GetPcsLostTimerExpired()==false`);`sue-llr-pcs-lost` 场景 1 |
| 长闪断(≥ timer_max) | 同上阈值更小或闪断更长 | PCS_status FALSE ≥ timer_max:**FLUSH 丢弃在途帧**(`tx_discard`≥1)+ INIT 重同步(对端 `GetNextRxSeq()` 重对齐);空闲链路**空缓冲也 FLUSH**;`sue-llr-pcs-lost` 场景 2/3 |
| 缓冲选型:ACK 间隔 → 最小缓冲 | 设 `LLR_REPLAY_BUF_MAX` 扫描 + `LLR_CTLOS_TARGET_SPACING`(ACK 周期);`sue-llr-replay-buffer` Part 1 | 每 ACK 周期 A 的最小缓冲 `B_min(A) = 10000×(RTT+A)/100` 帧(100Gbps;逐帧→512、1µs→640、5µs→1152、20µs→3072;1G 历史:6/7/12/26);缓冲 < B_min → 吞吐跌(<0.99×全速率),**不 FLUSH**(无损背压) |
| 缓冲选型:指定缓冲 → 最大支持闪断 | 设 `LLR_REPLAY_BUF_MAX` + `LLR_PCS_LOST_STATUS_TIMER_MAX_NS`(定时器上限);`sue-llr-replay-buffer` Part 2 | 热态上限 `T_supported = min(B×100/10000−RTT, pcs_lost_max, data_age_max)`;闪断 ≤ T_hot → 恢复后"热"重传(`tx_discard==0`);超定时器上限 → **FLUSH(与缓冲无关,大缓冲 = 大丢弃)** |

---

## 4. 如何运行

### 4.1 构建

```bash
cd /home/sujinchi/ns-3-scaleup
./build.sh                # 首次配置 + 全量构建(-j8)
./build.sh --target libsue   # 只增量编译 sue 模块
rm -rf ns-3-alibabacloud/simulation/build && ./build.sh   # 改了头文件需重新配置时
```

> 构建脚本为本地工具链做了兜底(GCC 15 缺 `<stdint.h>`、Python 3.14 不兼容 `./ns3`),勿直接用 `./ns3` 或裸 `cmake --build`(详见 `build.sh` 头注)。

> **多线程(S5a,§10.5)**:`build.sh` 已并入 `-DNS3_MTP=ON`(单进程多线程模拟器)。`build.sh` 默认 `NS3_TESTS=OFF`;跑单元测试需先重配 `-DNS3_TESTS=ON`,且只建**定向目标**(`libsue-test`、`scratch_*`),勿建 `all`——NS3_TESTS=ON 会触发 fork 既有 bug(`src/mtp/test/mtp-test-suite.cc` include 不存在的 `ns3/mtp.h`)。重配命令:`cmake -S ns-3-alibabacloud/simulation -B ns-3-alibabacloud/simulation/build -DNS3_EXAMPLES=OFF -DNS3_TESTS=ON -DNS3_MTP=ON -DCMAKE_CXX_FLAGS="-include stdint.h"`。

### 4.2 单元测试(TestSuite,40 例)

```bash
./run-sue-tests.sh                        # 全量,verbose(每例结果)
./run-sue-tests.sh --suite=sue --xml=result.xml
```

### 4.3 Demo(scratch 程序)

scratch 是随仓库 force-track 的运行时验证程序(`ns-3-alibabacloud/simulation/scratch/`)。构建并运行单个:

```bash
cd /home/sujinchi/ns-3-scaleup/ns-3-alibabacloud/simulation
cmake --build build -j8 --target scratch_sue-entry-dispatch
./build/scratch/ns3.36.1-sue-entry-dispatch-default
```

常用 demo 一览(按阶段,逐级递进):

| 程序 | 验证点 | 观察入口 |
|---|---|---|
| `sue-afh-device-dispatch` | 模块实例化/挂接 | 断言 PASS |
| `sue-afh-forward` / `sue-afh-header-parse` | 转发 / 头解析 | 断言 PASS |
| `sue-cbfc-*` / `sue-afh-wire-time` | CBFC 端到端 / 引擎 / 时序 | 计数器 |
| `sue-llr-engine` / `sue-llr-flush-init` / `sue-llr-preamble-fcs` / `sue-llr-poisoned` / `sue-llr-timer-wakeup` | LLR 引擎、FLUSH、重传、POISONED、唤醒 | `GetTxState()`、LLR 计数器 |
| `sue-fc-*` | CC_Update、LLR+CBFC 共存、计数器 dump、FC 门控、PFC | `PrintCounters` |
| `sue-fc-ctl-onwire / sue-fc-frame-headers / sue-llr-pcs-lost` | 控制帧 / 头 / pcs_lost 闪断(`sue-llr-pcs-lost`:短闪断 replay 恢复、长闪断 FLUSH+re-INIT、空闲空缓冲 FLUSH) | 断言 PASS、`GetPcsLostTimerExpired()` |
| `sue-cfg-parse-apply` | 配置系统(键解析/应用;含新 LLR 键) | 断言 + GlobalValue 读回 |
| `sue-entry-dispatch` | 入口建网 + 拓扑 llr_ena(默认配置 100G;曾因静态定时器 1000ns < CtlOS 门+ACK 往返收到 2 帧,1G 历史回归,§12.27) | `SueGetDevice(..)->GetLlrEngine()->GetTxState()` |
| `sue-llr-replay-buffer` | LLR 重传缓冲选型:Part 1 ACK 间隔 → 最小缓冲;Part 2 指定缓冲 → 最大支持闪断(热/停传/FLUSH 三态);Part 3 定时器交互(replay_ct 墙钟 ∝ timer、data_age 覆盖大缓冲) | `S7 REPLAY-BUF-RTT PASS` |
| `sue-mtp-equivalence` | S5a 单进程多线程等价性:baseline(threads=1)vs MTP-8 **聚合结果 hash 逐字节一致**;12 逻辑进程自动分区;delivered=1600/1600、无 tx_discard、无 FLUSH;NACK(`SetCorruptOnce(100)`)与 FLUSH(`SetCorruptFirstN(10)`)双模式;每 host 互异 dither 出发时刻(§12.20)。等价判据 = **行为 hash**(`S8 BHASH`,掩 ACK/NACK CtlOS 计数层;FLUSH 全 hash 的 n8d4 `rx_ack` ±1 为残留 ACK 时钟决胜,非模型缺陷,§12.28) | `./run-s8-mtp.sh [N]` 双跑 diff hash → NACK/FLUSH `S8 MTP EQUIVALENCE PASS` |
| `sue-endpoint-ring` | S5b `SueLiteEndpoint` 端到端驱动(§6.12):8 host + 2 ToR + 2 spine 双平面 ECMP,全链路 LLR+CBFC;ring / all-to-all × VC0 / VC(shim)四组合;`ΣRx==ΣTx==N·8` 无丢包、无 LLR discard、无 FLUSH、按 VC/目的/源归因;每 host 互异 dither(§12.20);env:`S5B_THREADS/MODE/FRAMES/INTERVAL_US/VC/PAYLOAD/DITHER` | `./run-s5b.sh [N]` 双跑 diff hash → `S5B MTP EQUIVALENCE PASS` |
| `sue-thpt-line-rate` | Task B 双 GPU 吞吐(§12.24–§12.26):10 Gbit 载荷 × 400G / 100G,clean 与确定性 1/10000 帧损(`SetBadEvery`)双跑;LLR NACK-replay 恢复闭环;`LLR_REPLAY_TIMER_MAX_NS` **自动 10×RTT 推导**(THPT_REPLAY_NS=0;`2×wire+2×prop+ackPeriod`);门/ACK 周期解耦后线速验证。**规范结果(CREDITS=1024,默认)**:400G 391.7 Gbps、100G 97.9 Gbps(≈97.9% 线速;1G 历史 0.974,§12.36),loss 场景 replay==nack、无 discard、损失 <0.02%;**CREDITS<1024 是绑定约束**(512 → 91.75% 线速、credit_exhausted>0,信用窗口分析用)。env:`THPT_RATE/GBITS/PAYLOAD/INTERVAL_NS/LOSS_EVERY/THREADS/CREDITS/CFMIN/REPLAY_NS` | `./run-s5c-thpt.sh [RATE] [GBITS] [LOSS] [CREDITS]` → 各场景 `THPT PASS` + 吞吐;loss 场景 replay==nack |
| `sue-incast-2to1` | S5e CBFC 出口→入口背压(§12.29 发现/§12.30 实现):3 XPU 全 200G 口、host0+host1 双发 host2 的 2:1 incast,`CBFC_EGRESS_BUF_BYTES` 默认 0=auto(每 VC 至多一个信用窗);出口拥塞真实节流发送端(credit_exhausted>0)、交换机挂起(`GetCntPendPkts/GetCntPendMax`)、`delivered==sent` 无损、incast 窗 200G 线顶。**输出**:`INCAST RATES` 每 200µs 瞬时速率曲线(src0/src1 双入口 + egress 出口 Gbps)——1024 下 400µs–10.2ms 稳定 97.9/97.9/195.87、10.6ms 归零;**预期**:CREDITS=1024 → INCAST PASS、两源各 ~97.9G 50/50 分拆、loss=0;CREDITS=512 → 窗口饥饿+出口门**双背压并存**仍 PASS(183.5G=91.75% 窗顶)。**多 VC(§7)**:`[DSCP0] [DSCP1] [DSCP_TO_VC_MAP]` 位置参数经 env `INCAST_DSCP0/1/INCAST_DSCP_TO_VC_MAP` 注入(>0 的 DSCP 走 shim + `SetDscp`,映射表注入内联 conf)——PASS 下 host2 `g_rxByVc` 归因 == per-source、交换机 `GetCntVcRx` 解出同一 VC、双 VC `credit_exhausted>0`。env:`THPT_RATE/GBITS/CREDITS/CFMIN` | `./run-s5e-incast.sh [RATE] [GBITS] [CREDITS] [DSCP0] [DSCP1] [DSCP_TO_VC_MAP]` → `INCAST PASS` + 计数器签名(credit_exhausted/pend);多 VC 例:`./run-s5e-incast.sh 200Gbps 1 1024 0 3`(dscp 0/3 → VC0/VC3)、非恒等:`./run-s5e-incast.sh 200Gbps 1 1024 0 3 3:7`(dscp3 → VC7 端到端) |

### 4.4 用仓库提供的 conf / topo 写 demo

```cpp
#include "ns3/sue-entry.h"
...
SueEntryDispatch ("ns-3-alibabacloud/simulation/conf/topo-3node.top",
                  "ns-3-alibabacloud/simulation/conf/sue-default.conf", &onTraffic);
Simulator::Stop (Seconds (1));
Simulator::Run ();
```

换端口速率:**改拓扑 `link` 行的 rate 字段**(`100Gbps`/`200Gbps`/`400Gbps`),conf 换对应预置(§2.7):

```cpp
SueEntryDispatch ("my-topo-200g.top",
                  "ns-3-alibabacloud/simulation/conf/sue-200g.conf", &onTraffic);
```

---

## 5. 观察仿真行为的方法

- **计数器**:`dev->PrintCounters (stdout, Simulator::Now ())`;LLR 详细计数经 `dev->GetLlrEngine()->GetCounters()`(`rx_ok / rx_bad / rx_poisoned / rx_replay / rx_missing_seq / tx_ok / tx_replay / tx_discard / tx_ack_ctl_os / tx_nack_ctl_os / tx_init_ctl_os / rx_init_ctl_os / tx_init_echo_ctl_os / rx_init_echo_ctl_os / ...`)。CBFC 计数经 `dev->GetCbfcEngine()->GetCounters()`。
- **LLR 状态**:`dev->GetLlrEngine()->GetTxState()`(OFF / INIT / ADVANCE / REPLAY / FLUSH)。
- **LLR mode/行为**:`GetModeLocal()`、`GetModeRemote()`、`GetTxBehavior()`、`GetInitBehavior()`、`GetFlushBehavior()`(读回配置是否已落到引擎)。
- **统计上送**:链路丢帧重传的闭环表现看 `rx_ok` 与 `tx_replay` 的关系;上层是否感知丢包看 `tx_discard`。

---

## 6. 与旧配置的兼容

- `LLR_ENABLE` 已**移除**;旧 conf 中该行作未知键**静默跳过**,无需改动即可继续运行,但不再起任何作用——LLR 启停改由拓扑 `link` 行的 `llr_ena`(决定引擎是否构造)与 `LLR_MODE_LOCAL/REMOTE`(决定引擎内 RX/TX 是否生效)共同控制。
- `PFC_ENABLE` 已**移除**;PFC 开关唯一事实源为 `FC_MODE=PFC`。其余历史键同样静默跳过。
- `CBFC_CF_MIN/MAX_TIMER_BYTES` 已随 UEC §5.2.6.1 双触发模型(§12.2)**恢复**为已注册键(预置 0/1250000,THU 对齐 §12.37;0=关闭定时器回退逐帧直发)。
- `DSCP_TO_VC_MAP`/`PCP_TO_VC_MAP`/`DEFAULT_VC` 由"表中列出但未注册"(旧 conf 该行静默跳过)改为**注册 + 端到端生效**(§7/§2.5.1)。默认表 = 直映,与旧固化 `shim.DSCP&0x07 > VLAN.PCP&0x07 > VC0` 逐字节等价,故**未写这三行的旧 conf 行为不变**;写了非默认表的旧 conf 从"静默跳过"变"真正生效",需按新语义核对。`SueReadConf`/`Config::SetGlobal` 改键后须 `SueConfig::InvalidateCache()`(流量开始前应用)。
