# SueNetDevice 工作原理与接口指南

> 面向需要在外部驱动这台设备的开发者——写 IO Die 转发节点、新的流量 workload、或把
> 平台对接到 astra-sim 的人。本文讲清楚**设备如何运转、每个接口为什么长这样**;API
> 签名以 `src/sue/model/sue-net-device.h` 为准,本文是理解它的路标。设计基准
> `DESIGN.md` §6;本期踩过的坑与教训见 `docs/rtt-investigation-2026-08.md`。

---

## 1. 这台设备是什么

`SueNetDevice` 是一台 L2 以太网设备,对应 Scale-Up 链路的一端。它继承自上游的
`PointToPointNetDevice`,但 **TX/RX 管线全部自研**——原因是上游基类的收发路径不是
虚函数、`PointToPointChannel` 会把帧静态分派到基类 `Receive`(绕过我们的逻辑),所以
配套的 `SueChannel` 把分派目标改成 `&SueNetDevice::Receive`(仿 fork 里 QbbNetDevice
+QbbChannel 的做法,DESIGN §6.13)。理解这一点很重要:**设备对外的行为契约由我们
自己的管线定义**,上游基类只贡献 TypeId/属性系统这些骨架。

设备内部住着三个引擎,各自负责一块协议:

- **LLR 引擎**(`SueLlrEngine`):链路级重传。给每个发出的帧编号(seq),维护重传
  缓冲;收帧侧校验序号,坏帧回 NACK、乱序帧按 GoBackN 语义丢弃等待重传。
- **CBFC 引擎**(`SueCbfcEngine`):逐链路信用流控。本端作为发送方时跟踪"已发出
  多少信用、对端确认了多少";作为接收方时计费收到的帧并在帧"排空"时通过对端发
  回 CF_Update 释放信用。
- **PFC 引擎**(`SuePfcEngine`):优先级暂停流控,与 CBFC 二选一,用于对比实验。

三个引擎由一个 `SueFcController` 门面统一挂到设备上(配置决定门面里装 CBFC 还是
PFC)。上层调用者通常感知不到引擎细节——只感知到"我发的帧有时会停在队列里一会儿"。

## 2. 一帧的旅程

理解接口之前先看数据怎么流。发送方向:

```
调用方 ──SwitchSend(qIndex, p)──▶ 8 个 VC 队列(SueEgressQueue)
                                       │
                              DequeueAndTransmit ──▶ LLR 出队门(CanTx)
                                       │                 (REPLAY/INIT 时停发新帧)
                              CBFC 信用门(CanSend) ◀───── FC 门面
                                       │
                              LLR:分配 seq、存重传缓冲、加 8B preamble
                                       │
                              TransmitStart ──▶ SueChannel ──▶ 对端
```

两个门都可能让帧**停在队列里**:LLR 处于 REPLAY 状态(正在重传一批旧帧)时不开新
帧;CBFC 信用不足(对端缓冲接近上限)时不发。这就是"入队成功 ≠ 上线"的原因——
`SwitchSend` 永远返回 true,拥塞被无损地吸收在队列里,直到条件满足才继续。

接收方向是同一条链的镜像:`SueChannel` 把帧交给 `Receive` → 剥 preamble、LLR 校验
(坏帧 NACK 后丢弃、乱序帧丢弃等重传)→ 非 SUE EtherType 的帧在这里被防御性丢弃 →
CBFC 计费(占用对端信用)→ 最后调用你注册的 **receive upcall**。所以 upcall 收到的
一定是"通过了全部协议检查的好帧",你可以放心解析。

控制帧(ACK/NACK/INIT/CF_Update 这些 9B 的 CtlOS 消息)走单独的旁路:它们没有
preamble、不占信用、在数据帧之间抢占发送,由设备内部的定时器和引擎回调自动驱动,
**外部调用者几乎从不需要碰它们**。

## 3. 发送:`SwitchSend` 的契约与那个断言

```cpp
bool SwitchSend (uint32_t qIndex, Ptr<Packet> p);
```

`qIndex` 是 egress 队列编号,**语义上就是 VC**(8 个队列一一对应 8 个 VC)。帧最终
会被交换机按线上字段分类——shim 里的 DSCP 优先,其次 VLAN 的 PCP(带 DEI 行),
都没有则用 DEFAULT_VC。**契约要求你传的 qIndex 等于帧自身的这个推导结果。**

这个要求不是风格建议,是带断言强制的,而断言背后是一次真实事故(§12.50):AXI
workload 曾经把 qIndex 硬编码为 0,而帧带着 VLAN PCP=1。结果发送端的计费
(OnTx,按队列 VC)落在 VC0、交换机的退款(RefundCredit,按线上推导)落在 VC1——
两个 VC 各自"工作正常",但合成效果是 VC1 只有退款没有计费,在途信用
CU=(CC−CF) mod 2^15 翻成一个巨大值,`CanSend` 判定窗口已满,发送端被永久阻塞,
带宽腰斩。排查花了一周,最后靠 credit 绝对值对账才定位。所以现在:

```cpp
NS_ASSERT_MSG (qIndex == vc, "SwitchSend qIndex=... != frame-derived VC=...");
```

debug 构建下不满足即 abort。写新调用方时,推荐像 TestSuite 的 SendOne 那样,发帧前
用公开的 `SueConfig::DscpToVc / PcpToVc(pcp, dei) / DefaultVc` 按帧头推导(与设备
内部 `ComputeVc` 同一张表、同一优先级),不要手抄数字。

关于帧本身:传给 `SwitchSend` 的 packet 应该是"MAC 帧"——即你已经 `AddHeader` 了
`SueHeader`(可选地含 VLAN/shim)。**不要自己加 LLR preamble**,那是设备在实际
出队时统一做的(重传缓冲里存的是无 preamble 的干净拷贝,这是 §12.42 修过的又一个
细节:提前加会导致重传时双重 preamble)。

还有一个容易误解的点:计费不发生在 `SwitchSend` 调用那一刻,而是帧真正离开队列
(DequeueAndTransmit)时。如果你在测试里发完帧立刻去读 `GetSvcCc`,帧可能还躺在
队列里没计费——先 `Simulator::Run()` 排空事件。

## 4. 接收:upcall 的设计

```cpp
typedef Callback<void, Ptr<SueNetDevice>, Ptr<Packet>, SueHeader> ReceiveUpcall;
void SetReceiveUpcall (ReceiveUpcall cb);
```

注册的回调在**每收到一个好帧时被同步调用**,参数给全了三样东西:哪个设备收的、
帧本体(payload 从 packet 读,自有协议头用 `CopyData` 取前几字节)、已解析的
SueHeader(省你一次 Peek)。

一个关键的设计事实:回调运行在**收包节点的 LP 上下文**里(下一节解释 LP)。实践
含义是——在回调里直接 `SwitchSend` 回发帧(slave 收到读请求立刻回数据)是安全且
常规的模式,AXI workload 的 SlaveRx 就是这么写的。你在回调里做的事都发生在"这台
设备的执行流"里,不存在跨线程问题。

默认行为:不注册 upcall 时,设备装一个计数器 upcall(拓扑入口统一装配)。所以
`SetReceiveUpcall` 是**覆盖**语义——注册你自己的回调后,默认计数不再累加,如果
你还想要默认计数就自己在回调里补。

## 5. 信用的运行时含义:我的帧为什么停在队列里

外部调用方对 CBFC 唯一需要建立的感觉:**发送速率由对端的"排空速率"决定**。你发的
每一帧都占用对端一个信用(按 creditSize 向上取整);对端把帧交付/排空后经 CF_Update
把信用还给你,你才能继续发。突发没关系——窗口(CL)允许在途;持续超发才会遇到
`CanSend` 假,帧停在队列。

诊断时看三个数:`GetSvcCu(vc)` 是在途信用,**健康状态必须 ≤ CL**;`cc_update_tx/rx`
是周期对账消息;`credit_exhausted` 计数告诉你出队门挡了多少次。如果 CU 出现巨大值
(比如 2 万多而窗口才 4096),那不是"拥塞",是 mod 2^15 计数器翻转——意味着某处
计费与退款没对上,**是 bug 的第一信号**。正因为如此,AXI workload 每次运行结束都
做一次四值对账(发送端 CC/CF vs 接收端 RCC/RCF,见 §8),任何新转发路径上线前也
应该这么验一遍。

LLR 侧还有一个会停发新帧的状态:REPLAY(收到 NACK 正在重传一批旧帧)。这段时间
队列里的新帧完全不动,属于 GoBackN 协议的正常行为——高 RTT 下它的代价见
`docs/app-notes.md` 实例 3。

## 6. 控制帧:知道存在即可

`SendCtlAckNack / SendCtlInit / SendCtlInitEcho / SendCtlCfUpdate / SendCtlCcUpdate /
SendCtlPause` 这组接口存在,但**正常开发不应直接调用**——LLR 引擎经 sink 回调自动
发 ACK/NACK/INIT,CBFC 的 CF/CC 由设备的阈值+定时器驱动。它们公开是因为 TestSuite
需要逐类注入测试。唯一值得知道的公共属性:所有 CtlOS 共享一个 pacing 门
(`SetCtlOsTargetSpacing(bytes)` 配置,即 spec 的 ctlos_target_spacing),控制帧在
数据帧间抢占发送但彼此保持节奏——ACK 流的节奏曾被误判为"饿死 CF_Update",实测
证明 pacing 正常(排查档案 §3),不需要外部干预。

## 7. 交换机与"admit-then-refund"模式

`SUESwitchNode` 是设备的第一个也是最重要的外部调用方,它示范了写转发节点的全部
要点。核心循环:upcall 收帧(`SwitchReceiveFromDevice`)→ 查路由(VRF×XPU-id)
→ 把帧交给出口设备的 `SwitchSend` → **立刻对入口设备调 `RefundCredit`**。

为什么最后一步必须存在:交换机入口收到帧时已经计费了(占了发送方的信用),如果
帧被转发出去后不释放,信用就永久滞留在"已收未退"状态,发送方窗口逐渐漏死。同
样,任何丢弃路径(lookup miss、TTL 归零)也必须退款——丢弃的帧占过的信用和转发
的帧一样真实。这套"准入即退款"的对称性是 CBFC 无损语义的一半,另一半是出口的
深度门 `EgressCanAccept`(出口队列满时帧挂起、不退款,背压由此产生)。

**写给 IO Die 实现者**:如果你要在主 Die 和网络之间加一个转发节点,你就是在扮演
SUESwitchNode 的角色,必须完整复制这套语义——转发成功退款、丢弃也退款、出口满则
挂起不退。漏掉任何一条,信用账就会不平衡,而它的症状(CU 假满)要很久才暴露且
极难追。先抄 `sue-switch-node.cc` 的 Forward/ServicePending,再改你的 D2D 逻辑。

## 8. 观测:计数器与对账

LLR 引擎暴露 22 项 UEC Table 5-13 计数器(`GetCounters()`),CBFC 暴露四组累计值。
日常最有用的对账四值(credit audit):

| 值 | 在谁身上 | 含义 |
|---|---|---|
| S_VC_CC | 发送端 | 我已发出多少信用 |
| S_VC_CF | 发送端 | 对端已确认多少(采纳自 CF_Update) |
| R_VC_CC | 接收端 | 我已计费多少 |
| R_VC_CF | 接收端 | 我已释放多少 |

健康链路上:发送端 CC == 接收端 RCC,发送端 CF == 接收端 RCF,CU=CC−CF ≤ CL。
这个对账已固化成 TestSuite 用例(`SueVcCreditAuditTestCase`)和 scratch 断言——
它是"VC 映射正确、退款链完整"的一击式判据,排查档案 §9 把"对账先于推测"列为
第一条方法论,就是这次事故换来的。

## 9. 装配与顺序

最小可用的设备装配(SueEntryDispatch 内部做的也是这些,顺序同理):

```cpp
Ptr<SueNetDevice> dev = CreateObject<SueNetDevice> ();
node->AddDevice (dev);
dev->Attach (ch);                                // SueChannel:静态分派的另一半
dev->SetDataRate (DataRate ("200Gbps"));         // ① 必须先于一切引擎配置
dev->SetEgressQueue (CreateObject<SueEgressQueue> ());
dev->SetFcController (fc);                       // 内含 CBFC 或 PFC 引擎
dev->SetLlrEngine (CreateObject<SueLlrEngine> ()); // 可选;自动置 llrEnabled
dev->SetReceiveUpcall (MakeCallback (&MyRx));
```

① 顺序敏感的原因:IFG 折算、CF 定时器字节数→时间的换算、LLR replay timer 的自动
推导(10×RTT)全部要乘端口速率——速率还是上游占位值(32kb/s)时配置会算出荒谬
的结果。正常使用走 `SueEntryDispatch(topoFile, confFile)` 一步到位:拓扑文件声明
节点/链路/FC 模式,conf 文件声明全部 GlobalValue,LLDP 协商和路由表它都管;手工
装配只留给 TestSuite 和非常规实验。

抓包观测是旁路能力:`SUE_PCAP=1` 环境变量打开后,挂在 `SueChannel` 上的 trace
输出 pcap+文本双格式,帧的 preamble/CtlOS/AFH 全解析(`tools/` 有 Wireshark
插件)。注意标准的 `PointToPointHelper::EnablePcap` 抓不到 SUE 流量——数据面绕过
了基类路径,必须走这套旁路工具。

## 10. 多线程(MTP)下的执行模型

单线程时可以跳过本节。MTP 模式下整个仿真被切成若干 LP(逻辑进程),**每个节点
属于一个 LP,节点上的事件只在所属 LP 的线程上执行**。设备的定时器、引擎回调、
你的 upcall 天然满足这个约束,因为它们都由设备自己的事件驱动。

会出问题的是**外部调度**:如果你在公共 LP(比如 main 里裸调 `Simulator::Schedule`)
安排了一个将来碰这台设备的动作,它会在错误的 LP 上执行——MTP 的驻留检查断言
(`SueMtpResidencyCheck`)会让它当场 abort,而不是留下静默的数据竞争。安全写法:
首次调度用 `Simulator::ScheduleWithContext(nodeId, delay, ...)`,之后自链式普通
`Schedule` 会继承正确的上下文。AXI workload 的所有事务调度都是这个模式。

## 11. 几个常见问题

**"我发了 100 帧为什么对端只收到 90?"** 先查对端 upcall 是不是被你覆盖后忘了计
数;再查 LLR 计数器有没有 rx_missing(乱序被丢等待重传,最终会补齐但有时间差);
最后跑对账四值。不要先怀疑丢包——这套数据面设计上不丢(CBFC 无损 + LLR 重传),
丢帧几乎总有计数器佐证的原因。

**"SwitchSend 返回 true 但线上没帧?"** 帧在队列里等信用或等 LLR 出 REPLAY。看
CU 和 credit_exhausted;长时间不动才是问题(那是信用账坏了,见 §5)。

**"我想给帧选个非默认 VC?"** 构造帧头时带上 VLAN PCP(或 shim DSCP),并把同一
个推导值传给 qIndex。VC 的语义(为什么 AW+W、AR、B、R 各占一个)见 DESIGN §7
与 app-notes 实例 2——四个通道各占一个 VC 是 AXI 的设计,不是设备要求。

**"在回调里能发帧吗?"** 能,而且推荐(§4)。在回调里做 `ScheduleWithContext`
的后续调度同样安全。
