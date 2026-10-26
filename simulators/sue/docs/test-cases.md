# SUE 仿真平台测试用例全集(101 例)

> 数据源:`src/sue/test/sue-test.cc`(TestSuite,CI 层)+ `scratch/sue-*.cc`(单测/场景层)。
> 规范:`docs/test-organization.md`(定位分工/命名)、`docs/uec-spec-matrix.md`(UEC 条款交叉引用)、`docs/test-report.md`(回归结果)、`DESIGN.md`(实现/评审唯一基准)。
> 用例 ID 为本文档唯一标识:TS = TestSuite(65 例),SC = scratch(36 个)。ID 固定,新增用例追加在尾。
> 每个用例按五段说明:**测什么**(被验对象/守护的缺陷)→ **场景**(拓扑/配置/注入/时序)→ **行为链**(仿真里一步步发生什么)→ **断言**(每条判据与它证明什么)→ **设计要点**(为什么这样构造、易错点)。

## 0. 测试体系速览

| 层 | 载体 | 数量 | 入口 | 判定方式 |
|---|---|---|---|---|
| TestSuite(CI) | `src/sue/test/sue-test.cc` | 61 | `./run-sue-tests.sh` | NS_TEST 断言,逐例 PASS/FAIL |
| scratch 单测 | `scratch/sue-<模块>-<功能点>.cc` | 34 | `./run-scratch-checks.sh` | 自检式 `PASS/FAIL` 判语 + CHECK |

设计原则(§test-organization):**同一功能点只保留一份权威实现**——协议状态机/纯引擎逻辑 → TestSuite;设备/网络路径/场景 → scratch;等价性与性能 → run-*.sh 脚本 + 行为 hash。

---

## 1. TestSuite 用例(TS-01 ~ TS-61)

### AFH 转发模块

**TS-01 AFH classes instantiable**
- 测什么:S0 脚手架——三个核心类能否被 ns-3 对象体系实例化(等价于 TypeId 注册成功、模块正确接入构建)。
- 场景:无拓扑,`CreateObject<SueNetDevice/SUESwitchNode/SueChannel>()`。
- 断言:三个 Ptr 均非空。
- 设计要点:全用例的基座;若此处失败,其余 57 例都会以更隐蔽的方式崩。

**TS-02 AFH attach registers two devices**
- 测什么:`SueChannel` 的设备注册表是否维护两端。
- 场景:两台 `SueNetDevice` Attach 到同一 `SueChannel`。
- 断言:`ch->GetNDevices()==2`。
- 设计要点:后续所有"发-收"场景的前提;`SueChannel` 仿 QbbChannel 自己维护注册(基座 `PointToPointChannel` 的成员在子类 Attach 后不更新,§6.13)。

**TS-03 AFH transmit dispatches to dst receive**
- 测什么:发帧静态分派到目的端 `SueNetDevice::Receive`(不经基座通道的 Receive 路径,§6.13)。
- 场景:直连两设备,dst 装 upcall,src 构造**合法 SUE 帧**(裸载荷会被非 SUE EtherType 守卫丢弃,E4)直接 `ch->TransmitStart(p, src, 0)`。
- 断言:upcall 触发 `g_rx==true`。
- 设计要点:合法帧构造是隐式前提——E4 防御层之后,不带 SUE EtherType 的包在 Receive 就被丢。

**TS-04 AFH header parse(4 combos, XPU-id boundaries)**
- 测什么:`SueHeader` 序列化 round-trip 的完整性(§5.2/§5.3)。
- 场景:纯内存序列化(AddHeader→PeekHeader),无网络。
  - XPU-id 边界:0 / 511 / 1023(10 位全范围,守位宽截断);
  - 组合 A:无 VLAN 无 shim → 14B,TTL=0;
  - 组合 B:VLAN(pcp=3/dei=1/vid=100)无 shim → 逐字段;
  - 组合 C:shim 2B(F=0,ttl=7/dscp=5)→ 用 `SUE_ETH_TYPE_1`(配置为 HAS_SHIM=1 的 etype);
  - 组合 D:VLAN+shim 3B(F=1 + FlowLabel=0x42)→ Size()==3、flowLabel 往返;
  - TTL 递减:3→2。
- 断言:每字段逐一 `==`;A 组合尺寸==14。
- 设计要点:shim 的有无由**逐 EtherType 配置**决定——组合 C/D 必须用 HAS_SHIM=1 的 etype,否则接收侧不解析 shim(隐式 bug 源)。

**TS-05 AFH routing LPM + ECMP**
- 测什么:路由表三条语义:LPM 最长前缀优先、ECMP 确定性/分布性、§12.33 流熵、边界钳制。
- 场景:纯路由表,不建网。装三条:精确 `0x1AB/10→{2}`、聚合 `0x100/8→{3,4}`(ECMP 组)、默认 `0/0→{5}`。
- 行为链与断言:
  - `Lookup(0x1AB)` → {2}(精确压默认);`Lookup(0x101)` → {3,4}(ECMP 组);`Lookup(0x3FF)` → {5}(默认);
  - ECMP 确定性:同 seed 同 xpu 两次 `GetOutPort` 相等;端口 ∈ {3,4};
  - 分布性:seed=0、扫 0x100..0x10F,两个端口都出现;
  - §12.33 流熵:`GetOutPort(x,0)==GetOutPort(x)`(0 熵与旧公式逐位一致)、同 (x,ent) 确定、ent=1..64 扫描覆盖 {3,4};
  - `AddPrefix(len=11)` 钳到 10;无路由 → miss。
- 设计要点:ECMP 断言只钉软性质(确定/分布/有效),**不钉具体端口值**——哈希实现可以演进;流熵三断言守护"旧单参路径逐位不变"的兼容承诺。

**TS-06 AFH forwarding(4 combos, TTL, miss)**
- 测什么:AFH 端到端转发 + 两种丢弃路径。
- 场景:`BuildFwdTopo`(host1—switch—host2,路由表 xpu1→p0、xpu2→p1),逐帧 `SwitchSend`:
  - A 无 vlan 无 shim;B vlan pcp=2;C shim 2B ttl=3;D vlan+shim 3B ttl=4;E shim ttl=1;F 目的 xpu=999(无路由)。
- 行为链:交换机解析 → XPU-id LPM → shim TTL 递减(A/B 无 shim 不递减)→ 转发。
- 断言:host2 收到 A-D 且字段正确(C ttl 3→2、D ttl 4→3、shim 尺寸);E 收不到(TTL 耗尽丢弃);F 收不到(lookup miss)。
- 设计要点:TTL 与 miss 用"收不到"作负断言,与 TS-07 的计数器正断言互补。

**TS-07 AFH counters(rx tx lookup_miss ttl_zero_drop vc)**
- 测什么:交换机五类 AFH 计数的语义(§6.11):什么该计、什么不该计。
- 场景:同 TS-06 拓扑,`ResetCounters` 后逐帧:
  - 正常转发(shim dscp=5):rx=1/tx=1/vc5 rx=1/vc5 tx=1/miss=0/ttl_drop=0;
  - TTL=1:ttl_drop=1 且 **tx 不增**(丢弃帧不计转发);
  - miss:lookup_miss=1;
  - VLAN pcp=2 无 shim:vc2 rx=1。
- 断言:上述逐项。
- 设计要点:本用例是 `dscp&0x07` 默认映射的 **CI 哨兵**(dscp=5→vc5)——TS-47 承诺每次场景后恢复默认映射表,就是为了保住这里的 5→5。

**TS-08 AFH wire-time + IFG**
- 测什么:线时/IFG 建模(§5.4)——含 8B preamble 的字节折算与帧间隔。
- 场景:直连,100Gbps,IFG=1ns(12B@100G),两帧背靠背;接收时刻用 upcall 时间戳记录。
- 断言:首帧@wireTime(100B@100G=8ns);次帧@wireTime+IFG+wireTime(设备同源 CalculateBytesTxTime 量化)。
- 设计要点:收帧时刻==wireTime(而非 0)证明发送侧按字节数折算线时;第二帧的偏移证明 IFG 在帧间生效;断言用设备同源 `CalculateBytesTxTime` 路径——NS 量化下两侧逐位相等(8ns/1ns)。

### CBFC 模块

**TS-09 CBFC engine(credit exhaust CF CC)**
- 测什么:信用账务核心循环——计费/耗尽/退款/泄漏回收,零 wire 依赖。
- 场景:引擎裸栈(非 CreateObject),CL=5、CreditSize=100、100B 帧=1 信用。
- 行为链:CanSend→OnTx×5 → CU=5 → CanSend false;VC1 独立(CanSend true);接收端 OnRx+OnDrain×5 → OnCfUpdate(对端 R_VC_CF)→ CU=0;再经 OnCcUpdate 泄漏回收 → CU=0。
- 断言:CU 的每一步。
- 设计要点:8-VC 独立性在此断言——一个 VC 耗尽不得影响其它 VC 的信用检查。

**TS-10 CBFC CC_Update LostCredits<0 resync-only**
- 测什么:环回比较防护(§12.31 A2,UEC §5.2.5.7.4)——`LostCredits<0` 是严重错误,不得把回绕巨值折进 R_VC_CF。
- 场景:引擎级。构造 R_VC_CC 领先 S_VC_CC(例如窗口内 CC_Update 的后遗症):B.OnRx 计 1,B 收 `cc[0]=0`(发送端 S_CC=0)快照 → lost=(0−1) mod 2^20 = 巨值。
- 断言:R_VC_CF 保持 0(不被污染,仅重同步);正向路径(lost=1)正常回收。
- 设计要点:钉死 `0 < lost < CC_MOD/2` 半量程判据——负值在 2^20 环上表现为 ≥2^19 的巨值,必须识别为错误而非信用。

**TS-11 CBFC e2e(gating + CF_Update recovery)**
- 测什么:信用门控的**时序证据**——门控不是靠终态计数,而是靠送达时刻。
- 场景:直连,CL=5、10 帧突发、接收端 drainDelay=10µs。
- 行为链:前 5 帧即时发出 → CU 耗尽 → 后 5 帧排队 → 10µs 后排空 + CF_Update → 窗口重开 → 剩余 5 帧发出。
- 断言:10 全送达、S_CC=10、CU=0、**第 6 帧送达时刻 >10µs**(第一批之后、退款之前不可能有第 6 帧)。
- 设计要点:若只断言"全送达+CU=0",一个从不门控的实现也能通过——时刻断言把"门控发生过"钉死。

**TS-12 CBFC TTL-drop refunds full-frame credit(no leak)**
- 测什么:回归 §12.31 A1——defer 退还下 TTL=0 丢弃必须按**含头完整帧**退还,否则跨信用边界的帧每丢一帧漏 1 信用。
- 场景:host—switch(p0/p1)—host,发送端 CL(5)=10,CreditSize=256;帧=254B 载荷+16B 头=270B 线上。
- 行为链:计费 `ceil(270/256)=2` 信用;TTL=1 帧在交换机丢弃;defer 路径显式 `RefundCredit`。
- 断言:ttl_drop=1;`charge(2)==refund`;CU=0。
- 设计要点:帧长故意构造为**跨信用边界**(剥头后 254B=ceil(254/256)=1)——若退还按剥头后字节,每帧漏 1 信用且测试立现;这是"用精确数字钉死字节口径"的典型构造。

### LLR 模块

**TS-13 LLR preamble + FCS drop**
- 测什么:preamble 恒在(§5.1.1.1)与 FCS 门在 LLR 过滤之前。
- 场景:双端 LLR,`SetBadEvery(2)`(确定性第 2/4 帧坏),4 帧。
- 行为链:好帧剥 8B preamble → LLR 过滤(accept)→ upcall;坏帧 RX 即丢(不 NACK,后续场景才测重传)。
- 断言:共收 2 帧。
- 设计要点:此用例只测"收发管线 + 坏帧门",故意不配 replay 场景——单变量隔离。

**TS-14 LLR NACK -> retransmit**
- 测什么:LLR 恢复主循环:NACK → REPLAY → 重传 → ACK → ADVANCE。
- 场景:`SetCorruptOnce(2)` 只坏第 2 帧,3 帧突发。
- 行为链:B 收 seq=1 坏帧 → NACK(1);A 从 replay buffer 重传 seq=1 → B 接受;ACK 释放缓冲 → ADVANCE。
- 断言:3 全送达、`TxState==ADVANCE`、`replay buffer 空`。
- 设计要点:一次性坏帧保证**只有一个 NACK 轮**,终态可精确断言(不是"最终会恢复"的弱断言)。

**TS-15 LLR FLUSH not lifted by late ACK(re_init_on_discard=0)**
- 测什么:回归 §12.31 A3——FLUSH 态不能被迟到的合法 ACK/NACK 拉回 ADVANCE(会绕过 INIT 重同步,FLUSH↔ADVANCE 振荡)。
- 场景(引擎级双场景对照):
  - (a) re_init=0、replay_ct_max=0 → 首轮 replay 即 FLUSH;注入"通过 ok 公式"的迟到 ACK(0);
  - (b) replay_ct_max=255,两帧入缓冲,NACK(0)→ REPLAY,ACK(1)→ 缓冲空 → ADVANCE,`tx_replay==1`。
- 断言:(a) 必须**停留 FLUSH**;(b) 正常完成路径不受影响。
- 设计要点:负向(不该动的别动)与正向(该动的还得动)成对出现——只修 bug 破坏正常路径是这类修复的常见副作用,双场景把两个方向都钉住;`tx_replay` 在此按 UEC 语义只数"完成的 REPLAY 退出"(FLUSH 会话不算)。

**TS-16 LLR partial ACK wakes TX(no stop-and-go)**
- 测什么:回归 §12.31 B1——ACK **部分**释放窗口(缓冲非空)时必须唤醒 TX,否则吞吐退化为按窗口粒度的停-走。
- 场景:窗=4(outstandingSeqMax)、B 端累积 ACK 周期 1ms、6 帧突发。先跑 10µs 完成 INIT 握手,再压帧。
- 行为链:seq 0-3 入窗口发出(在途 4),seq 4/5 排队;t=1ms 累积 ACK(2) 释放 seq 0-2 → 缓冲仍剩 seq3 → **TxOpportunityCb 触发** → DequeueAndTransmit 立即发 seq4/5;若无唤醒源,seq4/5 要等 2ms 的 ACK(3) 清空缓冲才走。
- 断言:t=1.05ms 时 6 帧全送达。
- 设计要点:用 ACK 周期(1ms)与窗口(4)的**不对齐**构造部分释放场景;1.05ms 观察点把"唤醒源缺失"变成 950µs 的送达时延差,一眼可判。

**TS-17 LLR PFC pause does not stop replay(UEC 5.1)**
- 测什么:UEC §5.1 明文——pause 不阻止 LLR 重放,只作用于新数据帧。
- 场景(引擎直驱,不经设备出队路径):两帧入 replay buffer;发送端 PFC VC0 **pause**;注入 NACK(0) → REPLAY 重传 seq1。
- 断言:pause 保持下首帧重传**必须送达**(g_rxCount==1),随后立即 Stop(防 replay 定时器重触发污染)。
- 设计要点:接收端 mode_local OFF 使重传帧不被 seq 过滤,把测试变量收窄到"pause 门"一个;修复前重传路径带着 `FcPaused(vc)` 门,本用例直接钉死其移除。

**TS-18 LLR FLUSH + INIT recovery**
- 测什么:FLUSH 后链路自愈的端到端闭环。
- 场景:`SetCorruptFirstN(3)` + replay_ct_max=2:seq0 三次损坏 → 两轮 replay 耗尽 → FLUSH(1 帧丢弃);t=300µs 起再发 2 新帧。
- 行为链:FLUSH → 自动重 INIT → INIT_ECHO 使 B 的 next_rx_seq 重同步为 3 → 新帧(seq 1,2)被接受。
- 断言:tx_discard==1、B next_rx_seq==3、2 帧送达。
- 设计要点:新帧延迟 300µs 发出,保证在 INIT 握手完成之后注入,不撞上重同步窗口。

**TS-19 LLR POISONED + 22 counters**
- 测什么:POISONED 帧语义(§5.1.10)——毒帧被**接受**(seq 前进),不 NACK 不重传。
- 场景:`SetPoisonOnce(2)` 只毒第 2 帧,3 帧。
- 断言:3 全送达;rx_ok=2、rx_poisoned=1、rx_expected_seq_poisoned=1、tx_nack=0(毒帧不触发 NACK)、B next_rx_seq=3、A tx_replay=0、A rx_ack=3。
- 设计要点:POISONED 与 BAD 的区分正是计数语义的核心——BAD 帧计数 rx_bad+触发 NACK,POISONED 帧走正常接收路径但带中毒标记。

**TS-20 LLR INIT handshake(echo -> ADVANCE; timeout x retry -> GoAdvance)**
- 测什么:退出 INIT 的两条路径都要有——正常 INIT_ECHO 与超时兜底(§6.2/§12.10)。
- 场景:引擎对引擎,init sink 直接回调 peer 的 OnInit(设备级 wiring §6.2 已保证,这里只测引擎语义)。
  - 场景 1:A/B 互接 sink,A Start → INIT → B echo → ADVANCE;
  - 场景 2:无对端,init 定时器 10µs + retryMax=2。
- 断言:场景 1:四计数各 1、A==ADVANCE、B==OFF(纯接收端不进 INIT);场景 2:tx_init==3(1 首发+2 重试)、GoAdvance 兜底、rx_init_echo==0。
- 设计要点:兜底路径保证无对端引擎时链路仍能投入使用(BEST_EFFORT 降级),而不是永久卡 INIT。

**TS-21 LLR two-phase modes + init and flush behaviors + re_init(UEC 5.1.4)**
- 测什么:两相使能(mode_local/remote)、三行为(DISCARD/BLOCK/BEST_EFFORT)、re_init 语义、配置键接线,五段一网打尽。
- 场景:
  - A:mode_remote OFF → Start 不发 INIT、TX=BEST_EFFORT、直接 ADVANCE;
  - B:mode_local OFF → OnInit 不处理(不计数不 echo)、BAD 帧 ACCEPT 不过滤、不 NACK;
  - C:init_behavior 默认 BLOCK;DISCARD 时 `DiscardNew()` 计 tx_discard;BEST_EFFORT;未知串回退 BLOCK;
  - D1:re_init=FALSE → replay 耗尽 FLUSH 后**不发 INIT**、停留 FLUSH;管理方 Start() → 重入 INIT 发 INIT;
  - D2:re_init=TRUE → FLUSH 自动重入 INIT(tx_init==2:初发+重发);
  - E:配置接线:5 个 GlobalValue → ApplySueConfig → 引擎字段逐项核对。
- 断言:每段如上;**末尾恢复全部默认 GlobalValue + Simulator::Destroy 清挂起定时器**(防串扰其它用例)。
- 设计要点:E 段证明 conf 键到引擎的真实接线(键存在但没接线是历史 bug 模式);恢复默认是本用例的纪律——55 例共享一个进程,残留状态会污染后续。

**TS-22 LLR next_rx_seq_vld gating(UEC 5.1.7)**
- 测什么:expected_frame 判据的完整真值表:`vld && (seq==next_rx_seq)`。
- 场景:引擎级。
  - INIT 前:vld=FALSE → eligible 帧(seq=0,fcs=0)DROP:不计数、不 NACK、next_rx_seq 不动;
  - OnInit(0)→ vld=TRUE → 同帧 ACCEPT:rx_ok=1、seq 前进;
  - mode_local OFF:直通,不查 vld——BAD 帧也 ACCEPT。
- 断言:每步的判定结果 + 计数。
- 设计要点:vld 门保证握手完成前接收端对乱序/重放帧免疫;local OFF 的直通路径是"关闭 RX 相"实验的前提。

**TS-23 LLR INIT_ECHO init_seq match(UEC 5.1.9.1.2)**
- 测什么:INIT_ECHO 校验——`init_seq==tx_seq` 才退出 INIT;错 echo 只计数不影响状态。
- 场景:INIT(seq=0)后依次注入:错 echo(42)→ 正确 echo(0)→ tx_seq 前进后旧 echo(0)。
- 断言:错 echo:rx_init_echo 计 1、仍 INIT;正确 echo:计 2、ADVANCE;旧 echo:计 3、仍 ADVANCE(不倒退)。
- 设计要点:"收到即计、校验决定迁移"两件事分离——计数语义(SP-16)与状态机语义互不干扰。

**TS-24 LLR ineligible preamble(BEST_EFFORT TX, unfiltered RX)**
- 测什么:回归"关闭 TX 相实验解析错位"——数据帧恒带 8B preamble(§5.1.1.1)后,接收端剥 preamble 再解析,non-LLR 发送端(BEST_EFFORT、ineligible preamble)的帧不再被误解析。
- 场景:A mode_remote OFF(发 ineligible,B1=0x55)、B 正常(先剥 8B 再按 mode_local 过滤);1 帧。
- 断言:B 收到且 SUE 头解析正确(xpu=2)、next_rx_seq 不动(ineligible 不过滤)、B 不 NACK、A 全程无 INIT。
- 设计要点:修复前 B 把 preamble 当 SUE 头解析导致 xpu 错乱——本用例以"xpu==2"钉死解析正确性,以"next_rx_seq 不动"钉死 ineligible 不过滤。

**TS-25 LLR periodic ACK(UEC 5.1.6)**
- 测什么:ACK 节奏模型——周期累积 ACK(next_rx_seq-1)vs 逐帧 ACK。
- 场景:引擎级,ackPeriod=50µs:OnInit(0) 触发 SEND_ACKS(节奏由 **init_echo_sent** 触发,§5.1.6),3 帧入周期。
- 行为链与断言:周期边界前 tx_ack==0(有节奏时不做逐帧 ACK);跑到 60µs:tx_ack==1、ack sink 一次、**累积 seq==2**(next_rx_seq-1,覆盖式);nack==0。第二段 ackPeriod=0(默认)→ 逐帧 ACK×3。
- 设计要点:累积语义是"覆盖式一条"而非"3 条"——这是与逐帧模式的本质区别,也决定了空闲链路重复 ACK 的行为(TS-26 的配平前提)。

**TS-26 LLR ACK-NACK counters pair across link**
- 测什么:回归 LLR-SP-16——rx_ack/rx_nack 必须对**收到的每一条** CtlOS 计数(超范围/重复都计),否则空闲链路两端计数永不配平。
- 场景:
  - 引擎级:ACK(0) 有效 → 重复 ACK(0) → 超范围 ACK(SEQ_MOD/2+10):rx_ack==3、seq_error==1、无 replay;
  - 设备级:空闲链路 + ackPeriod=10µs(周期重复 ACK),跑 55µs:对端 rx_ack == 本端 tx_ack(等式)。
- 断言:如上。
- 设计要点:修复前 rx_ack 只计"首次见到"的 seq,空闲链路的重复累积 ACK 让 rx_ack 恒为 1 而 tx_ack 持续增长——等式断言把"收到即计"钉死;重复 ACK 的"处理跳过"由 TS-27(c) 另行钉住,两用例分工明确。

**TS-27 LLR acked_seq init(UEC Table 5-10)**
- 测什么:`acked_seq` 字段(最近一条成功 ACK/NACK 确认的 seq)的三条初值规则与首个 ACK 的接受语义。
- 背景:acked_seq 初值若取 0,与第一帧的 seq=0 相同 → 首 ACK(0) 会被 dup 判据(`seq==acked_seq`)误判为重复而跳过处理 → **replay buffer 永不释放、首帧永远在途**。UEC Table 5-10 规定复位值为**全 1**(SEQ_MOD-1=1048575)。
- 场景(三段):
  - (a) INIT 发送:Start() 发 INIT(init_seq=tx_seq=0)→ acked_seq=init_seq-1=**全 1**;3 帧入缓冲(tx_seq=3)后管理方 Start() 重入 INIT(init_seq=3)→ acked_seq=**2**;
  - (b) mode_remote 变化(→OFF 与 →ON 各一次):acked_seq 重置全 1、**tx_seq 归 0**、缓冲清空(outstanding_seq==0);中间还验证 outstanding_seq 公式 `((tx_seq-1)-acked_seq) mod 2^20` 与 replay 尺寸一致(1 帧在途时 ==1);
  - (c) 首 ACK 语义:ACK(0) → 接受(acked_seq=0、计数 1、缓冲释放);再来一条 ACK(0)→ 真重复:acked_seq 不动、缓冲不重放、**但仍计数**(SP-16 收到即计)。
- 断言:上述每一步的精确值。
- 设计要点:全 1 初值根治首帧碰撞后,**删除旧 m_anyAcked 守卫**——本用例 (c) 段同时钉住"首 ACK 不误判 dup"与"真 dup 仍跳过处理",保证修复不引入反向错误。

**TS-28 LLR pcs_lost_status_timer(UEC Table 5-10)**
- 测什么:最小 PCS 闪断模型——pcs_lost_status_timer 的超时→FLUSH→重 INIT 链,以及它与 replay_ct 判据的**解耦**。
- 场景(引擎级双场景):
  - (a) 短闪断:PCS FALSE 20µs 后恢复(max=50µs)→ 定时器取消、无 FLUSH、ADVANCE 保持;
  - (b) 长闪断:PCS FALSE 保持 ≥50µs → 定时器超时 → FLUSH(**缓冲为空也 FLUSH**——这是与 replay_ct_max 只数重放轮的区分行为)→ 自动重 INIT。
- 断言:(a) 无 FLUSH、定时器未过期;(b) 定时器过期、状态 INIT、空缓冲下 tx_discard==0(FLUSH 但无可丢)。
- 设计要点:闪断期间数据平面不可解码(设备级丢帧由 SC-20 验证),引擎级的责任只是"计时→FLUSH→重同步";空缓冲 FLUSH 是规格要求(闪断后链路状态必须重置,即使没有在途帧)。

**TS-29 LLR status register(UEC Table 5-10)**
- 测什么:`LLR_status` 寄存器的完整真值表。
- 场景:引擎级,状态逐一走:OFF → INIT → ADVANCE → REPLAY → mode_remote OFF → PCS 开关。
- 断言:OFF=FALSE、INIT=FALSE、ADVANCE=TRUE、REPLAY=TRUE;mode_remote OFF 时 LLR_status **跟踪 PCS_status**(PCS FALSE → FALSE,恢复 → TRUE)。
- 设计要点:寄存器是管理面观测口,每个状态的取值都有规格含义(TRUE=链路在无损模式);pass-through 分支是 mode OFF 时"无 LLR 状态可言"的合理退化。

**TS-30 LLR replay_ct_max=255(UEC Table 5-10)**
- 测什么:replay_ct_max=255(全 1)是哨兵值——**计数上限永不触发 FLUSH**,仅 data_age/pcs_lost 可触发。
- 场景:引擎级,max=255、replay 定时器 10µs、data-age 设 10s(排除干扰);NACK → REPLAY 后跑 500µs(约 50 次 replay 定时器触发)。
- 断言:500µs 后仍 REPLAY、tx_discard==0。
- 设计要点:预判式 `replay_ct >= max` 语义(先判后增)使 255 永不满足;50 次触发远超任何合理 max,负向证据充分。

**TS-31 LLR replay-buffer gates(storage, BDP, window)**
- 测什么:CanTx 的三个独立门限——存储容量(帧数)、在途字节(BDP)、在途窗口(seq)——各自闭-开。
- 场景:引擎级,2 帧(86B)入缓冲后逐门限:
  - storage:`SetReplayBufMax(1)` → 关;恢复 2^20 → 开;
  - byte:`SetOutstandingDataMax(150)`(2×86=172B>150)→ 关;恢复 → 开;
  - seq:`SetOutstandingSeqMax(1)`(2 在途>1)→ 关;恢复 → 开;
  - ACK(s0+1) 释放两帧 → 全开、缓冲空。
- 尾段:配置接线——`LLR_REPLAY_BUF_MAX` GlobalValue 注册存在性 + Bind(1)+ ApplySueConfig 后 2 帧被 storage 门挡住;末尾恢复默认。
- 断言:每步 CanTx 的开关 + 尾段配置生效。
- 设计要点:三门限语义不同(闪断吸收容量 vs BDP 上界 vs 在途窗),历史上被混为一谈——逐个独立证闭证开,防止任一门的实现退化被另两个掩盖。

### FC 共存 / PFC / 门面

**TS-32 FC LLR+CBFC coexistence(nine rules 1 2 4)**
- 测什么:九规则中由结构保证的三条(§5.2.9):规则 1(重传不重复计费)、规则 2(LLR 丢弃帧不计数)、规则 4(排空退款正常)。
- 场景:LLR+CBFC 双开直连,CL=10、坏第 2 帧(`SetCorruptOnce(2)`)、drainDelay=10µs、3 帧。
- 行为链:seq1 坏 → NACK → 重传(重传路径**不调 OnTx**,规则 1 结构保证)→ 好帧排空 → CF_Update。
- 断言:3 全送达;S_VC_CC==3(重传未重复计费);S_VC_CU==0(全退还);R_VC_CF==3(3 好帧排空,坏帧未计入);LLR ADVANCE。
- 设计要点:一次坏帧同时覆盖三条规则的真值——比三条分别造场景经济且不失真。

**TS-33 PFC frame header(real 802.1Qbb 64B)**
- 测什么:PFC 帧的**字节级线上格式**(§5.10)——不是语义近似,是真实 64B 以太网帧。
- 场景:构造 `SuePfcHeader`(MACSA 已知、quanta[3]=500),序列化到 Packet 后逐字节检查。
- 断言:总长 64;MACDA 逐字节 == 01-80-C2-00-00-01(慢协议组播);EtherType==0x8808;Opcode==0x0101;FCS(CRC-32 覆盖前 60B)非零;round-trip 后 PEV 位 3、quanta[3]=500、MACSA 一致。
- 设计要点:FCS 只覆盖 60B 帧体(标准以太网 FCS 语义)——若实现把 FCS 覆盖到 64B 全帧,线格式就是错的。

**TS-34 CBFC CC_Update frame header(real 64B + 20b pack)**
- 测什么:CC_Update 帧的字节级格式,重点是 **20 位 CC 字段的 MSB-first 连续打包**。
- 场景:cc[0]=3、cc[1]=0xABCDE、cc[7]=0xFFFFF;序列化后逐字节检查;再单 VC 隔离检查。
- 断言:64B;EtherType 0x8808、Opcode 0xFFFE、CID=FA-7A-CB、MsgType=0x01;20 位字段从 byte 20 起连续排布——**byte 22 == 0x3A**(= cc0 的 0x30 | cc1 低位的 0x0A,手工逐位推导);单 VC 时 byte 22 == 0x30;SetCc 21 位值掩到 20 位;round-trip 三值一致。
- 设计要点:两个 VC 的位域跨字节边界**重叠在同一个字节**(byte 22),任何位序/边界错误都会让 0x3A 对不上——这是位打包最易错点的最强钉法。

**TS-35 LLR CtlOS min-interval pacing**
- 测什么:CtlOS 节流门(§5.7):突发被摊开,**全部送达**且间距 ≥ 配置间隔。
- 场景(两变体):①显式 `SetCtlOsMinInterval(1µs)` + 5 连发 ACK;②`SetCtlOsTargetSpacing(2048)` 线字节 @100Gbps=~164ns(NS 截断 163)+ 3 连发,界取设备同源 CalculateBytesTxTime(2048)-1ns。观察点:channel `SueRx` trace 筛 LLR_ACK tag 的时间戳。
- 断言:全送达(5/3 条)+ 相邻间隔 > 门限(留 1ns 容差)。
- 设计要点:"全送达 + 间距下界"双断言——节流不能靠丢帧实现(丢帧是错误模型的事);两变体覆盖时间键与线字节键两条配置路径。

**TS-36 CBFC CC_Update on-wire leak recovery**
- 测什么:信用泄漏的**发现-回收闭环**(九规则 8/9 的端到端场景)。
- 场景:直连,LLR+CBFC,CL=100,CC_Update 周期 5µs;`SetCorruptFirstN(3)` + replay_ct_max=2 → seq0 FLUSH。
- 行为链:发送端 seq0 计费 1 信用(OnTx)但接收端从未接收(FLUSH 丢弃)→ **1 信用泄漏**(B 的 R_CC < A 的 S_CC);CC_Update 周期把 A 的 S_CC 带给 B → B 算出 lost=1 → 补进 R_CF → CF_Update 回 A → CU 归零;300µs 后 2 新帧正常投递。
- 断言:2 帧送达、tx_discard==1、**CU==0(泄漏已回收)**、S_CC==3。
- 设计要点:无 CC_Update 时泄漏会让窗口永久缩小 CL/泄漏 帧后停摆——本用例证明周期 CC_Update 是泄漏的自愈机制;1ms Stop 兜底 CC_Update 是周期性的。

**TS-37 FC LLR+CBFC counters**
- 测什么:计数系统的端到端联动(§6.11)——UEC Table 5-13 关键项在真实收发链上的值。
- 场景:直连,LLR+CBFC,CC_Update 50µs,坏第 2 帧,3 帧。
- 断言:3 送达;A rx_ack≥3、rx_nack==1;B tx_nack==1、rx_bad==1、rx_expected_seq_bad==1;A tx_replay≥1;A cc_update_tx≥1。
- 设计要点:与 TS-19(引擎级计数)互补——此处计数经过设备接线/定时器/线格式全链路;rx_bad 与 rx_expected_seq_bad 的区分(坏帧被丢弃 vs 期望帧损坏)在此都有非零值可验。

**TS-38 FC facade(CBFC mode)**
- 测什么:门面路径(§6.5)——设备数据面实际走 `SueFcController`,必须与直连引擎等价。
- 场景:同 TS-11 的 10 帧/CL=5 场景,但引擎包在 `SueFcController` 里经 `SetFcController` 接线。
- 断言:GetMode()==CBFC、设备持有控制器、10 全送达、S_CC==10、CU==0。
- 设计要点:历史 bug 模式是"引擎直连好用、经门面断线"——门面路径独立覆盖,防止 ApplySueConfig 只接引擎不接门面。

**TS-39 PFC gating(simple threshold)**
- 测什么:PFC 简单阈值门控的时序证据。
- 场景:直连,pause@300/resume@100、drainDelay=20µs,10 帧突发。
- 行为链:缓冲越过 300B → pause 帧 → 发送端停 → 排空至 ≤100 → resume。
- 断言:10 全送达;首帧 <2µs(暂停前)、**末帧 >15µs**(被暂停延迟——若不暂停,10 帧只需 ~8µs)。
- 设计要点:与 TS-11 同思路——用首/末帧时刻夹出"暂停发生过"的区间。

**TS-40 PFC shared-pool gating**
- 测什么:共享池动态阈值确实在起作用(而非简单阈值)。
- 场景:简单模式阈值故意设到不可达(10000B);共享池 2048B、动态阈值 `(2048-sharedUsed)>>2`、headroom 10KB 不可达;10 帧突发 + 计数 pause/resume sink。
- 行为链:池随突发填满 → 动态阈值收缩 → **共享池触发 pause**(简单模式永远够不着)→ 排空 → resume。
- 断言:10 全送达、pause>0、resume>0、末帧 >15µs。
- 设计要点:场景构造先排除简单模式的可能(阈值 10000B 不可达),再断言 pause 发生——只剩共享池一个解释。

**TS-41 PFC shared-pool hysteresis(resume offset)**
- 测什么:回归 PFC-SP-03——resume 线 = th − resume_offset 的迟滞带,以及"池耗尽 th=0 误 pause 空闲 VC"缺陷的 hdrm>0 守卫。
- 场景:引擎级,共享 1000B/headroom 100B/offset 200B/alpha 0(动态阈值在排空期钉在 1000,迟滞带恰为 offset=200B 宽)。
- 行为链:200B 入队 → 超 headroom → pause(一次);灌到 1000(池满)→ 仍单次 pause;排到 900(带内)→ **无 resume**;排到 800(带边)→ resume;再灌再排 → 第二周期稳定。
- 断言:pause/resume 计数 = 1/0/0/1/2/2 序列。
- 设计要点:迟滞带两端点(900 与 800)各取一侧,把"带内不 resume、带边 resume"钉死;hdrm>0 守卫修复的是"池耗尽时动态阈值退化为 0、空闲 VC 被误 pause"——场景在 pause 前先经 headroom 路径,两缺陷都覆盖。

**TS-42 FC nine rule 3(CC_Update in LLR window)**
- 测什么:规则 3——LLR 窗口内的 CC_Update 必须被丢弃(不更新 R_VC_CF),以及回收的幂等性。
- 场景:引擎级,窗口旗标保持 + 快照 cc[0]=5。
- 断言:窗口内 OnCcUpdate → R_VC_CF==0(无回收);窗口开 → 回收 5、lost-recovered 计数 1;同快照再来 → 不双收;新快照 10 → 回收到 10。
- 设计要点:幂等性("同快照不双收")防止周期 CC_Update 重复注入同一丢失量;与 TS-43(c) 的真实窗口路径互补。

**TS-43 FC LLR window engine-driven(rules 3 and 9 real windows)**
- 测什么:回归 §12.31 B2——LLR 窗口必须由**引擎状态驱动**(覆盖跨多事件的真实 REPLAY/丢弃区间),而非只在同步重传调用内的纸面旗标。
- 场景(三节):
  - (a) 引擎窗口真值:接收侧 BAD 帧 → NACK → 丢弃窗口**开**;重传落地 → **关**;发送侧 NACK → REPLAY → **开**;最终 ACK → ADVANCE → **关**(经 SetLlrWindowCb 观察);
  - (b) 规则 9 设备接线:真实 REPLAY 期间 `SendCtlCcUpdate` 被禁(cc_update_tx==0),ADVANCE 后放行(==1);
  - (c) 规则 3 + 类型 B:接收端丢弃窗口内 CC_Update 被丢(R_CF==0),但**类型 B(LLR-ineligible)帧仍计 R_VC_CC**(UEC Table 5-26);窗口关后回收 lost=5−1=4。
- 断言:每节的旗标与计数。
- 设计要点:旧实现的窗口旗标只在重传 TransmitStart 的同步区间为真,周期 CC_Update 穿窗导致 CC/CF 双偏、CU 误算 2^15、VC 误闸停摆——(b) 用真实 REPLAY 状态验证发送侧禁发,(c) 验证接收侧丢弃且类型 B 例外。

**TS-44 FC nine rule 9(no CC_Update in window)**
- 测什么:规则 9 的静态旗标路径(与 TS-43(b) 的引擎驱动路径双保险)。
- 场景:设备级,窗口旗标保持 → `SendCtlCcUpdate`;窗口开 → 再发。
- 断言:窗口内 cc_update_tx==0(无帧无计数)、窗口后 ==1。
- 设计要点:两条路径(静态 SuspendForLlrWindow 与引擎驱动)共用同一个 `m_inLlrWindow` 事实源,本用例守护静态入口不被绕过。

**TS-45 FC facade PFC OnRx thresholds**
- 测什么:回归 PFC-SP-04——门面 OnRx 必须链到 `CheckThresholds`,否则经 conf 路径的 PFC 门控静默等效 OFF。
- 场景:引擎包在 `SueFcController`(FC_MODE=PFC),4×90B=360B ≥ pause 300 → pause sink;排 320B 至 40B ≤ resume 100 → resume sink。
- 断言:pause==1、buffer used==40、resume==1。
- 设计要点:该缺陷在"直接调引擎"的测试里永不暴露(直连时谁都会记得调 CheckThresholds)——必须走门面路径才能抓住接线缺失。

### 端点 / 配置 / 协商

**TS-46 ENDPOINT generator(shim-vlan VC, stubs, workload gate)**
- 测什么:`SueLiteEndpoint` 的三件事:流类别编码与接收归因、预留 AstraNetworkAPI stub、workload 门回退语义。
- 场景:三场景(均经 FwdTopo,先给全设备 100Gbps 真实速率——占位 32768b/s 会让 20µs 间隔的帧排队重叠而丢):
  - A:shim dscp=3(SetUseShim+SetVc(3)),5 帧 h1→h2;
  - B:VLAN pcp=2(SetUseVlan+SetVc(2)),4 帧 + 三个 `sim_send/sim_recv/sim_schedule` stub 调用;
  - C:`ENABLE_SCALEUP_WORKLOAD=1`,3 帧(场景尾恢复默认 0)。
- 断言:A 收 5、`GetRxByVc(3)==5`、byDst(2)==5、bySrc(1)==5;B 收 4、`GetRxByVc(2)==4`、三 stub 均返回 -1;C 仍收 3(**warn 后回退内置生成器,非静默 inert,§12.23**)。
- 设计要点:接收归因(byVc/byDst/bySrc)是 host 侧 §7 端到端一致性的直接证据——发送端队列 VC 与接收端线头解出的 VC 必须同表一致。

**TS-47 CFG VC mapping(DSCP_PCP_DEFAULT tables)**
- 测什么:§7 三键的完整语义:默认直映、部分覆盖、DEFAULT_VC 独立性、**主机/交换机解同一张表**、PCP_TO_VC_MAP 的 **DEI 行选**(16 项)。
- 场景(五场景,每场景后 `RestoreDefaults()+AssertDefaults()`——55 例共享进程,残留非恒等映射会污染 TS-07 的 dscp=5→VC5 哨兵):
  - ① 默认直映:0/3/5→自身、**8→0**(钉住 `dscp&0x07` 周期,而非 dscp/8 分块——这是历史上被写错过的点)、12→4、63→7;pcp 恒等(含 dei=1 行);default==0;
  - ② 部分覆盖 `0:7,63:0,5:5` + `PCP 0:7` + `DEFAULT_VC 5`:列出的改、**未列出的保持直映**(dscp9→1、pcp3→3)、DEFAULT_VC 独立于两表;
  - ③ 交换机路径 `5:7`:shim dscp=5 帧 → 交换机 vc7 rx/tx==1、**vc5==0**(证明交换机查表而非硬编码 `&7`);
  - ④ 主机路径 `3:7`:端点 SetDscp(3) → 接收端 `GetRxByVc(7)==5`、vc3==0(host 发送与接收归因同表);
  - ⑤ **DEI 行选** `0:1,8:7`:单元级 `PcpToVc(0)==1`、`PcpToVc(0,dei=1)==7`、`PcpToVc(1,dei=1)==1`(未列出行内恒等);交换机路径 pcp=0+DEI=1 VLAN 帧 → vc7 rx==1、vc1==0、**vc0==0**(不是旧 8 项恒等,也不是 DEI0 行);主机路径端点 `SetPcp(0)+SetDei(true)+SetUseVlan` → 接收端 `GetRxByVc(7)==5`、vc1==0。
- 断言:如上逐项。
- 设计要点:② 的"未列出保持直映"是部分覆盖语义的关键;③④ 合起来是"host 发送/主机归因/交换机解码同一张表"的端到端证据链;⑤ 钉住 DEI 的三处贯通——线头 DEI 位(TAG 首字节 bit4)序列化存活、三处解码点(端点发送 qvc/交换机 ComputeVc/主机归因)都用双参 `PcpToVc(pcp, dei)`,且 8 键映射下两 DEI 行相同(旧配置逐字节零回归,① 的 dei=1 行断言即此)。

**TS-48 LLDP header round-trip(RTS-RTR bitmaps)**
- 测什么:CBFC-SP-13 头扩展的序列化完整性——45B(42+3 位图)。
- 场景:纯序列化:want 位 0/3/7、rts 位 1/7、rtr 位 5(跨字节边界位 7 特意覆盖)+ 既有字段(creditSize=256、vcLimit[7]=107)。
- 断言:尺寸==45;三张位图逐位 == 0x89/0x82/0x20;未设位仍 0;既有字段不受新字段影响。
- 设计要点:位图访问器与字节布局独立验证——位 7 是 1B 位图最容易错移的边界。

**TS-49 CBFC RTS-RTR handshake gating(no credit TX before init)**
- 测什么:§5.2.8.4 核心禁令——握手完成前不得用信用发帧,及完成后的自动放行(唤醒源)。
- 场景:双端协商,链路 prop **2µs**(把 3 轮握手拉长到 ~7.2µs,制造可观测窗口);10 帧 t=0 压入;6µs 处埋观察点。
- 行为链:t=0 双端发 F1(want)→ 2.4µs 各收对方 F1 → 置 rts 发 F2 → 4.8µs 收 F2 → 置 rtr 发 F3 → 7.2µs 收 F3 → **active 置位 + DequeueAndTransmit 唤醒** → 帧从 ~7.2µs 起发出。6µs 观察点落在第 2/3 轮之间。
- 断言:**6µs 时 0 送达**;终态 10 全送达、S_CC=10、CU=0、双端 `GetVcLosslessActive(0)==true`。
- 设计要点:若没有链路时延,握手在亚微秒内完成、"完成前 0 送达"不可观测——2µs prop 是**用物理参数构造断言窗口**的手法;10 帧全送达同时证明握手完成后的唤醒源(缺了它,挂起帧永不发)。

**TS-50 CBFC asymmetric RTS-RTR(VC not wanted stays gated)**
- 测什么:非对称 want 的端到端语义——对端未 want 的 VC 本端永久门控。
- 场景:A 只配 CL[0..3]=8(want=0x0F),B 配全 8(want=0xFF);B 压 vc0×10 + vc4×10(帧用 shim dscp 编码 VC,identity 映射保证线头归因一致)。
- 行为链:B 的 rts=want_A=0x0F → B active==0x0F → vc4 的 CanSend 恒 false → 帧留在 egress 队列永不出。
- 断言:B `S_CC(0)==10`、`S_CC(4)==0`、B active 位图==0x0F、A active==0xFF(B want 全 8)、A 收 vc0==10/vc4==0。
- 设计要点:固化最小模型的"对端未 want 的 VC 排队不发"(引擎无 best-effort 通道,§12.32 记录在案);A/B 位图断言互为镜像,证明握手双向独立推进。

**TS-51 CBFC LLDP no-peer fallback self-negotiates**
- 测什么:no-peer 兜底路径——10µs 收不到对端 TLV → 自协商完成握手,链路可用。
- 场景:仅 A 协商,B 直配(忽略 LLDP 帧——OnRemoteLldpTlv 的 `m_lldpNegotiate` 守卫,防"无 want 的响应取消 A 的 fallback 造成死锁"——这是实现中抓出的真缺陷);A 压 10 帧 t=0。
- 行为链:A 10µs 兜底定时器触发 → `m_lldpRemote=m_lldpLocal` 自协商 → 不动点循环(≤3 轮:want→rts→rtr→active)→ active 全开 → 帧发出。
- 断言:10 全送达、**首帧 ≥10µs**(兜底前不可能发出)、CU=0、A active==true。
- 设计要点:自协商必须是**不动点迭代**——单次调用只完成 want→rts,rtr 仍为 0,VC 永不激活(实现中抓出的第二个真缺陷);首帧时刻断言证明门控到兜底为止。

**TS-52 CBFC lossless-active gate unit(default on, credit-limit immune)**
- 测什么:active 门的两条实现不变量:默认全开(直配零回归)、`SetVcCreditLimit` 不碰该位。
- 场景:引擎级:构造+Init → CanSend true;SetVcLosslessActive(0,false)→ CanSend false;`SetVcCreditLimit(0,8)` → **仍 false**(不得重开);VC1 不受门影响;Init → 恢复 true。
- 断言:如上。
- 设计要点:为什么 `SetVcCreditLimit` 不能碰 active:LLDP 首帧采纳远端 limit 时会调它——若它顺带置 true,握手未完成就提前激活,TS-49 的"完成前 0 送达"即刻失效。这条单测是那条不变量的事后守护。

**TS-53 CFG default-config 100G E2E(auto timer, no spurious replay or FLUSH)**
- 测什么:回归 §12.27——100G(ScaleOut 最小端口速率)下默认配置(定时器 0=自动折算)端到端无病理重传。
- 背景:1G 历史病理——固定 1000ns 默认定时器时,1G 链路 CtlOS 门(400B=3.2µs)与 ACK 周期(2048B=16.4µs)都远超 1µs → replay 定时器每 µs 无谓重传 → replay_ct 耗尽 → FLUSH 级联(s6-entry 实测)。100G 下门缩到 32ns/164ns,病理不复现;本用例现守护 100G 默认配置自动折算(≈21840ns ≫ RTT_eff≈184ns)路径。
- 场景:经 conf 路径建网(100G 直连),单帧端到端。
- 断言:收 1、tx_replay==0、无 discard。
- 设计要点:单帧是最锐利的病理探针——若定时器仍病态,一帧也会触发重传链;零重放断言是"自动折算公式正确"的直接证据;2026-08-13 由 1G 版更名上移(§12.36)。

**TS-54 CBFC CF_Update loss compensation + CF_min-CF_max pacing**
- 测什么:CF_Update 发送侧节奏约束(UEC §5.2.11.2/Table 5-26 双触发)与**丢 CF 的信用补偿闭环**——TX-UPD-003 的单包全覆盖证据:一条 CF_Update 在链路上丢失,发送端 CU 仍能归零(靠后续 CF_min 网格/CF_max 全量补偿),不靠对端重发同一条消息。
- 场景(三场景,每场景独立建 100G 直连网,双引擎 100B 信用/CL=5):
  - S1 纯 CF_min 网格(CF_max=0):B 端 `SetCfUpdateTimers (2µs, 0)`,drainDelay=10µs,A 压 10 帧;
  - S2 纯 CF_max 周期全量(空载,只有周期触发器):`SetCfUpdateTimers (2µs, 8µs)`,不发数据;
  - S3 双触发 + 丢 CF:A 挂 `SetCtlDropOnce (2)`(第 2 条入向控制帧=一条 CF_Update 丢失),B 双定时器,压 10 帧。
- 行为链:S1 每次排空产生 CF_min 一枪;S2 空载下 CF_max 每 8µs 发全量(一次打包 2 VC、背靠背 4 条消息,72ns 间隔);S3 丢失一条 CF 后,后续 CF_min/CF_max 消息携带**全量 CF**(补偿语义),发送端 CU 最终归零。
- 断言:S1 10 全送达、CU==0、CF 观测 >3 条、**相邻 CF 间隔 ≥1.9µs**(CF_min 网格,留 100ns 容差);S2 观测 >5 条、除首条 arm 边缘外间隔 ∈ (7µs, 9µs);S3 10 全送达、CU==0、`cf_update_rx == cf_update_tx − 1`(恰丢一条,而非丢后重发)。
- 设计要点:观测器**过滤 VC0**——CF_max 一次打包 2 VC 的消息间 72ns 间隙是批内间隔,不过滤会把合法批内间隔误判为 CF_min 违例;线头是 `[SueCtlHeader][SueCbfcCtlOsHeader]`,必须**先拷贝再 RemoveHeader 剥 1 字节 ctl 头**才能 PeekHeader 到 CF 头(直接 Peek 读到的是垃圾——实现中踩过的坑)。S2 跳过首条观测(周期定时器 arm 边界非 8µs)。

**TS-55 LLDP LLR capability quadrants(both-single degrade)**
- 测什么:NEG-PROC-001 / NEG-OPT-LLR-002 的 W/E 能力协商端到端:双向支持 → LLR 正常启动;单边支持 → 支持方**降级 BEST_EFFORT**(不发 INIT、RX 不过滤),链路仍可用。
- 场景(三象限 a/b/b′,独立 100G 直连网,20µs 协商窗后发 1 帧):
  - (a) 双端引擎 + 双端 `EnableLldpNegotiation` → 均宣告支持;
  - (b) 仅 A 有引擎(B 无引擎 → 宣告 llrSupported=0),B 用 `SetLlrEnabled (true)` 走 legacy 剥 preamble 路径;
  - (b') 镜像:仅 B 有引擎,A legacy。
- 行为链:(a) LLDP 采纳 → A 发 INIT → B 回 INIT_ECHO → A 进入 ADVANCE → 帧作为 LLR-eligible 走 seq 过滤送达;(b)/(b') 采纳对端不支持 → 支持方 mode_remote/mode_local 双侧 OFF → 帧走 BEST_EFFORT 送达。
- 断言:(a) A `TxState==ADVANCE`、`tx_init_ctl_os>0`、B `rx_ok==1`、帧送达;(b)/(b') 支持方 `GetModeRemote()==false && GetModeLocal()==false`、`tx_init_ctl_os==0`(从未发 INIT)、帧仍送达。
- 设计要点:无引擎端**必须 `SetLlrEnabled (true)`**——引擎不存在时设备不剥 8B preamble,收方会把 preamble 当载荷、帧不可读(实现中踩过的坑,镜像 SC-26 的 MakeLink(false) 手法);b/b' 成对断言"降级是对称的",钉死能力交换不因方向而异的语义。

**TS-56 LLDP adoption mismatch WARN observables(NumVCs, lossless set)**
- 测什么:ULL-CBFC-INIT-001(UEC §5.2.8.1)——NumVCs 与无损 VC 集合必须两端一致;不一致时**告警 + 可观测标志**,且采纳照常(告警不阻断,RTS/RTR 门已在功能上兜住)。
- 场景(三场景,独立 100G 直连网):
  - (a) 非对称 want 自然交换:A 只配 VC0-3、B 配全 8(镜像 TS-50)→ 双方各收到不一致的 want 位图;
  - (b) 构造 NumVCs=4 的远端 TLV(手工帧直投 `HandleCtlFrame`),本地 8;
  - (c) 对称交换对照组(全 8,无任何不一致)。
- 行为链:(a)/(c) 正常 LLDP 多轮交换,首帧采纳块比较本地快照与远端 TLV;(b) 首帧采纳块收到 numVcs=4 → WARN,随后 creditSize=128 照常采纳。
- 断言:(a) 双方 `GetLldpLosslessSetMismatch()==true`、`GetLldpNumVcsMismatch()==false`;(b) B `GetLldpNumVcsMismatch()==true`、lossless-set 不误报、`GetCreditSize()==128`(采纳未被告警阻断);(c) 双标志全 false。
- 设计要点:只对**结构性**不一致告警——creditSize/pktOvhd/vcLimit 数值按 UEC 方向独立、可合法不同(SC-26 的 256↔64),若对数值告警会在合法场景误报;告警不阻断是刻意的(最小模型无拒绝路径,TS-50 证明门控已功能性兜底),可观测标志把 WARN 从纯日志变成可断言对象。

**TS-57 CBFC CC_Update MACDA-CID configurable(5.2.6.2)**
- 测什么:ULL-CBFC-MSG-CC-003——CC_Update 的 MACDA/MACSA/CID 可配(§5.2.6.2 建议级):默认字节不变、定制值逐字节上线、**定制帧仍被对端识别处理**(识别走内部 tag 分发,线头字段不影响)。
- 场景(两段):
  - (a) 头单元:默认构造 → 序列化字节钉 MACDA=01-80-C2-00-00-01、CID=FA-7A-CB;定制 `SetMacDa(02:03:04:05:06:07)+SetCid(0xA1B2C3)` → 逐字节断言 + RemoveHeader round-trip;
  - (b) e2e:直连双 CBFC 引擎,A 配 `SetCcUpdateMacDa(02:00:00:00:00:01)+SetCcUpdateCid(0xCAFEB0)`,`StartCcUpdateTimer(50µs)`,B 挂 SueRx 观测器抓 CC_UPDATE 帧。
- 行为链:A 在 50/100µs 发两帧 CC_Update(定制 MACDA/CID + 自身 MACSA)→ 线头字段按配置 → B 的 tag 分发不受影响 → cc_update_rx 递增。
- 断言:(a) 默认/定制逐字节、round-trip 相等;(b) 观测 ≥2 帧、wire MACDA/CID == 配置值、wire MACSA == `A->GetMacSa()`、`cc_update_rx>=2`、`cc_update_tx>=2`。
- 设计要点:默认值即规格值(TS-34 钉的 FA-7A-CB 字节不动),配置是**覆盖**而非改默认——零回归的根基;(b) 的 `cc_update_rx>=2` 证明"识别不依赖 MACDA/CID 数值",防止有人把识别改成按线头字段匹配后本场景静默失败。

**TS-58 LLDP want revocation closes the gate(RTS-RTR assignment)**
- 测什么:ULL-NEG-CBF-RCV-006 / INIT-008(部分)——接收端 R_VC_Want TRUE→FALSE 的动态撤销:发送端 S_VC_RTS 清位、**已激活 VC 的门再次关闭(act 1→0)**、R_VC_RTR 随之为 0;以及 §5.2.8.1 的方向独立性。
- 场景:直连 2µs prop,双端对称 want(全 8),A 发 vc0/vc4 各 5 帧;t=30µs B 调 `SetVcWant(4..7,false)`;t=50µs A 再发 vc0/vc4 各 5 帧。
- 行为链:阶段1 对称协商(~7.2µs)→ vc0/vc4 均激活、各 5 帧流通;阶段2 B 撤销 want → assignment 语义逐轮传播(A.rts 清 4-7 → B.rtr = rts&want 清 4-7 → A.act 4-7 **1→0** 关闭,≤3 轮 ~7.2µs);阶段3 vc0 照走、vc4 帧排队于 CanSend 之后永不发出。
- 断言:A `GetVcLosslessActive(0)==true / (4)==false / (7)==false`;B `GetVcLosslessActive(4)==true`(**方向独立:B 撤销的是自己的接收方向,A 仍 want VC4,B 的发送方向不受影响**);B 收 vc0==10 / vc4==5(撤销后 5 帧未达);A `S_CC(0)==10 / S_CC(4)==5`(撤销后帧未计费);两 VC CU==0(排空)。
- 设计要点:旧实现位图**单调 OR**——已激活 VC 永远无法撤销(want 翻转被 OR 吞掉),assignment 是规格要求的语义;测试必须**先激活后撤销**(阶段1/阶段3 的帧流对比),若一开始就非对称,静态门控(TS-50)就能通过、测不到 1→0 转换;阶段2 的 t=30µs 刻意选在阶段1 帧全流通之后(~16µs)且早于阶段3,窗口不重叠。

**TS-59 LLR seq wraps the full 2^20 space cleanly (no NACK)**
- 测什么:2^20 seq 空间回绕的正确性——接收侧 `next_rx_seq` 必须留在取模空间;裸递增会在第 2^20 帧后与 wire seq 永不相等,回绕后全部帧判 missing(2026-08-14 thpt 512B 长突发回归:每回绕丢 ~3 帧,§12.38)。
- 场景:引擎级直驱,TX/RX 引擎对,A 逐帧 `TxStore`、B `RxFrame`,ACK 经 sink 环回 `OnAckNack`;共 SEQ_MOD+100 = 1,048,676 帧。
- 行为链:B `OnInit(0)` 建立 vld → 全空间逐帧到达 → 跨越 2^20 边界(seq 1,048,575 → 0)连续接受 → 逐帧 ACK 全部被 TX 判为在界内。
- 断言:ACCEPT==n、DROP==0、NACK==0、`GetNextRxSeq()==100`(取模空间)、`rx_missing_seq==0`、TX `rx_ack_nack_seq_error==0`。
- 设计要点:引擎级直驱 1M+ 帧(0.44s)远快于设备级 E2E;短用例(帧数 < 2^20)永远测不到回绕——本用例是长突发场景的守护,thpt 512B/1024B 长突发实测配套验证。

**TS-60 AFH VRF segmentation + source precedence (DSCP then VLAN then port)**
- 测什么:Switch-ASIC-AFH §6.6.1/6.6.4——转发库按 VRF 分段(查表键 = XPU-id+VRF)、VRF 三源派生与优先级(DSCP > VLAN > 端口,用户拍板)、未编程 VRF 表 = miss 不回退、默认全部 VRF0(§12.40)。
- 场景(三节):(a) 分段单元:每 VRF 一张独立路由表经 switch 装配,旧签名 = VRF0,未编程 vrf = null;(b) 配置 accessor:三张映射表的默认(全 VRF0)与部分覆盖;(c) e2e:两个入口 host(h1a=port0 映射 vrf1、h1b=port1 未映射)+ 两个出口 host,路由 vrf0 xpu2→hA / vrf1 xpu2→hB / vrf2 xpu2→hA / vrf3 未编程。
- 行为链:优先级阶梯四帧——h1b 裸帧(全未映射)→ vrf0 → hA;h1a 裸帧(port→vrf1)→ hB;h1a vid=100(VLAN 覆盖 port)→ vrf2 → hA;h1a shim dscp=9 + vid=100(DSCP 覆盖 VLAN+port)→ vrf3 未编程 → miss 丢弃。
- 断言:hA 收 2 / hB 收 1;`GetCntVrfRx(0/1/2)==1`、`GetCntVrfRx(3)==0`、`GetCntVrfMiss(3)==1`、全局 `GetCntLookupMiss()==1`;accessor 默认与映射值逐项;末尾恢复三键默认(防串扰)。
- 设计要点:帧不携带 VRF(线上无字段)——VRF 是交换机本地配置,测试全走 e2e 帧流而非注入字段;三源同中时的优先级阶梯必须用"映射到不同 VRF + 未编程表 miss"构造,单一映射的测试测不出优先级;端口映射经 `GetPortIdx`(AddPort 序)反查,与通道接线顺序绑定。

**TS-61 CBFC deferred refund is preamble-invariant (no over-refund)**
- 测什么:§12.42 延迟退款的尺寸不变性——§12.30 延迟退款与 LLR in-place preamble 前插交互下,退款释放的 PktCredits 必须恰等于入口 Receive 计费的数目;守护缺陷 = 过度退款导致 S_CF > S_CC,CU = (S_CC−S_CF) mod 2^15 翻到 ≈2^15 ≫ CL,信用窗口永久毒化(§12.41 VRF 32 平面全互联停滞事故的回归闸)。
- 场景:host A — 交换机(p0/p1)— host B,100Gbps 三链路;ca 与 cp1 挂 CBFC 引擎(creditSize=256、VC0 CL=32);p0 `SetDeferCreditRefund(true)`;p1 与 B `SetLlrEnabled(true)`(legacy preamble 路径,无引擎 INIT 握手);A 连发两个 2048B 帧(2034 载荷 + 14 SueHeader)= 每帧 8 信用。
- 行为链:帧 1 到达时出口 p1 空闲 → `SwitchSend` 同步跑 `DequeueAndTransmit` → 8B LLR preamble **原地**前插(包对象此刻 2056B)→ 延迟退款用 SwitchSend 前快照的 2048B 退 8 信用;帧 2 入出口队列排队后 drain;接收侧 OnRx 量无 preamble 帧、计费 8 信用。
- 断言:两帧全收(`g_rxCount==2`);`cp0->GetRvcCf(0)==16`(2×8 信用,preamble 未入记账);`ca->OnCfUpdate(0,16)` 后 `ca->GetSvcCc(0)==16` 且 `GetSvcCu(0)==0`(收发相抵、窗口归零、无毒化)。
- 设计要点:帧 1 必中空闲出口 = 同步前插事故现场(无需构造时序即可复现);legacy preamble 路径让用例保持 QUICK(不拖引擎握手定时器);2048B 刻意贴 256B 信用边界——2048→8 信用 vs 2056→9 信用,1 信用超退即令 CU=32767>CL=32、`CanSend` 永假;双帧使 R_CF==16 能区分"漏退"与"超退"两种失败形态。

---

**TS-62 CBFC dual-cell granularity (32 vs 56 reconcile)**
- 测什么:§12.56 双粒度记账(UEC §5.2.5.8 method-2)——发送侧按 tx cell 计 CC、接收侧按原生 rx cell 计 CF 时,粒度差经 CC_Update 对账后账本守恒。
- 场景:纯引擎;A(tx=32, rx=128)— B(tx=128, rx=56,交换机角色),VC0 窗口 4096;1518B 帧一收一发一 drain。
- 断言:`PktCredits(1518)==48`、`RxPktCredits(1518)==28`、40B 短帧 tx=2/rx=1(不足一个 cell 占满一个信用);CF_Update 后 CU==20(泄漏可见);`OnCcUpdate` 后 R_CF==48(28 drain + 20 泄漏)、再 CF_Update 后 CU==0(账平)。
- 设计要点:48/28/20 是方案评审的规范化走账数字,逐位固化为判据;泄漏必须"先可见、后回收"两段断言,防实现直接换算掩盖机制。

**TS-63 CBFC TX-cell adoption snap (5.2.8.2 + 5.2.10.1)**
- 测什么:LLDP 采纳语义——广播 rx 原生 cell,对端采纳时 snap 到支持集内最大 ≤ 值,且**只写 tx 不碰 rx**。
- 场景:引擎配 rx=128、无序支持集 {2048,32,64,128,256,512,1024};`AdoptTxCellFromPeerRx` 分别喂 56/128/0;另配 {64,128} 喂 32;空支持集喂 56。
- 断言:56→32(2 的幂 snap)、rx 恒 128;128→128 精确采纳;0(缺 TLV)保持现值;无可行值→最小支持值 64;空集→逐字采纳(旧行为)。
- 设计要点:无可行值分支守护"Sender ≤ Receiver"硬规则被破坏时的可观测性(LostCredits<0 会在首个 CC_Update 上报);无序输入验证内部排序去重。

**TS-64 CBFC rx==tx identical to single-cell**
- 测什么:回归护栏——rx==tx 退化为旧单 cell 引擎,所有可观测量逐位一致(§12.56 落地不改变既有行为)。
- 场景:legacy 对(SetCreditSize(100))与 dual 对(等 cell + 支持集含 100 + 采纳 100)跑同一 op 序列(7×OnTx/OnRx、隔帧 OnDrain、逐帧 CF、末轮 CC)。
- 断言:PktCredits/RxPktCredits 对 86/100/1518/40B 四种帧长全等;S_CC/S_CF/CU/R_CC/R_CF 五组计数器全等。
- 设计要点:双引擎对照而非黄金值——任何未来单 cell 行为演进自动同步到本护栏;支持集含 100 验证"精确命中不 snap"。

**TS-65 CBFC dual-cell e2e (CC_Update reconciles leak)**
- 测什么:真实设备路径上,32/56 异粒度 + 周期 CC_Update 下窗口不被泄漏耗尽(与 SC-36 c 臂的硬停互为正反)。
- 场景:A—B 直连 100Gbps;ca 全 32、cb 全 56;VC0 CL=512;B drain 10µs、CF 5/50µs;A CC 50µs;连发 100×1518B。
- 断言:100 帧全收;`S_CC==4800`(100×48);末态 CU==0(泄漏全部对账回收);B 侧 cc_update_rx>0。
- 设计要点:窗口 512、每帧泄漏 20——无对账时 ~25 帧即卡死,用例证明 CC 周期介入后满交付;S_CC==4800 同时钉住发送侧粒度未被采纳/配置路径意外改动。

---


---

## 2. scratch 用例(SC-01 ~ SC-36)

> scratch 为自检式二进制:`[ok]/[FAIL]` 逐条 CHECK + 末尾判语串;`run-scratch-checks.sh` 按判语判定,环境变量可配场景。

### afh(转发)

**SC-01 sue-afh-device-dispatch**
- 测什么:S0 脚手架直跑版(TS-01~03 三合一)。
- 场景:三类型实例化 + Attach 注册(GetNDevices==2)+ TransmitStart 静态分派到 dst Receive。
- 断言:判语 `AFH DEVICE-DISPATCH PASS`。
- 设计要点:fork 的 test-runner/test.py 在 Python 3.14 下不可用(AGENTS 记录),本二进制是无框架快速验证入口;CI 权威版本在 TestSuite。

**SC-02 sue-afh-header-parse**
- 测什么:S1a 头解析 + LPM/ECMP 的直跑版(与 TS-04/05 同构)。
- 设计要点:早于 TestSuite 落地而保留;按 test-organization 原则权威实现是 TestSuite,本文件是入口路径哨兵。

**SC-03 sue-afh-forward**
- 测什么:S1c 端到端:四组合转发 + TTL 递减 + TTL=0 丢弃 + lookup-miss 丢弃。
- 场景:2 host + 1 switch,逐帧注入并逐项 CHECK 收到/收不到/字段值。
- 设计要点:与 TS-06/07 同判据,判语 `AFH FORWARD PASS`。

**SC-04 sue-afh-wire-time**
- 测什么:S2 线时/IFG:100B@100G=8ns;背靠背第 2 帧@wireTime+IFG+wireTime(设备同源 CalculateBytesTxTime)。
- 设计要点:与 TS-08 同判据,经 channel trace 采样。

### cbfc(信用)

**SC-05 sue-cbfc-engine-credit**
- 测什么:S2a 引擎账务直跑:CanSend/OnTx 计费/耗尽/CF_Update 回收/CC_Update 泄漏回收/8-VC 独立性。
- 设计要点:引擎裸栈零 wire,与 TS-09 同构;判语 `CBFC ENGINE-CREDIT PASS`。

**SC-06 sue-cbfc-e2e-gating**
- 测什么:S2c 端到端门控:10 帧/CL=5,断言送达批次的暂停间隔(drainDelay 10µs)+ 全回收。
- 设计要点:与 TS-11 同判据,经 inter-device 回调路径(CF_Update 不走线)。

**SC-07 sue-cbfc-cc-update-onwire**
- 测什么:S4d 泄漏回收闭环:FLUSH 泄漏 1 信用 → CC_Update 周期回收 → CU 归零 + 后续帧投递。
- 设计要点:与 TS-36 同场景的直跑版。

### cfg / entry(配置与入口)

**SC-08 sue-cfg-parse-apply**
- 测什么:S6 配置系统三件套:SueReadConf 解析健壮性(注释行/十六进制值/未知键静默跳过)+ 运行时 EtherType 覆盖 + ApplySueConfig 门面接线与信用上限落地。
- 设计要点:未知键静默跳过是镜像 baseline ReadConf 的承诺——解析器不能因一个多余键拒绝整个文件。

**SC-09 sue-cfg-preset-smoke**
- 测什么:E1 conf 预置冒烟:conf/ 下 4 个预置逐一 `SueReadConf`(必须解析)+ `SueEntryDispatch`(必须建网)+ 端到端投递一帧(必须通)。
- 背景:此前没有任何自动化读过 conf/ 预置,scratch 内联镜像与预置已漂移过(credit limits、data-age 等键)——本用例把"预置文件可用"钉进回归。
- 设计要点:三个断言分层(解析/建网/投递),失败能定位到层。

**SC-10 sue-entry-dispatch**
- 测什么:S6 入口:host0—switch1—host2(CBFC+LLR)经 `SueEntryDispatch` 全链路投递 + `ENABLE_SCALEUP=0` → externalSetup 路径分支检查。
- 设计要点:入口分发是配置驱动场景的总闸,本用例守护"拓扑解析→BFS 路由→设备创建→端点绑定"整链。

### endpoint(主机生成器)

**SC-11 sue-endpoint-ring**
- 测什么:S5b 端到端 + **MTP 等价载体**:全 SUE 网络由 `SueLiteEndpoint` 驱动(替换手写 SendOne 突发),8 host + 2 ToR + 2 spine 双平面,ring/alltoall × VC0/VC3 四组合。
- 场景:每 host 一个自模拟生成器(目的轮转、间隔、帧数、互异 dither),全链路 LLR+CBFC。
- 断言:ΣRx==ΣTx==N·8 无丢包、无 discard、无 FLUSH、per-VC/目的/源归因;`run-s5b.sh` 双跑(threads=1 vs N)diff `ENDPOINT HASH` 逐字节一致 → `ENDPOINT MTP EQUIVALENCE PASS`。
- 设计要点:每 host 互异 dither 起始偏移(§12.20)保证出发时刻全局唯一——同时间戳跨 LP 事件的顺序在 Default(FIFO)与 MTP(LP 序)下会"内容→槽位"互换,等价性刀锋翻转;hash 含端点 tx/rx/归因全量。

### fc(流控门面/共存/帧格式)

**SC-12 sue-fc-counters**
- 测什么:S4f 计数系统直跑:3 帧坏 1 → NACK 重传,LLR/CBFC 计数逐项 + `PrintCounters` 落盘。
- 设计要点:与 TS-37 同判据,另覆盖 PrintCounters 输出路径(§6.11 的观测口)。

**SC-13 sue-fc-facade**
- 测什么:S4e 门面路径:FC_MODE=CBFC 经 `SueFcController` 完成 10 帧/CL=5 门控。
- 设计要点:与 TS-38 同判据,证明门面与直连引擎等价。

**SC-14 sue-fc-frame-headers**
- 测什么:S5 线格式字节级:PFC 64B(MACDA/SA/EtherType/Opcode/PEV/quanta/pad/FCS,CRC 恰覆盖前 60B)+ CC_Update 64B(CID/MsgType/20 位 MSB-first 打包/round-trip)。
- 设计要点:与 TS-33/34 同判据的直跑版;与 SC-17 分工——本文件是**字节级**(构造→序列化→逐字节),SC-17 是**线上级**(经 channel trace 观测真实传输尺寸)。

**SC-15 sue-fc-llr-cbfc-coexist**
- 测什么:S4 九规则直跑:坏第 2 帧 → 规则 1(重传不重计费)、规则 2(丢弃帧不计数)、规则 4(排空退款正常)+ ACK 路径信用归还。
- 设计要点:与 TS-32 同场景。

### incast(拥塞场景)

**SC-16 sue-incast-2to1**
- 测什么:S5e 2:1 CBFC incast(§12.29 发现/§12.30 实现):出口拥塞是否真实节流发送端、无损、线顶。
- 场景:3 XPU 全 200G,host0+host1 双线速发 host2,`CBFC_EGRESS_BUF_BYTES` 默认 0=auto(每 VC 至多一个信用窗)。
- 行为链:交换机出口队列满 → 帧挂起 m_pending[port][vc] → 入口信用**延迟退还**(defer,§12.30)→ 发送端窗口耗尽。
- 断言:(a) 出口聚合速率钉在 200G 线顶(非 380G 供给)+ per-source 50/50 分拆;(b) 有限突发下无损(送==收、replay=0、discard=0);(c) credit_exhausted>0 + 交换机挂起>0(背压耦合真实生效);(d) `INCAST RATES` 每 200µs 瞬时速率曲线(爬坡→饱和→排空)。env:THPT_RATE/GBITS/CREDITS/CFMIN。`run-s5e-incast.sh` 场景化运行 + 多 VC/非恒等映射变体。
- 设计要点:无损判据只在"有限突发"下成立(队列吸收超额后排空);credit_exhausted 与挂起计数是"节流真实发生"的正证据——没有 defer 退还的实现会在此双断言下现形。

### llr(链路层重试)

**SC-17 sue-fc-ctl-onwire**(旧名 sue-llr-ctlos-frames;ID 保持 SC-17)
- 测什么:跨模块控制帧线上保真(模块 fc=流控控制帧——改名原因:内容含 CBFC CC_Update 与 PFC 帧,并非 LLR 的 CtlOS)。
- 场景:经 SueChannel TxRxSue trace 观测每条 TransmitStart 的线上尺寸:PFC 帧与 CC_Update 是**真实 64B 以太网帧**(字节级布局由 SC-14 钉死);CtlOS 保持 9B 短报文(1B SueCtlHeader+8B 载荷)且节流门把突发摊开(相邻 ≥ 配置间隔)。
- 断言:判语 `FC CTL-ONWIRE PASS`。
- 设计要点:"线上真实"与"字节正确"分属两层——本文件证前者(设备确实经控制路径发完整帧),SC-14 证后者(帧内容正确)。

**SC-18 sue-llr-engine**
- 测什么:S3b 引擎端到端:坏第 2 帧 → NACK → replay 重传 → 3 全送达、ACK 释放 replay、状态回 ADVANCE。
- 设计要点:与 TS-14 同判据的直跑版。

**SC-19 sue-llr-flush-init**
- 测什么:S3c FLUSH 自愈:seq0 三坏 + replay_ct_max=2 → FLUSH → INIT 重同步 → 后续 2 帧送达。
- 设计要点:与 TS-18 同场景。

**SC-20 sue-llr-pcs-lost**
- 测什么:S5 闪断模型**设备级**三场景(引擎级在 TS-28):短闪断(<max)→ replay 恢复无 FLUSH;长闪断 → 超时 FLUSH + 重 INIT;空闲链路 + 长闪断 → **空缓冲也 FLUSH**。
- 场景:`SetPcsStatus` 经 Simulator::Schedule 按时间线切换;PCS FALSE 期间设备丢弃一切入帧(控制帧在内——不可解码)。
- 断言:三场景判语 + `GetPcsLostTimerExpired()`。
- 设计要点:设备级丢帧是引擎定时器的触发前提——本文件与 TS-28 合起来才是完整闪断链。

**SC-21 sue-llr-poisoned**
- 测什么:S3e 毒帧:被接受、不 NACK 不重传、22 计数器全量核对。
- 设计要点:与 TS-19 同判据的直跑版。

**SC-22 sue-llr-preamble-fcs**
- 测什么:S3a preamble+FCS:好帧剥 8B 后 SUE 头完好、坏帧 RX 丢弃、CBFC 信用按 MAC 尺寸计。
- 设计要点:与 TS-13 同判据。

**SC-23 sue-llr-replay-buffer**
- 测什么:S7 重放缓冲定容(§6.10):给定 LLR_REPLAY_BUF_MAX 支持多大 RTT/闪断/ACK 间隔——统一规则 `buffer ≥ rate×(RTT+silence)` 的换算验证,三门限(storage/BDP/window)场景。
- 设计要点:定容公式是本平台把"重放缓冲大小"从玄学变工程的产出,本文件守护公式在场景级的成立。

**SC-24 sue-llr-ctlos-loss**
- 测什么:LLR-SP-22 控制帧丢失容错五场景——**丢的是 CtlOS 本身**(错误模型在引擎见到控制帧前丢帧),恢复必须来自协议机制而非运气:
  - S1a 丢 ACK:周期累积 ACK 节奏下,下一周期重发(重复帧过滤不回 ACK 为规范行为)→ 发送端重复 ACK 仍释放窗口;
  - S1b 无节奏模式:丢 ACK → replay 定时器轮 → replay_ct 耗尽 FLUSH → INIT 重同步链;
  - S2 丢 NACK:发送端等不到 NACK → replay_timer 兜底重传;
  - S3 丢 INIT_ECHO:init 定时器超时重试;
  - S4 丢 INIT:对端无 INIT 则本端不设 vld,重试后握手完成。
- 断言:五场景逐条 + 判语 `LLR CTLOS-LOSS PASS`。
- 设计要点:每一类控制帧丢失对应一条独立恢复路径(ACK→周期重发/NACK→定时器/INIT 系→重试)——五场景把五条路径全部独立覆盖。

**SC-25 sue-llr-random-errors**
- 测什么:PLAT-SP-08 随机错误注入:拓扑 `err_rate` 驱动两端**概率 FCS 模型**,且必须**确定性可复现**。
- 场景:独立 RNG 流 + 显式流序号(全局种子确定、网络重建可复现);**同一场景进程内跑两遍**;1%/5% 两档;2000 帧。
- 断言:两档均无损恢复(rx=2000、discard=0)、损坏率与配置率同阶(nack<<frames)、**两遍计数器逐字节一致**。env:RND_RATE/RND_FRAMES。
- 设计要点:双跑一致性是确定性判据——随机注入若用隐式 RNG(无显式流序号),建网顺序的微小变化就会翻转损坏集合,双跑立刻不同。

**SC-26 sue-lldp-negotiate**(旧名 sue-llrp-negotiate;ID 保持 SC-26)
- 测什么:NEG-SP-01/02 LLDP 链路协商(模块 lldp;改名原因:creditSize/VC limit 协商属 CBFC 域而非 LLR)。一个用例覆盖两个协议域:
  - **Options TLV(LLR 域)**:B 无 LLR 对端 → 双方能力交换后降级 BEST_EFFORT TX / 不过滤 RX,帧仍投递;
  - **CBFC TLV(CBFC 域)**:A 非对称 creditSize(256↔64)/VC limit(512↔8)——**每个方向都采纳对端接收侧参数**,双向收敛;引擎在 peer TLV 应用后才 Start。
- 断言:判语 `LLDP NEGOTIATE PASS`。
- 设计要点:与 TestSuite TS-48~52 分工——本文件覆盖**参数采纳路径**(数值收敛),TestSuite 覆盖**握手门控路径**(RTS/RTR 时序);双向收敛是关键断言:单向采纳只对一半。

**SC-27 sue-llr-timer-wakeup**
- 测什么:S3d 两个唤醒路径:
  - [1] TX 唤醒链:REPLAY 期间新帧被 TX 门拦下 → 重传成功 → ACK → ADVANCE → **OnLlrStateChange 唤醒 DequeueAndTransmit** → 新帧自动发出(无任何外部触发);
  - [2] 纯超时恢复:对端无引擎(NACK 缺失)→ replay_timer 触发 DoReplay → 重传成功(错误模型只坏首帧)→ 走简单 ACK 路径 → ADVANCE。
- 断言:两场景帧送达 + 状态。
- 设计要点:[1] 是"设备与引擎唤醒接线"的守护(引擎状态变了设备必须知道);[2] 是 NACK 缺失兜底(§5.1.5)。

### mtp(等价性)

**SC-28 sue-mtp-equivalence**
- 测什么:S8 MTP 等价载体(§10.5/§12.20/§12.28):单线程 baseline 与 MTP-8 的行为等价。
- 场景:12 节点 2-plane 拓扑(8 host + 2 ToR + 2 spine,全部 CBFC+LLR + CF 定时器门控),每进程一跑;`run-s8-mtp.sh [2,4,8]` 双跑 diff:
  - **判据一**:行为 hash(`S8 BHASH`,掩 5 项 ACK/NACK 计数层)逐字节一致——交付/重放/丢弃/FLUSH 恢复/INIT 握手/CBFC 必须完全一致;
  - **判据二**(硬化):`S8 ACKSUM` 对被掩计数逐字段 |delta|≤1——"ACK 被吞/翻倍"类 bug 会在行为 hash 恒等时现形;
  - 双模式:NACK(SetCorruptOnce(100),单 NACK → 重放 83 帧 → 收敛)/ FLUSH(SetCorruptFirstN(10),replay 耗尽 → FLUSH → 跨 LP INIT 重同步,discard>0)。env:S8_THREADS/S8_FLUSH。
- 断言:两模式 `S8 MTP EQUIVALENCE PASS`。
- 设计要点:①每 host 互异 dither 出发时刻(§12.20)——同时间戳跨 LP 事件在 Default(FIFO)与 MTP(LP 序)下分配互换,刀锋翻转,出发时刻必须全局唯一;②行为 hash 掩 ACK/NACK 计数是**有据的**——周期累积 ACK 的 seq 流在两种全序下允许 ±1 漂移(§12.28),掩掉后用 ACKSUM 单独断言,两判据互补不盲区。

### pfc / thpt(对比与吞吐)

**SC-29 sue-pfc-gating**
- 测什么:S4b PFC vs CBFC 对比:同一突发双模式。
- 场景:PFC 侧断言门控确实发生(末帧到达晚于无门基线——pause 延迟了投递);CBFC 侧信用门控对照。
- 设计要点:对比结构让两种门控的语义差异可见(PFC=pause/resume 波次,CBFC=信用窗批次)。

**SC-30 sue-thpt-line-rate**
- 测什么:S5c 吞吐(§12.24-12.26):LLR+CBFC 下可达线速 + 帧损代价。
- 场景:2 节点直连,`THPT_GBITS` 载荷推送;发送端自链间隔 = **0.95×帧线时**(供给略超线速,测的是 CBFC/LLR 窗口实际能撑住的速率而非供给);clean(THPT_LOSS_EVERY=0)与确定性 1/N 帧损双跑。
- 断言:clean 速率 ≥95%×线速×载荷占比 + **sumReplay==0**(D1 加固:旧断言只查"全量送达",守不住"无损但吞吐崩塌");loss 场景 replay==nack 闭环、丢弃损失 <0.02%。`run-s5c-thpt.sh` 场景化;CL 64..4096×400G/100G 扫描产出 `docs/test-report/credits-vs-rate.html`(100G 列 2026-08-13 重测,§12.36)(≤512 信用窗口线性绑定、≥768 进入 97.9% 载荷效率天花板)。env:THPT_RATE/GBITS/PAYLOAD/INTERVAL_NS/LOSS_EVERY/THREADS/CREDITS/CFMIN/REPLAY_NS。
- 设计要点:速率阈值 + 零重放双断言是"吞吐护栏"——防止未来改动让链路"无损但慢"而无人察觉;0.95 因子保证供给饱和但出口积压浅。

### scale(规模)

**SC-31 sue-scale-topo-gen**
- 测什么:拓扑生成器 CLI:SCALE_N → /tmp topo + 布局统计。
- 场景:64 机 → 8×64 XPU + 64 内部交换机 + 2 spine = 578 节点、640 链路。
- 断言:布局计数与 `SueScaleUpTopoPlan` 一致 + 判语 `SCALE TOPO-GEN PASS`。
- 设计要点:生成物供冒烟/压测复用;布局校验即断言(节点=9N+2、链路=10N)。

**SC-32 sue-scale-smoke**
- 测什么:PLAT-SP-07 规模冒烟:64/128/256 机(512/1024/2048 XPU)建网 + 轻载端到端。
- 场景:机内环(slot1-6)+ 跨机点对点(slot0/7 双组——ECMP 熵扩展后双平面由流哈希自然分拆,保留双组以同时压两平面);`SueLiteEndpoint` 驱动;20 帧/端点。env:SCALE_N/SCALE_FRAMES/SCALE_THREADS。
- 断言:无损(送==收)、无 discard/FLUSH、双 plane spine 均转发、setup/run 墙钟记录(64 机 setup 227ms / 128 机 1.39s / 256 机 7.87s——BFS 超线性实测)。
- 设计要点:规模冒烟的目的是"结构不坏"(路由/ECMP/端点在大拓扑下仍对),不是性能;轻载保证墙钟可控(256 机 2 帧/端点)。

**SC-33 sue-scale-stress**
- 测什么:E7 满负载压测(§12.34):512 XPU 满负载的运行包络——持续速率、背压、无损、事件率、MTP 墙钟。
- 场景:64 机(578 节点/640 链路/400G,ACK spacing 16384B=UEC 上限),全端点跨机环流**连续注入** 50µs + 500µs 排空;双模式:
  - clean(interval 256ns≈63G/XPU,总需求 32T < 51.2T 截面积):无热点超 252G → 全接受、精确无损判据;
  - sat(interval 96ns≈168G/XPU,总需求 86T > 51.2T):信用窗吸收超额。
- 断言:clean:rx==tx==98368、credit_exhausted==0、持续 31.4T;sat:**持续 48.0T=93.7% 截面积**、credit_exhausted==221528、261760/261760 全投递、双平面 49.7/50.3;MTP-8 结果逐位一致(等价性保持到满负载)但墙钟 69.5s vs 9.3s(7.5× 慢);无负载模式:578 LP 分区/43MB RSS/1.65M 事件/ms-sim。env:SCALE_N/SCALE_DURATION_US/SCALE_INTERVAL_NS/SCALE_PAYLOAD/SCALE_THREADS/SCALE_SAT/SCALE_LOAD/SCALE_SPACING。
- 设计要点(两个实测教训,均已在场景构造中规避):
  ① **启动交错**:smoke 的 700ns×主机号交错在 50µs 压测窗口下会把半数端点推出窗口(实测吞吐虚低 7 倍)——压测用槽位级 dither(≤1µs);
  ② **持续速率口径**:必须按**注入停止瞬间采样**(排空尾只清积压,计入窗口会虚报超截面积——实测 84.3T > 51.2T 物理上限的假象);sampling 点与停止注入同一时刻、FIFO 序采样先行;
  ③ sat 模式"零拒绝"是信用窗吸收的结果(64 帧深 × 512 端点 + 128KB/VC 窗),断言"全投递 + 饱和"而非"有拒绝"——拒绝与否取决于过载比,不是机制正确性的判据。

### vrf(平面转发)

**SC-34 sue-vrf-planes**
- 测什么:§12.41 VRF 32 平面拓扑——8 GPU × 32×200G 口、单台 256 口交换机;GPU 的 32 口只用 XPU ID 标识,交换机 VRF 划成 32 平面;同端口号通信 + 纯 XPU ID 寻址的 all-to-all 连通与性能;LLR+CBFC 全栈(256 链路 INIT 握手 + 累积 ACK + replay 保护)。**§12.43 扩展**:同场景 CBFC vs PFC A/B 臂——同等数据量、同一注入纪律、同一标尺下,判定 802.1Qbb pause/resume 与逐 VC 信用窗的 all-to-all 效果差异。
- 场景:host 0-7 + switch 8,topo 256 行 `link i 8 200Gbps <llr> <CBFC|PFC> 0`(行序 = AddPort 序 i×32+k);conf = 200g 预置镜像(creditSize 256 / CL 512×8 / threshold 2075 / CC 102µs / ACK 2048 / CF 定时器 / `PORT_TO_VRF_MAP p→p%32+1`),PFC 臂替换为 `FC_MODE PFC` + 显式写出全部门限(drain/共享池 32KB/headroom 8KB/buffer 64KB/resume offset 4KB/alpha=1/quanta max,防默认漂移改口径);路由显式覆盖(自动建路只支持 VRF0 且会建成 32 口 ECMP 大组):VRF0 = 空表 + 32 张表各 8 条精确路由 `XPU j → 口 j×32+k`;流量条纹化:56 对 × 61024 帧(`VRF_PL_PAIR_GBITS` 默认 1),帧序 %32 = 发送口 k → 每平面每对 1907;1792 流,注入间隔 = 7×`CalculateBytesTxTime(2140)`(wireBytes = 帧+14+8+12+2×29 = 2140,两臂同一 wireBytes = 同 offered load),注入门控 FC 感知(CBFC 窗口满 / PFC pause 时 100ns 重试不积压);Simulator::Stop 12ms;PFC 模式另有 200µs 全织构 512 口 `IsPaused(0)` 普查采样器。
- 断言(六组 + 判语 `VRF-PLANES PASS`):①每 GPU 从其余 7 GPU 各收 61024(XPU-id-only 可达);②delivered==sent(3,417,344 零丢失);③逐平面 `GetCntVrfRx(k+1)==56×1907`(×32)+ `GetCntVrfRx(0)==0` + `GetCntLookupMiss()==0`;④每 GPU 口 k 收 7×1907(同口通信);⑤LLR replay/discard/nack 全 0;⑥聚合 ≥ 0.95×天花板(51,200×2048/(2048+14+8+12+2×29) = 48,998.9 Gbps 载荷份额口径;PFC 臂沿用同一 CBFC 标尺——A/B 必须同一标尺,PFC 无 CF 的物理天花板更高属其让利);附 per-GPU Gbit、交换机 pend/pendMax/rx/tx、PFC pause-samples 打印。
- 实测:LLR=1 CBFC 聚合 48,982.8 Gbps = 天花板 99.9673%,per-GPU 6.99872 Gbit × 8,所有 CU=0、pend=0、CF/CC 收支平衡;LLR=0 同判语 PASS;**FC A/B(§12.43)**:PFC drain=0 48,983.0 Gbps(99.9675%,pause-samples=0,PFC 实质未生效)PASS;PFC drain=1µs 36,890.9 Gbps(75.29%,pause-samples 2528)、2µs 22,343.9(45.60%,4320)、5µs 9,658.6(19.71%,11328)——生效即塌落,无损契约全臂保持(delivered==sent、replay/discard/nack=0、平面隔离精确),代价纯是带宽;墙钟 ~93s/档。
- 设计要点:帧不携带 VRF——平面 = 发送端口的选择(§12.40 纯 XPU ID 寻址语义的场景级验证);自动建路与平面化冲突 → 必须显式覆盖 VRF0 为空表(否则 32 口 ECMP 会把平面打散);注入节拍取"7 源共享出口"的满载线速,由 egress 队列 + FC 门自然整形,不人为限速;run-scratch-checks 钉 LLR=1(默认,§12.42 退款尺寸事故现场)、`VRF_PL_LLR=0`(同一退款路径无 preamble 模式)与 `VRF_PL_FC=PFC`(drain=0 配置默认,钉 PFC 全链装配/注入门控/采样诊断路径)三档;drain>0 三档低于 95% 标尺不注册,塌落机制(阈值-迟滞 stop-and-go 震荡 vs 逐 VC 信用均衡)与数字见 DESIGN §12.43。env:`VRF_PL_PAIR_GBITS`/`VRF_PL_FRAME_BYTES`/`VRF_PL_LLR`/`VRF_PL_FC`/`VRF_PL_PFC_DRAIN_NS`/`VRF_PL_THREADS`/`VRF_PL_SPLIT`(§12.45 拓扑分解:32×8 口平面交换机 = 40 LP,MTP-8 1.89× 加速,与单机臂结果逐位一致)。**§12.44 MTP 扩展**:`VRF_PL_THREADS>1` 走 NS3_MTP 自动分区(首发 `ScheduleWithContext` 驻留宿主 LP + 互异 1ns dither,跨 LP 聚合计数 atomic 化),末尾输出 `VRF-PLANES BHASH`;`./run-vrf-mtp.sh [2,4,8]` 四臂(默认/LLR=0/PFC drain=0/PFC drain=2000)双跑 diff BHASH,1/2/4/8 线程逐字节一致(2026-08-18)。

---


**SC-35 sue-axi-workload**
- 测什么:AXI-over-SUE 事务型 workload(§12.47):GoBackN 事务仿真+4-VC 通道分离(AW+W→VC1,AR→VC2,B→VC3,R→VC4)。验证 LLR=1 时链路损失被完全吸收(零事务回滚)vs LLR=0 时丢帧触发整事务回滚重传。
- 场景:3 节点(host0 master/host1 slave/switch2),200Gbps,100ns prop;32KB 事务×500 笔,16 帧/事务,outstanding=16;CBFC per-VC(信用窗 4096×256B/VC);VLAN PCP→VC 映射;AXI_LOSS_EVERY=100(确定性 1/N 坏帧注入,双向)。
- 断言:① completed+aborted==issued;② LLR=1:retries==0;③ LLR=0 loss≤1/1000:retries>0;④ per-VC 帧计数与 txn/retry 数精确匹配;⑤ aborted==0。
- 实测:LLR=1 四档 retries=0,avg latency≈28µs;LLR=0 loss=1/1000→retries=4,loss=1/100→retries=55;8/8 PASS。
- 设计要点:事务 FSM 在 scratch 实现;GoBackN 语义:超时→整事务重发;B 帧在 slave 收到最后一个 W 帧后发出;timeout auto=200×K×wireNs+RTT;margin;LLR=1 恒零回滚。env:AXI_RATE/AXI_TXN_BYTES/AXI_TXNS/AXI_OUTSTANDING/AXI_DATA_BYTES/AXI_CTRL_BYTES/AXI_LLR/AXI_LOSS_EVERY/AXI_TIMEOUT_NS/AXI_MAX_RETRY/AXI_THREADS/AXI_VID。

**SC-36 sue-cbfc-cellsize**
- 测什么:§12.56 CBFC CreditSize 异粒度四臂对照(UEC §5.2.5.8 method-2)——双端 MMU cell 不一致(A 原生 128/仅 2 的幂,SW 原生 56/不可配)时,"每方向以 RX 原生 cell 为准 + CC_Update 对账"方案 vs 协议 min() 基线 vs 理想同粒度。
- 场景:A—B 直连(逐臂独立拓扑+仿真会话),100Gbps,1518B,VC0 窗口 4096 credits,drain 1µs,CF 2/20µs,测量 [100,3000]µs,预入队 30000 帧由信用门自然整形(offer 恒为线速);四臂:a) A.tx=32/SW.rx=32(min 基线)、b) 32/56+CC 10µs、c) 32/56 无 CC、d) 56/56(理想)。
- 断言(判语 `CBFC CELLSIZE PASS`):a、d ≥95% 线速且互差 <5%;b ≥90%×a;c ≤75%×a 且 b>c;b CC_tx>0、c==0;SW 原生 cell 峰值计数 d<a(min() 口径放大)。
- 实测(2026-08-31):a 100.36Gbps / b 100.31(CU 锯齿峰 2388,CC 299 帧)/ **c 硬停:仅 203 帧后 CU 卡死 4060/4096(≈CL/(1−rx/tx) 帧,泄漏不可自愈,比"掉到 57%"更严重——钉死 CC 周期 dT ≤ W−BDP 约束)** / d 100.36;SW cell 峰值 a=432 vs d=252(min() 放大 71%)。
- 设计要点:预入队全量帧让 egress 信用门做唯一节拍器,达成速率纯粹由被测信用动力学决定;c 臂的硬停是设计约束的定量证据(TS-65 是其 e2e 正面镜像);CU 逐 10µs 采样输出锯齿轨迹(泄漏-对账周期可见)。env:RATE/FRAME_BYTES/CL/CC_US/DRAIN_US/WARM_US/STOP_US/N/SAMPLE_US。

## 3. 交叉索引

| 维度 | 位置 |
|---|---|
| UEC 条款 → 用例 | `docs/uec-spec-matrix.md`(58 项,52✅/6⚪;验证列指向本节用例) |
| 模块命名规范 | `docs/test-organization.md` |
| 回归基线/结果 | `docs/test-report.md`(TestSuite 61/61、scratch 47 判语) |
| 评审修复 → 用例 | `docs/review-fix-plan.md`(Stage A-E,每项注明新用例) |
| 等价/性能脚本 | `run-s8-mtp.sh` / `run-vrf-mtp.sh` / `run-s5b.sh` / `run-s5c-thpt.sh` / `run-s5e-incast.sh` / `run-scratch-checks.sh` |
