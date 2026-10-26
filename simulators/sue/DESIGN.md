# SUE Scale-Up Ethernet 仿真平台 - 设计文档

> 用途: 后续**代码实现 / 代码评审 / 规格验证**的唯一基准。实现与本档不一致处以本档为准。
> 版本管理: 本档纳入 git 仓库根;变更须经评审后提交,提交信息说明变更点;严禁直接覆盖或删除,历史可回溯。
> 状态: 已定稿(纳入评审意见修订:自研 TX/RX 管线 + SueChannel、LLR 计数器更正为 22、IFG 自管、PFC 帧格式、Time 精度等),进入 S0 实现。S5a(单进程多线程适配)已闭环,见 §10.5 / §11 / §12#19。

---

## 1. 概述

### 1.1 目标
基于 `ns-3-alibabacloud` 仿真基座(已拷入工程 `ns-3-alibabacloud/` 作为受控基线),以**新增模块**方式实现 Scale-Up 以太网(SUE)仿真平台,支持:
- **SUE Gen1 Lite 转发**(AFH Gen1,无 RH 头)
- **LLR**(Link Level Retry,逐跳重传)
- **CBFC**(Credit-Based Flow Control)+ **PFC**(保留,用于与 CBFC 对比)
- **多线程**(`NS3_MTP`,512 卡 / 32 交换机 / 多平面;S5a 已闭环:单进程多线程 × 节点上下文路由,§10.5)

### 1.2 范围
| 项 | 是否纳入 | 说明 |
|---|---|---|
| AFH Gen1 转发(±shim, ±vlan 四组合) | ✅ | 基本转发 |
| LLR(状态机/重传/CtlOS) | ✅ | 含异常处理(§6.10) |
| CBFC(8 VC,信用门控,CF/CC_Update) | ✅ | 初始化配置直配;`LLDP_NEGOTIATE=1` 走 UEC §5.3 TLV + §5.2.8.4 RTS/RTR 握手(§12.32) |
| PFC(8 优先级,pause/resume) | ✅ | 与 CBFC 二选一(配置),用于效果对比 |
| SueChannel(专用通道,静态分派到 SueNetDevice::Receive) | ✅ | 见 §6.1/§6.13(基座 channel 不会调子类 Receive) |
| VC 映射(DSCP->VC, PCP->VC) | ✅ | 表驱动通用流类别映射:`DSCP_TO_VC_MAP`/`PCP_TO_VC_MAP`(16 项,含 DEI 行)/`DEFAULT_VC`,host 与交换机同一张表端到端;默认直映零回归(§7/§12.8) |
| 拓扑(单跳交换,多 plane/多 rail) | ✅ | XPU-id 高 10 位 LPM 路由 |
| 多线程(`NS3_MTP`) | ✅(必选) | 单进程多线程(本期,**S5a 已闭环**:自动分区 × 节点上下文路由,§10.5);MPI 后续 |
| 完备计数器与调试 | ✅ | UEC 全部计数器 + 日志(§6.11) |
| LLR preamble(携带 20b seq) | ✅ | ns-3 建模见 §5.4 |
| CtlOS 带宽建模 | ✅ | 见 §5.7 |
| RH 头 / SUE 完整可靠传输 / GoBackN | ❌ | 预留扩展位,不实现 |
| 自适应路由(AR) | ❌ | 仅确定性路由(DR/ECMP) |
| 拥塞控制(CNM-2/TRIM/PDN-2/ECN 动作) | ❌(本期) | 解析字段不动作 |
| AFH Gen2 / FEC / PCS 64B66B | ❌ | 预留 |
| Workload(AICB/SimCCL) | ❌(本期) | `SueLiteEndpoint` 自模拟发包,接口预留对接 astra-sim |

### 1.3 与 scale-out 的关系
- 现有 scale-out(RoCEv2/DCQCN/PFC)功能**完整保留**,配置使能切换。
- scale-up 作**独立新模块** `sue`,不修改现有定制模块源码;仅入口处 1 个分发分支(§6.8)。
- 同一二进制可混合拓扑(SUESwitch 与 SwitchNode 共存),便于对照。
- **已实现的集成面**:`network_frontend/sue-entry`(入口分发 + `SetupSueNetwork`)+ `network_frontend/sue-config`(GlobalValue + `SueReadConf` + `ApplySueConfig`)。`sue-helper` / `sue-topology` / `sue-lite-endpoint` / `sue-mmu` 为后续工作(§4.1 注明),本次不实现混合拓扑 demo,分发契约见 §6.8。

---

## 2. 设计原则与约束

1. **新模块优先**:scale-up 逻辑置于 `ns-3-alibabacloud/simulation/src/sue/`;现有定制模块(QbbNetDevice/SwitchNode/NVSwitchNode/CustomHeader/RdmaHw/SwitchMmu)**不改源码**。
2. **继承上游基类 + 自研管线**:继承 `PointToPointNetDevice`/`Node`/`Application`;但 **TX/RX 管线自研**(基类 `Receive/TransmitStart/TransmitComplete` 非虚、`DequeueAndTransmit` 不存在,且 `PointToPointChannel` 静态分派到基类 `Receive`--见 §6.1/§6.13),仿 `QbbNetDevice`+`QbbChannel` 套路:配套 `SueChannel` 静态分派到 `&SueNetDevice::Receive`。
3. **配置使能**:`ENABLE_SCALEUP` 与各特性开关;未使能时现状不变。
4. **多线程必选**:从设计起即考虑 `NS3_MTP`(临界区/线程安全),非后置;S5a 已按"**节点上下文路由、不加全局锁**"落地——自动分区下节点状态单 LP 独占,explicitCriticalSection 全局单锁会串行化,留 MPI 后续(§10.5)。
5. **小步验证**:按 §11 阶段交付。
6. **前向兼容**:为 RH/AR/Gen2/CC 预留扩展点。

---

## 3. 参考与术语

| 术语 | 含义 | 参考 |
|---|---|---|
| AFH Gen1 | AI Fabric Header Gen1,L2 转发头 | SUE §3.5.3, `Switch-ASIC-AFH.md` §6.6 |
| shim header | AFH Gen1 可选压缩头,2B 或 3B(由 F 位决定) | `Switch-ASIC-AFH.md` Table 29 |
| LLR | Link Level Retry,逐跳重传 | UEC §5.1, SUE §3.5.5 |
| CBFC | Credit-Based Flow Control | UEC §5.2, SUE §3.5.7 |
| PFC | Priority Flow Control(IEEE 802.1Qbb) | SUE §3.5.6 |
| CtlOS | Control Ordered Set,8B 链路控制信令 | UEC §5.1.3, §5.2.11 |
| VC | Virtual Channel(本期 8 个) | - |
| XPU-id | 目的加速器标识(10 位),置于 MAC DA 高 10 位 | SUE §3.5.3, 本设计 §5.1 |
| SUESwitch / SueNetDevice / SueChannel | 新增节点/设备/通道类型 | 本设计 |

---

## 4. 总体架构

### 4.1 模块划分
```
ns-3-alibabacloud/simulation/src/sue/
├── model/
│   ├── sue-llr-preamble-header.{h,cc} # LLR preamble(8B, 携 20b seq) - §5.4
│   ├── sue-header.{h,cc}           # AFH Gen1 帧(MACDA/VLAN/EtherType/shim)
│   ├── sue-shim-header.h           # shim(2B/3B,F 位;header-only)
│   ├── sue-llr-ctlos-header.{h,cc} # LLR CtlOS 8B 负载头(ACK/NACK/INIT/INIT_ECHO) - §5.5
│   ├── sue-cbfc-header.{h,cc}      # CF_Update 8B 负载头(2 VC) - §5.6
│   ├── sue-cbfc-cc-header.{h,cc}   # CC_Update 完整 64B 以太网帧头 - §5.6
│   ├── sue-pfc-header.{h,cc}       # PFC pause 帧(独立 64B,§5.10)
│   ├── sue-ctl-header.{h,cc}       # 控制帧 1B CtlType 判别字节(§5.7)
│   ├── sue-ctl-tag.{h,cc}          # 控制帧 metadata tag(RX 分发,§6.1)
│   ├── sue-frame-bad-tag.{h,cc}    # POISONED 帧坏帧标记
│   ├── sue-channel.{h,cc}          # SueChannel:静态分派到 &SueNetDevice::Receive - §6.13
│   ├── sue-net-device.{h,cc}       # L2 设备:自研 TX/RX + MAC + LLR + FC(CBFC/PFC)
│   ├── sue-llr-engine.{h,cc}       # LLR TX/RX 状态机/replay/异常
│   ├── sue-cbfc-engine.{h,cc}      # CBFC 信用引擎
│   ├── sue-pfc-engine.{h,cc}       # PFC 引擎(与 CBFC 二选一,含共享池动态阈值)
│   ├── sue-fc-controller.{h,cc}    # 流控策略门面(CBFC|PFC, 配置选择)
│   ├── sue-switch-node.{h,cc}      # SUESwitch: AFH 转发
│   ├── sue-egress-queue.{h,cc}     # 8-VC 出队 + 流控门控
│   ├── sue-error-model.{h,cc}      # 确定性 corruptor(坏帧/POISONED,§10.1)
│   ├── sue-routing.{h,cc}          # LPM 路由表(XPU-id 高 10 位)
│   ├── sue-llr-counters.{h,cc}     # LLR 22 项计数器 - §6.11
│   └── sue-cbfc-counters.{h,cc}    # CBFC 计数器 - §6.11
├── helper/                        # 后续工作(未实现)
│   ├── sue-helper.{h,cc}          # 拓扑安装
│   └── sue-topology.{h,cc}        # 拓扑解析/建链/路由
├── network_frontend/              # 已实现
│   ├── sue-entry.{h,cc}           # 入口分发 + SetupSueNetwork(§6.8)
│   └── sue-config.{h,cc}          # GlobalValue 表 + SueReadConf + ApplySueConfig(§9)
└── test/
    └── sue-test.cc                # 阶段单测(61 例,§11)
```

> 注:`sue-helper`/`sue-topology`/`sue-mmu`/`sue-lite-endpoint` 尚未实现(标记为后续工作);本次集成面 = `sue-entry` + `sue-config`。`sue-mmu` 的 PFC 动态阈值算法已内联进 `SuePfcEngine`(§6.4)。

### 4.2 数据流(SUESwitch 转发)
```
[SueChannel]  (静态分派到 &SueNetDevice::Receive, §6.13)
 -> SueNetDevice::Receive()
    -> SueLlrPreamble 解析(seq/FCS; SueErrorModel 判坏帧)
    -> LLR RX(FCS/seq 校验, ACK/NACK 生成, 重传吸收; 异常见 §6.10)
    -> FC RX(CBFC: R_VC_CC+=/R_VC_CF+=, 生成 CF_Update; 或 PFC: 处理 pause/resume)
    -> (good frame 上送) SUESwitchNode::SwitchReceiveFromDevice()
        -> SueHeader 解析(EtherType->判 shim 与否; MACDA; VLAN; shim)
        -> VC 派生(DSCP 优先, 否则 PCP, 否则 VC0)
        -> LPM 路由查表(MACDA 高 10 位 XPU-id -> port/ECMP, 无 AR)
        -> shim TTL-- ; TTL==0 丢弃+计数
        -> 入 egress VC 队列
 -> egress SueNetDevice::SwitchSend() -> DequeueAndTransmit(自研)
    -> FC TX 门控(CBFC 信用 或 PFC 未 pause)
    -> LLR TX(分配 seq, 存 replay, 装 preamble)
    -> TransmitStart(自研) -> [SueChannel]
控制面: LLR CtlOS / CBFC CF_Update / PFC 帧 走高优先级控制队列,绕过 FC 信用
```

### 4.3 与现有模块的接触面
| 现有模块 | 接触 | 改源码? |
|---|---|---|
| `PointToPointNetDevice`/`Node`/`PointToPointChannel`/`Application`(上游基类) | 继承 | 否(上游) |
| `QbbNetDevice`+`QbbChannel`(自研管线+专用通道的**范式参考**) | 仅参考实现 | 否 |
| `SwitchNode`/`NVSwitchNode`/`CustomHeader`/`RdmaHw`/`SwitchMmu` | 不接触(仅参考) | 否 |
| astra-sim `entry.h`/`common.h` | 入口分发 1 处 if | 最小(1 分支) |

---

## 5. 报文格式设计

### 5.1 AFH Gen1 帧总体结构
```
+──────────+──────────+────────+──────────+──────────+─────────+──────────+
| Preamble | MAC DA(6)| MAC SA | [VLAN(4)]| EtherType| [shim]  | Payload  |
| (8B,LLR) | XPU-id高10| (6B)   | 802.1Q   | (2B)      | 2B/3B   | ...      |
+──────────+──────────+────────+──────────+──────────+─────────+──────────+
```
- **MAC DA**: 标准 6B(48b)。**XPU-id 占高 10 位(bit47..38)**,用于 LPM 转发查表(§6.9)。非标准 MAC 编址,不应用 I/G、U/L 位语义;高 10 位全部承载 XPU-id(2^10=1024,覆盖 512 卡目标)。
- **MAC SA**: 标准 6B。
- **VLAN**: 可选 802.1Q(PCP 3 位用于无 shim 时的 VC 映射)。
- **EtherType**: 两个**对称的 SUE 协议标识**(§5.2)。
- **shim**: 可选 2B/3B(由 shim 内 F 位决定,§5.3);shim **有无**由逐 EtherType 的配置决定(§5.2),不在报文里单独指示。

### 5.2 双 EtherType 配置(对称标识;shim 有无 = 逐 etype 配置)
提供**两个对称的 SUE EtherType**值(默认私有值 `0xFCF1`/`0xFCF2`,后续有标准值再改)。二者**不是**"一个带 shim / 一个不带 shim"的角色——那只是其中一种配置用法。**shim 有无由逐 etype 的配置决定**(芯片模型:配置某 EtherType 时"一起配了 shim header"→ 该 EtherType 的帧按带 shim 解析;"没配 shim header"→ 按无 shim 解析):
- `SUE_ETH_TYPE_0` / `SUE_ETH_TYPE_1`:对称协议标识,任一都可能带或不带 shim。
- `SUE_ETH_TYPE_0_HAS_SHIM` / `SUE_ETH_TYPE_1_HAS_SHIM`:该 EtherType 的帧是否按带 shim 解析(默认 `0`/`1`,即默认配置恰好是"eth0 无 shim、eth1 带 shim"——仅为维持既有行为,非协议规定)。
- 接收端 `SueHeader::Deserialize` 对**任一** SUE EtherType 查 `SueConfig::HasShim(et)`:真 → 按 shim 解析(F 位选 2B/3B);假 → 无 shim。shim 有无**不在报文字段中指示**,纯由配置指示。
- 发送端:带 shim 的帧须选一个 `*_HAS_SHIM=1` 的 EtherType(默认 `SUE_ETH_TYPE_1`);`SetShim`/`ClearShim` 只切 shim 存在标志,**不改 EtherType**。

**已实现**:四个值为运行时 `GlobalValue`(`SUE_ETH_TYPE_0/1` 与 `*_HAS_SHIM`);`SueHeader` 构造(默认取 `SUE_ETH_TYPE_0`)与 `Deserialize`(`SueConfig::HasShim`)读取(首次使用缓存)。配置须在流量开始前应用(`SueReadConf`/`Config::SetGlobal`),见 §9。
> **建帧侧一致性**:测试/scratch 用编译期常量 `SUE_ETH_TYPE_0`/`SUE_ETH_TYPE_1`(与默认 GlobalValue 一致)建帧;若 conf 覆盖了 EtherType 值,建帧侧须改用 `SueConfig::EthType0()/EthType1()`(或与 conf 保持一致),否则接收端按新配置解析会与帧内容不符。

### 5.3 shim header 位级布局

依据 `Switch-ASIC-AFH.md` Table 29(TTL 0–4b, Flow Label 0–8b)。**`F` 位(bit1,即"第2 bit")为长度选择器**:值 0 -> 2B,值 1 -> 3B。下表字段后为**位宽**,`F=` 为**该变体下 F 的取值**:

| bit 偏移 | 字段 | 位宽 | 含义 |
|---|---|---|---|
| 0 | AR | 1 | 自适应路由资格(本期保留不动作;0=DR) |
| 1 | F | 1 | **长度选择器:0=2B,1=3B** |
| 2 | C | 1 | CNM 生成资格(本期解析不动作) |
| 3 | CNM | 1 | 本帧是否 CNM(本期解析不动作) |
| 4–7 | TTL | 4 | 逐跳减1;TTL==0 丢弃(仅 shim 在场时) |
| 8–13 | DSCP | 6 | VC 映射来源 |
| 14–15 | ECN | 2 | ECN(本期解析不动作) |
| 16–23 | Flow Label | 8 | 熵/ECMP hash 输入(**仅 F=1 时存在**) |

**2-byte 变体(F=0,共 16 bit,无 Flow Label):**
```
Byte0: [ AR | F=0 | C | CNM | TTL(4b) ]
Byte1: [ DSCP(6b)      | ECN(2b)      ]
```
**3-byte 变体(F=1,共 24 bit,含 Flow Label):**
```
Byte0: [ AR | F=1 | C | CNM | TTL(4b) ]
Byte1: [ DSCP(6b)      | ECN(2b)      ]
Byte2: [ Flow Label(8b)               ]
```
> 字段顺序、网络序以此为准。`AR/C/CNM/ECN` 本期**仅解析存字段**,不触发转发/拥塞动作,为后续扩展预留。

### 5.4 LLR preamble 的 ns-3 建模

**问题**:LLR-eligible 帧在 preamble 中携带 20-bit seq,ns-3 能否仿真?

**结论:可以。** 采用"preamble 作 ns-3 Header"方案:

- 新增 `SueLlrPreambleHeader`(8B),在 `SueNetDevice` 发送前 prepend 到 `Packet`,接收后 strip。内容即 UEC §5.1.1.1:
  ```
  B0=0xFB; B1=[seq[19:16]|0x7](eligible) 或 [0x55](ineligible);
  B2=seq[15:8]; B3=seq[7:0]; B4-6=0x55; B7=0xD5(SFD)
  ```
- **功能正确性**:seq 是 `Packet` 的一部分被序列化,收端 `SueNetDevice` 直接解析 -> ACK/NACK/重传逻辑可用。
- **带宽正确性**:8B 计入 `Packet::GetSize()` -> 通道 `transmit_time = size*8/rate` **自然包含 preamble 带宽**。
- **CBFC 信用一致性**(评审意见):真机 CBFC 按 MAC 帧计信用、**不含 preamble**;而 preamble 计入 `Packet::GetSize()` 会使 `pktCredits` 多算 8B。故 `SueCbfcEngine` 计算信用时用 **MAC 帧字节数(扣除 preamble)**,即 `pktCredits = roundup((macFrameBytes + PktOvhd)/CreditSize)`,**不把 preamble 计入信用**。 RX 侧:`SueNetDevice::Receive` 须先 strip `SueLlrPreambleHeader` 再 `OnRx`/`OnDrain`(S3 接入 preamble 后),确保信用字节口径为 MAC 帧(不含 preamble)。
- **IFG**(帧间隔 12B):基类 `m_tInterframeGap` 为私有,且加 IFG 的基类 `TransmitComplete` 非虚、在自研管线下不会运行;故 **IFG 由 `SueNetDevice::TransmitComplete`(自研)在发送完成时自行加入**(`m_ifg = 12B wire-time`,与 §10.1 一致)。
- **ns-3 现状对照**:ns-3 无现成 preamble Header(P2P 用 PPP 帧、无 preamble;IFG 以 `m_tInterframeGap` 时间间隔建模,但私有且绑定基类非虚 `TransmitComplete`)。本方案 preamble 用 Header 惯例、IFG 在自研发送完成里加,是适配本基座的自然做法。
- **LLR-ineligible 数据帧**(BEST_EFFORT TX / `llr_mode_remote` OFF):**仍装 8B preamble**(B1=0x55 标准格式,`SetEligible(false,0)`),RX 先剥 8B 再按 `mode_local` 门控(ON→不过滤直通 / OFF→直通)——链路两端恒 8B 线格式对齐(§6.10"关闭 TX 相"实验因此修复)。**CtlOS 控制帧**(ACK/NACK/INIT/INIT_ECHO、CF_Update):**不装 preamble**——`TransmitControl` 直发,RX 先查 `SueCtlTag` 分发、不剥 preamble(§5.6/§5.10/§6.1);带宽按 9B 短报文 / 64B 真帧计(§5.7)。本条取代早期"ineligible 帧不装 8B"表述(以实现为准)。
- > 备选(Tag 方案)被否决:Tag 不计入 Packet 大小,无法自动反映 preamble 带宽,且 seq 仍需额外承载。

### 5.5 LLR CtlOS 格式(UEC §5.1)
**已实现(on-wire 9B)** = 1B `SueCtlHeader`(CtlType 判别字节)+ 8B `SueLlrCtlOsHeader`;高优先级,**LLR-ineligible,不消耗 CBFC 信用**,不装 preamble(§5.4)。
| CtlType | 子类型 | 携带 | 用途 |
|---|---|---|---|
| LLR_ACK | 0x01 | 20b seq | 释放对端 replay |
| LLR_NACK | 0x02 | 20b seq | 触发对端重传 |
| LLR_INIT | 0x03 | 20b init_seq + 16b init_data | 初始化 next_rx_seq(init_data 恒 0) |
| LLR_INIT_ECHO | 0x04 | 同上 | 回执就绪(init_data 恒 0) |

8B `SueLlrCtlOsHeader` 字节布局: D0=subtype; D1=`(seq[19:16]<<4) | 0x07`(低半字节 0x7 = **eligible 标记**); D2=seq[15:8]; D3=seq[7:0]; D4–D5=init_data(仅 INIT/ECHO,发送侧恒 0); D6–D7=`0x55`(reserved)。
> 原表 "Lane0=0x5C / O-code=0x6" **未在代码复刻**:类型判别由独立 1B `SueCtlHeader` 承载;`SueCtlTag` 仅作进程内 metadata,RX 分发不解析线上字段(§6.1)。

### 5.6 CBFC 报文格式(UEC §5.2)
实现于 `SueCbfcHeader`(`sue-cbfc-header`)。
**CF_Update CtlOS(已实现 on-wire 9B = 1B `SueCtlHeader` + 8B `SueCbfcCtlOsHeader`,2 个 VC):**
```
SueCtlHeader: CtlType=0x10(CF_UPDATE)
D0-D1 = 0x55 (reserved)
D2[7:3]=CF1_VC(5b) | D2[2:0]=CF1_count[14:12]
D3    = CF1_count[11:4]
D4[7:4]=CF1_count[3:0] | D4[3:0]=0x55 (UEC 此处为 O-code)
D5[7:3]=CF2_VC(5b) | D5[2:0]=CF2_count[14:12]
D6    = CF2_count[11:4]
D7[7:4]=CF2_count[3:0] | D7[3:0]=0x55
```
> count 为 **15b**,与引擎 `CF_MOD = 2^15` 一致(§6.3)。曾简化为 11b,计数 ≥2048 会静默截断,已扩回 15b(5b VC + 15b count = 20b/VC,8B 净余 24B)。原表 "Lane0=0x5C / O-code=0x6" 未复刻,类型由 `SueCtlHeader` 承载。
**CC_Update(64B Ethernet 帧,16 VC):**
```
MACDA|MACSA|EtherType=0x8808|Opcode=0xFFFE|CID=FA-7A-CB
|MsgType(0x01=VC[0:15])|16×20b CC(40B)|FCS(4B)
```
本期 8 VC,单帧(MsgType=0x01),VC[8:15] 填 0。
**已实现**(`SueCbfcCcHeader`):真实 64B 以太网帧整包发送(MACDA=01-80-C2-00-00-01 慢协议多播、MACSA=本设备地址、CID=FA-7A-CB;**MACDA/CID 可配**,§5.2.6.2 建议级:`CBFC_CC_MACDA`/`CBFC_CC_CID` 键 + 设备 `SetCcUpdateMacDa/SetCcUpdateCid`,默认=规格值逐字节不变),16×20b CC 按 MSB-first 连续位打包进 frame[20..59],FCS=对前 60B 的 CRC-32(big-endian)。**不挂 SueCtlHeader**(线上由 EtherType/Opcode 自标识);RX 经 `SueCtlTag` 分发到 `OnCcUpdate`。FCS 只做带宽/格式保真,不校验(错误模型独立)。

> **CF_Update 生成(已实现细节,决策 §12.2/§12.20/§12.37)**:`SetCbfcEngine` 把 `m_cfSink` 自动接到 `SendCtlCfUpdate`;`OnDrain` 在 **CF_min/CF_max 定时器与阈值均未配置时**保持**每帧直发**旧行为(测试/非配置设备)。配置 `CBFC_CF_MIN_TIMER_BYTES`/`CBFC_CF_MAX_TIMER_BYTES`/`CBFC_CF_THRESHOLD_BYTES`(§9)后改由 **UEC §5.2.6.1 双触发 + SDK 数据量触发**门控:触发 (a) `R_VC_CF` 更新(`OnDrain`→`OnCfDirty` 置脏位)且 **CF_min one-shot** 到期 → 批量上报脏 VC(`SendCfUpdateBatch`,2 VC/8B 消息);触发 (b) **CF_max 周期定时器**到期 → 全量上报所有 VC(防丢失);触发 (c) **阈值数据量触发**(SDK credit_threshold 对等物):累计释放字节跨过 2075B(预设值)即上报,逐量子(§12.37 多量子修复)。one-shot 的 fire 时刻由共享 `m_lastCfUpdate` 算**绝对时刻**(`max(Now, last+CF_min)`),再以 `arm-Now` 相对延迟调度——与"哪个 drain 触发 arm"无关 → fire 时刻/发送计数确定性(§10.5 等价)。CF_max 到期 Cancel 未决 one-shot(全量已覆盖)。速率上限由设备**固定 400B CtlOS 节流门**兜底(CTLOS_MIN_SPACING_BYTES,§5.2.11.2,§12.26)。
> **CC_Update 生成**:`StartCcUpdateTimer(period)` 周期驱动 `OnCcUpdateTimer`→`SendCtlCcUpdate`;replay 窗口内被抑制(规则 9,`SendCtlCcUpdate` 顶部查 `m_cbfc->GetInLlrWindow()`,§5.2.9)。

### 5.7 CtlOS / 控制帧的带宽建模

**问题**:CtlOS 码流,现有仿真是否占用带宽?

**结论:本期显式建模并占用带宽。**

- 所有控制信令建模为独立 `Packet`,经**同一 `SueChannel`** 发送。**PFC 与 CC_Update 为真实以太网帧,满 64B 占用真实带宽**(见 §5.6/§5.10);LLR CtlOS(ACK/NACK/INIT/INIT_ECHO)与 CBFC CF_Update 维持**短报文近似**:1B `SueCtlHeader` + 8B 负载,共 9B。
- `SueChannel` 为单发射机,控制帧与数据帧**串行化** -> **占用链路带宽**(与真机控制信令 <2% 占比一致,比例自动正确)。
- 优先级:控制帧入**高优先级控制队列**(`m_ctlQueue`,忙时入队,`TransmitComplete` 中先于 retx/数据出队)。**已实现为单一 FIFO,队列内部无 UEC 子优先级**(INIT>ACK/NACK>CF_Update 仅体现为"控制>数据"这一层);PFC/CC_Update 经 `TransmitControl` 完全绕过 egress 队列(无 VC 分类/无"BE VC"概念),语义上仅"绕过 FC 信用"。**决策(§12.11):记为简化,不做子优先级/独立控制队列建模**。CF_Update 为最低优先级、纯 FIFO 排在 ACK/NACK 之后(UEC Table 5-12)——被 ACK 流积压是**预期规格行为**(§12.26 解耦后 ACK 不再独占 pacing FIFO,CF 响应已恢复,但 400B 门对 CF_Update 的线速上限仍按规格成立)。
- **CtlOS 发送速率控制(pacing 门)**:`SetCtlOsMinInterval(Time)` 直接设最短间隔,或 `SetCtlOsTargetSpacing(bytes)` 按 `GetDataRate().CalculateBytesTxTime(bytes)` 折算。**配置路径上门固定为 UEC §5.2.11.2 的 400B floor**(`CTLOS_MIN_SPACING_BYTES` = "任意两条 CtlOS ≥400B"、CtlOS<2% 带宽的硬下限,**不再随 ACK 周期缩放**,§12.26)。超过速率生成的 CtlOS 入**节流队列**(FIFO)由定时器按间隔排空;首个 CtlOS 恒放行(不推迟 INIT)。**默认关闭**(interval=0,纯引擎/非配置 scratch),PFC/CC_Update 不走节流(PFC 阈值触发需即时,CC_Update 已由周期定时器控制节奏)。100G 下 400B 门远不绑定(CtlOS 需求 ~6.1M/s vs 门容量 ~31.2M/s,比例与速率无关),不是任何速率瓶颈(§12.25 为 1G 历史实测)。
- **已知近似**:真机 CtlOS 可在帧内(首 256B 后、间隔 ≥2KB)插入;本期**帧间插入**(帧粒度)。聚合带宽正确;单帧延迟略有近似。后续如需可细化为子帧插入。
- **端口速率预置(100G/200G/400G;ScaleOut 最小端口速率 100G,§12.36;THU-SPEC 三速率对齐 §12.37)**:随仓库交付 `conf/sue-default|200g|400g.conf`(默认档 = 100G,与 `topo` 的 `link` 行 rate 字段配对)。速率耦合键(`LLR_CTLOS_TARGET_SPACING`/`CBFC_CF_THRESHOLD|MIN|MAX_TIMER_BYTES`/`SUE_IFG_BYTES`)都是**线字节**阈值 → 控制开销比恒定、与速率无关,三个预置数值一致(唯一例外:400G 档 ACK 间距 2050B,NS 网格对齐 THU 实测,§12.37),仅折算出的时间间隔随速率变化:CtlOS/ACK 间隔 2048B = 163.84ns@100G / 81.92ns@200G(400G 档 2050B = 41.0ns);CF 阈值 2075B = 166.0ns@100G / 83.0ns@200G / 41.5ns@400G(THU 默认档 6.02M/12.05M/24.10M/s 对齐);IFG 与 CtlOS 线时在 100G+ 均 <1ns → 需 PS 分辨率(入口自动升)。400G 下 ACK 事件率 ≈2439 万/秒/链路,对事件率敏感时提 `LLR_CTLOS_TARGET_SPACING` 至 UEC 上限 16384B(327ns)或评估时间键方案。预置各速率把 `LLR_REPLAY_TIMER_MAX_NS`/`LLR_INIT_TIMER_MAX_NS` 按**计算后的推荐值**直接写入:`sue-default.conf` 21830ns@100G、`sue-200g|400g.conf` 各 20915/20458ns(公式 = 10× LLR RTT = `2×wire + 2×prop + ackPeriod`,随链路速率缩放,§12.25/§12.27,各 conf 键注释注明公式与原理);亦可填 **0=自动折算**(效果等同)。**replay_timer 配置必须与 RTT 关联**:固定值 ≈ ACK RTT 时无谓重传成倍占带宽(1G 历史实证,§12.25);固定值 < RTT 时每周期无谓重传 → replay_ct 耗尽 FLUSH 级联(1G 默认配置 s6-entry 回归,§12.27)。完整对照表见 config-guide §2.7。

### 5.8 四种帧组合与 VC 来源
| 变体 | VLAN | shim | VC 来源 |
|---|---|---|---|
| A | 无 | 无 | **VC0**(无法映射,应避免;使用上需 with VLAN) |
| B | 有 | 无 | PCP->VC |
| C | 无 | 有 | DSCP->VC |
| D | 有 | 有 | DSCP->VC(shim 优先;VLAN 透传) |

> 不带 shim 时**使用上需保证 with VLAN**,否则无法映射 VC、全部走 VC0。
> shim 的有无由**逐 EtherType 配置**决定(§5.2):测试中 C/D 用默认 `SUE_ETH_TYPE_1`(HAS_SHIM=1)建帧,A/B 用 `SUE_ETH_TYPE_0`(HAS_SHIM=0)。若某部署两个 EtherType 都配 HAS_SHIM=1,则该部署不存在无 shim 的 SUE 帧(VC 来源只剩 DSCP),此表 A/B 不再适用。
> **映射实现(§7)**:VC 来源为**表驱动**——`SueConfig` 的 `DSCP_TO_VC_MAP`/`PCP_TO_VC_MAP`/`DEFAULT_VC`(默认 = 本表语义:直映 `dscp&0x07`/PCP 恒等/VC0)。host 发送路径、host 接收归因与交换机 `ComputeVc` **从同一张表**解出同一 VC,端到端一致。

### 5.9 前向兼容
- `SueHeader` 保留全部原始字段(AR/C/CNM/ECN)供后续读取。
- EtherType 分发表预留 AFH Gen2 / 标准以太网入口。
- shim 字段位宽/偏移可配(默认 §5.3),为非标布局留余地。
- LLR/CBFC/PFC 引擎与上层(AFH)解耦:上层只见"可靠无丢包链路",后续 RH/CC 可叠加不改 L2。

### 5.10 PFC 帧格式(IEEE 802.1Qbb,评审补)
`SuePfcHeader`(`sue-pfc-header`)独立实现标准 PFC pause 帧(**仅取语义,不依赖 `PauseHeader` 类**):
```
MACDA=01-80-C2-00-00-01(慢协议多播) | MACSA | EtherType=0x8808 | Opcode=0x0101
| 2B priority_enable_vector + 2B×8 pause_time_quanta(每优先级 0~65535 quanta,1quanta=512bit-time)
```
**已实现**:整帧 **64B** 上线——34B 体(MACDA 6 + MACSA 6 + EtherType 2 + Opcode 2 + PEV 2 + quanta 16)+ 26B 补零至以太网最小 60B + 4B CRC-32 FCS(对前 60B)。**不挂 SueCtlHeader**;RX 经 `SueCtlTag` 分发后按 802.1Qbb 语义解码(PEV 位置位时 `quanta>0 → OnPause(vc)`,`quanta=0 → OnResume(vc)`)。PFC 帧不消耗 CBFC 信用、LLR-ineligible(tag 分发先于 LLR)。
- pause/resume:某优先级 `quanta>0` 表示 pause(剩余时长),`quanta=0` 表示 resume;发送端 pause 帧默认 quanta=0xFFFF(暂停至显式 resume,匹配引擎显式 resume 语义),可配 `PFC_PAUSE_QUANTA`。
- 8 优先级一一对应 8 VC;`SuePfcEngine` 据此置位/清零 per-VC pause 状态门控出队。
- 与 CBFC 二选一(`FC_MODE`);PFC 帧本身 LLR-ineligible、走 BE VC、不消耗 CBFC 信用(若 CBFC 与 PFC 同存,按 §5.2.9 规则 5/6)。

---

## 6. 模块详细设计

### 6.1 SueNetDevice(核心 L2 设备,自研 TX/RX 管线)
> **关键约束(已查源码)**:`PointToPointNetDevice::Receive/TransmitStart/TransmitComplete` 均**非虚**,基类无 `DequeueAndTransmit`;`PointToPointChannel::TransmitStart` 经函数指针**静态分派到 `&PointToPointNetDevice::Receive`**(point-to-point-channel.cc:101),子类覆写不会被调用。fork 的 `QbbNetDevice` 解法是自研 TX/RX 管线 + 专用 `QbbChannel` 静态分派到 `&QbbNetDevice::Receive`(qbb-channel.cc:106)。本设计照此:自研管线 + `SueChannel`(§6.13)。
```cpp
class SueNetDevice : public PointToPointNetDevice {  // 继承上游基类,自研管线(同 QbbNetDevice 套路)
public:
  static const uint32_t qCnt = 8;
  static TypeId GetTypeId(void);
  bool Attach(Ptr<SueChannel> ch);             // 注册到 SueChannel(非基类 Attach)
  bool SwitchSend(uint32_t qIndex, Ptr<Packet> p);   // egress 入队(上层调用;VC 由调用方 ComputeVc 后传 qIndex)
  // NetDevice::Send 未自覆写:继承 PointToPointNetDevice::Send(自研管线不走它,留作 NetDevice 接口合规)
  void SetReceiveUpcall(Callback<void, Ptr<Packet>, SueHeader&> cb);
  void SetLlrEngine(Ptr<SueLlrEngine>); void SetFcController(Ptr<SueFcController>);
  void SetEgressQueue(Ptr<SueEgressQueue>); void SetErrorModel(Ptr<SueErrorModel>);
  // 计数器为 SueLlrCounters/SueCbfcCounters struct(引擎内建,无聚合类,§6.11);
  // 设备层仅 PrintCounters(FILE*, Time) 统一落盘。
  // 以下均为自研(基类同名方法非虚,此处隐藏而非 override;由 SueChannel 静态分派调用 Receive)
  void Receive(Ptr<Packet> p);                 // LLR RX -> FC RX -> 上送
  void DequeueAndTransmit(void);               // 自研出队+发送(基类无此方法)
  void TransmitComplete(void);                 // 自研发送完成(含 IFG,见 §5.4)
  bool TransmitStart(Ptr<Packet> p);           // 自研发送启动
private:
  Ptr<SueLlrEngine> m_llr; Ptr<SueFcController> m_fc;  // CBFC 或 PFC(FC 门面)
  Ptr<SueEgressQueue> m_queue; Ptr<SueErrorModel> m_err;
  Ptr<SueChannel> m_channel; bool m_rateSet;
  Time m_ifg;                                  // IFG 自管(基类 m_tInterframeGap 私有且其 TransmitComplete 非虚不运行)
  bool m_llrEnabled, m_cbfcEnabled, m_pfcEnabled;
};
```
- `Receive`: 先查 `SueCtlTag`(metadata)-> 有则 `HandleCtlFrame(p, tagType)` 控制帧分发并返回;**无 preamble/不消耗 CBFC 信用**。否则**先剥 8B preamble**(有 `m_llr` 时一律剥,再按 `mode_local` 门控——ON→LLR RX 过滤 / OFF→直通不过滤;§5.4)-> LLR RX -> FC RX(CBFC 计数 或 PFC pause 处理)-> 上送。
- `DequeueAndTransmit`: 选 VC(固定 1:1 RR,§6.7)-> FC 门控(信用 或 未 pause)-> LLR TX(装 preamble)-> TransmitStart。
- `TransmitComplete`: 发送完成 -> 优先出控制队列 -> 加 `m_ifg` -> 触发下一帧 DequeueAndTransmit。
- **控制帧发送**:`SendCtlFrame(type, payload)` 为 **CtlOS 短报文**路径(LLR ACK/NACK/INIT/INIT_ECHO、CF_Update):挂 1B `SueCtlHeader` + `SueCtlTag`,经可配**节流门**(`m_ctlOsMinInterval`/`m_ctlOsPending`/`m_ctlOsEvent`,见 §5.7)后 `TransmitControl`。**PFC/CC_Update 不走此路径**:`SendCtlPause`/`SendCtlCcUpdate` 组**完整 64B 以太网帧**(§5.10/§5.6,不挂 SueCtlHeader)直接 `TransmitControl`。`TransmitControl` = 忙则入高优先 `m_ctlQueue`,否则 `TransmitStart`。
- 控制帧走独立控制队列,绕过 FC 信用/普通出队。
- `Attach(Ptr<SueChannel>)` 隐藏基类非虚 `Attach(Ptr<PointToPointChannel>)`;自定义 Attach 须自行完成通道注册(本实现内含 `m_channel->Attach(this)`),不依赖基类 Attach,避免实现踩坑。
- **多线程适配(§10.5, S5a)**:覆写 `GetChannel()` 返回子类 `m_channel`(基类非虚返回基类私有成员、Attach 不设置——MTP 分区器靠它发现链路/计算 lookahead,根因 bug 见 §10.5);定时器 arm 统一走模板 `ScheduleSelf(delay, fn, ...)`(透传普通 `Schedule`;路由靠 LP 驻留,§10.5)(7 处:OnCtlOsPacingTimer、OnCcUpdateTimer、OnDrain、TransmitComplete、IFG DequeueAndTransmit)。`SetLlrEngine` 注入 `llr->SetContext(GetNode()->GetId())`。

### 6.2 SueLlrEngine(LLR,UEC §5.1)
```cpp
class SueLlrEngine : public Object {
public:
  void Start();                                     // 发起 INIT 握手(发 INIT + 调度 init 定时器);亦是 re_init=FALSE 时的管理重入入口(§6.10)
  void SetContext(uint32_t nodeId); uint32_t GetContext() const;  // §10.5 节点上下文路由:定时器 arm 带节点 context(ScheduleCtx)
  void StartOnLp();                                 // §10.5 MTP:Start() 在公共 LP 被调时 kick t==0 节点 context 事件,Partition re-home 后在此执行 StartInit
  bool TxStore(Ptr<Packet> p);                     // 分配 seq, 存 replay, 装 preamble
  LlrRxResult RxFrame(uint32_t seq, int fcsStatus); // fcsStatus: 0=GOOD 1=POISONED 2=BAD
  void OnAckNack(uint32_t seq, bool isNack);
  void OnReplayTimer();
  void OnInit(uint32_t seq); void OnInitEcho(uint32_t seq);   // 收 INIT / 收 INIT_ECHO
  void FlushAndInit();                             // FLUSH 两路径统一:FLUSH->取消定时器->(re_init_on_discard 时)StartInit(等 INIT_ECHO)
  void SetReplayTimerMax(Time); void SetReplayCtMax(uint32_t);
  void SetInitTimerMax(Time); void SetInitRetryMax(uint32_t);  // 独立 INIT 定时器(§6.2 保真度)
  void SetDataAgeTimerMax(Time);                   // 独立 buffer 驱动 data_age 定时器
  void SetAckPeriod(Time);                         // UEC §5.1.6 ACK 周期:0=每收好帧立即 ACK;>0=发过首个 INIT_ECHO(init_echo_sent=SEND_ACKS 触发源)后每周期一条累积 ACK(空闲也发,发送端 dup 跳过)
  void SetPcsLostTimerMax(Time);                   // UEC Table 5-10 pcs_lost_status_timer_max(0=立即过期);PCS_status FALSE 持续超过 -> FLUSH(§6.10)
  void SetPcsStatus(bool); bool GetPcsStatus();    // 最小 PCS 模型:场景驱动 bool,无 PCS 子层(设备在 PCS down 时丢入帧)
  void SetOutstandingSeqMax(uint32_t); void SetOutstandingDataMax(uint32_t);
  void SetReplayBufMax(uint32_t n);                 // 存储容量(帧数,LLR_REPLAY_BUF_MAX,§6.10)
  void SetModeLocal(bool); void SetModeRemote(bool);  // UEC §5.1.4 两相使能:local=RX 可收 LLR_INIT/过滤,remote=TX 可发 LLR_INIT/分配 seq
  void SetInitBehavior(std::string); void SetFlushBehavior(std::string);  // DISCARD|BLOCK|BEST_EFFORT(非法回退 BLOCK)
  void SetReInitOnDiscard(bool);                   // TRUE=FLUSH 自动重入 INIT;FALSE=停留 FLUSH 等管理 Start()
  TxBehavior GetTxBehavior() const;                // 当前状态对 LLR-desired 帧的处理风格(§5.1.4)
  enum class TxState { OFF, INIT, ADVANCE, REPLAY, FLUSH };
  enum class TxBehavior { BLOCK, DISCARD, BEST_EFFORT, LLR_ACTIVE };
private:
  void GoAdvance(); void StartInit(); void OnInitTimeout();
  void OnDataAge();                                 // data_age 定时器回调
  void OnAckTimer();                                // UEC §5.1.6 ACK 周期定时器回调(自转,累积 ACK)
  void ArmPcsLostTimer(); void OnPcsLostTimeout();  // UEC Table 5-10 pcs_lost_status_timer(见下)
  TxState m_tx; uint32_t m_txSeq, m_ackedSeq, m_nextRxSeq;   // m_ackedSeq = UEC Table 5-10 acked_seq(见下)
  bool m_nextRxSeqVld;                              // UEC §5.1.7:收有效 INIT(local ON)置 TRUE;FLUSH 清 FALSE
  bool m_nackSent;                                  // NACK_SENT:bool 简化(非枚举状态机);置位期间不发 ACK/NACK
  Time m_ackPeriod; EventId m_ackTimer;             // UEC §5.1.6:0=每帧 ACK(历史);>0=init_echo_sent 后自转的累积 ACK 周期
  bool m_modeLocal{true}, m_modeRemote{true};       // 两相使能默认都开 = 原 LLR_ENABLE=1(§5.1.4)
  TxBehavior m_initBehavior{BLOCK}, m_flushBehavior{BLOCK};  // 默认 BLOCK=取帧排队、无丢包(§5.1.4)
  bool m_reInitOnDiscard{true};                     // 默认自动重入 INIT(§5.1.4)
  std::deque<ReplayEntry> m_replayBuf;   // FIFO
  uint32_t m_replayBufMax{1u<<20};                 // 存储容量(帧数,LLR_REPLAY_BUF_MAX;与窗/字节上限独立,§6.10)
  uint32_t m_replayCt, m_replayCtMax;               // replay_ct_is_max = replay_ct>=max;max=255(全 1)永不触发
  Time m_initTimerMax; EventId m_initTimer;   // 独立 INIT 超时定时器
  EventId m_dataAgeTimer;                     // 独立 buffer 驱动 data_age 定时器
  bool m_pcsStatus{true};                     // 最小 PCS 模型:场景驱动 bool(无 PCS 子层)
  Time m_pcsLostTimerMax{Seconds(0)};         // UEC Table 5-10 pcs_lost_status_timer_max(0=立即过期)
  EventId m_pcsLostTimer; bool m_pcsLostTimerExpired{false};   // 过期锁存,直至 PCS 恢复
  // 参数(§9)
};
```
- TX 状态机 `OFF->INIT->ADVANCE↔REPLAY->FLUSH`(§5.1.5)。
- ACK/NACK 机为 **`bool m_nackSent` 简化**(UEC 的 `OFF/SEND_ACKS/SEND_NACK/NACK_SENT` 枚举状态机未按名落地;NACK 仍立即单发;ACK 依 `m_ackPeriod` 两路——0=每收好帧立即发(历史),>0=**以首个 `LLR_INIT_ECHO` 的发出(init_echo_sent)为进入 SEND_ACKS 的触发源**(§5.1.6),`OnInit` 时 arm 一次、`OnAckTimer` 自转,每周期发一条**累积 ACK**(`next_rx_seq-1`),空闲周期重复末 seq(发送端 dup 跳过),`m_nackSent` 置位期间抑制);全部经 `m_ackNackSink` 发出。**推论(时序影响)**:`ackPeriod>0` 的 LLR 链路上,进入 SEND_ACKS 后定时器**自转不停**——空闲链路不再自然静默,凡跑配置路径(`ApplySueConfig` 设 `LLR_CTLOS_TARGET_SPACING`)的仿真程序须显式 `Simulator::Stop`(s6-entry 已加;纯引擎/非配置 scratch 用 `ackPeriod=0` 走 per-frame 不受影响)。
- **next_rx_seq_vld(UEC §5.1.7)**:`OnInit`(有效 INIT,local ON)置 TRUE;`FlushAndInit` 清 FALSE。`RxFrame` 在 mode_local 检查后 `if (!m_nextRxSeqVld) return DROP`——**不计数、不 NACK**(filtered,非 expected/duplicate/missing)。
- **INIT 握手(已实现端到端,标准入口现跑真握手)**:对端有引擎时收 `LLR_INIT` → `OnInit`(重置 next_rx_seq/nackSent、`m_nextRxSeqVld=TRUE`,`++rx_init_ctl_os`)→ 回 `LLR_INIT_ECHO`(`++tx_init_echo_ctl_os`);发送端收 `INIT_ECHO` → `OnInitEcho`(`rx_init_echo_ctl_os` 照常 +1;仅 `init_seq==tx_seq` 才取消 init 定时器 + `GoAdvance`——UEC §5.1.9.1.2,否则忽略 + 告警日志)→ `GoAdvance`。对端无引擎时 INIT 超时×`m_initRetryMax`(默认 3)后告警 `GoAdvance` 兜底。**入口时序(§6.8)**:`MakeDeviceOn` 先 wire 引擎、`ApplySueConfig` 落 config 后才 `Start()`——故 `LLR_MODE_REMOTE 0` 真抑制 INIT、对端已存在,标准流 INIT 握手真实执行(不再走 `StartInit` sink 未接的 `GoAdvance` 兜底)。
- ACK/NACK 校验公式:`ok = ((tx_seq-1-seq) mod 2^20 ≤ 2^19) && ((seq-acked_seq) mod 2^20 < 2^19)`。
- replay buffer: FIFO,ACK 释放头部连续段。`CanTx()` 三门限**独立 AND**(均门控同一 FIFO):在途帧窗 `m_outstandingSeqMax` / 在途字节 `m_outstandingDataMax` / 存储容量 `m_replayBufMax`——窗=在途帧数、字节=在途字节(BDP,SHOULD 值)、存储=芯片可缓存帧数,三者不互通(§6.10 选型)。
- **acked_seq(UEC Table 5-10)**:20 位寄存器 = 最近一次成功接收的 LLR_ACK/LLR_NACK 中确认的序列号(接收路径 `m_ackedSeq=ack_nack_seq` 已实现)。初始化三处对齐 UEC:**初值全 1**(`m_ackedSeq{SEQ_MOD-1}`,幂等"尚无 ACK");`SetModeRemote` 在 llr_mode_remote **改变时**重置全 1(且 `m_txSeq=0`);`StartInit`/`OnInitTimeout` 每次发 LLR_INIT 置 `init_seq-1`(init_seq = tx_seq % 2^20)。全 1 / init_seq-1 初值根治"首帧 seq=0 与 ackedSeq=0 碰撞":首 ACK(0) 不再被误判重复,真重复 ACK(`seq==ackedSeq` 且非 NACK)仍跳过——`m_anyAcked` 已删除。**outstanding_seq(UEC Table 5-10)= `((tx_seq-1) - acked_seq) mod 2^20`(序列空间量,非缓冲长度)**:由于 mode_remote 改变时 tx_seq 清零 + acked_seq 置全 1 + 缓冲清空,该公式与 replay buffer 大小在**所有**状态下一致(§12.18 复核:早前 §12.16 的"改按 buffer 长度"作废,回退 UEC 原式)。
- **LLR 保真度(全改)**:① FLUSH **三**条路径(数据超龄 / replay_ct 超限 / pcs_lost_status_timer 超时)统一走 `FlushAndInit()`:`m_tx=FLUSH`、取消 replay/data_age 定时器、`StartInit()` 发 INIT 并进入 INIT 状态**等待 INIT_ECHO**(不再发 INIT 即回 ADVANCE);对端无引擎时经 INIT 超时×重试后 `GoAdvance()`(兜底不变)。② **独立 INIT 超时定时器** `m_initTimerMax`(引擎默认 5000ns floor,`SetInitTimerMax` 可配;配置路径经 `LLR_INIT_TIMER_MAX_NS 0`=自动折算,§12.27),不再复用 `m_replayTimerMax`;重试上限 `LLR_INIT_RETRY_MAX`。③ **独立 buffer 驱动的 data_age 定时器** `m_dataAgeTimer`(replay 缓冲由空→非空时调度一次,覆盖整个突发;`GoAdvance`/`FlushAndInit` 取消);到期按**实际最老帧年龄**重排(`buffer.front().ts`),超龄才 FLUSH。`DoReplay` 内嵌的 data-age 检查保留为兜底(replay 定时器驱动时也能触发;先触发者清空 buffer,后触者见空 no-op)。④ **UEC §5.1.4 两相使能 + 三行为**(§12.6):`m_modeLocal` 门控 RX(`OnInit` 不回 INIT_ECHO/不重同步、`RxFrame` 不过滤直接 ACCEPT;设备 `Receive` 仍**先剥 8B preamble** 再按 mode_local 门控,§5.4);`m_modeRemote` 门控 TX(`StartInit` 不发 INIT 直接 GoAdvance、`GetTxBehavior` 返 BEST_EFFORT)。`GetTxBehavior()` 按当前态返回:`OFF→BEST_EFFORT`、`INIT→m_initBehavior`、`FLUSH→m_flushBehavior`、`ADVANCE/REPLAY→LLR_ACTIVE`(eligible);设备据此 BLOCK(帧留队列)/DISCARD(取走丢弃计 tx_discard)/BEST_EFFORT(直发不 seq 不入 replay)。`FlushAndInit` 在 `m_reInitOnDiscard=false` 时**停留 FLUSH**,管理方 `Start()` 重入 INIT。默认(两相开 + BLOCK + 自动重入)精确复刻原单键 LLR_ENABLE=1 行为。⑤ **tx_seq 清零 + LLR_status + pcs_lost_status_timer(§12.18)**:`SetModeRemote` 在 llr_mode_remote 改变时 `m_txSeq=0`(UEC Table 5-10);`GetLlrStatus()` = mode_remote ON 且 ADVANCE/REPLAY(mode_remote OFF 时跟踪 PCS_status,直通)。**最小 PCS 模型**(无 PCS 子层):设备 `SetPcsStatus(bool)` 由场景驱动,`m_pcsStatus=FALSE` 时设备**丢弃入帧**(闪断期间不可解码 → 对端 replay 缓冲增长),引擎 `pcs_lost_status_timer` 在 ADVANCE/REPLAY 且 PCS FALSE 时运行、OFF/INIT/FLUSH 停止,超时(即使缓冲为空)FLUSH 重同步;过期标志 `m_pcsLostTimerExpired` 锁存至 PCS 恢复。`pcs_lost_status_timer_max=0` = 立即过期(UEC)。⑥ **replay_ct 语义(UEC Table 5-10,§12.18)**:`replay_ct_is_max = replay_ct >= replay_ct_max`;`replay_ct_max` 为全 1(255)时**永不触发** FLUSH(仅 data_age / pcs_lost 可触发)。
- 异常处理见 §6.10。
- **节点上下文路由(§10.5, S5a)**:设备 `SetLlrEngine` 时注入 `llr->SetContext(GetNode() ? GetNode()->GetId() : Simulator::NO_CONTEXT)`;全部定时器 arm(init / replay / data_age / ack / pcs_lost,9 处)统一走私有模板 `ScheduleCtx(delay, fn, ...)`——该模板现**透传普通 `Simulator::Schedule`**,路由靠 **LP 驻留**:所有 arm 点都在节点 LP 的线程内执行(数据平面经 §6.13 channel 接缝落节点 LP;定时器回调自续转时也已在节点 LP),普通 Schedule 即落入该 LP 的事件堆、`Cancel` 同 LP 有效。模板把"定时器一律按节点 context 语义 arm"集中成唯一通道,后续如需显式逐 arm 路由,只需在模板内改 `ScheduleWithContext(m_context, ...)` 一处、不动 9 个调用点;`m_context` 目前仅用于 `Start()` 的启动 kick。`Start()` 在 MTP 下不直接 `StartInit`(那会把 INIT 与 init 定时器 arm 在公共 LP),而是 kick `t==0` 的 `ScheduleWithContext(m_context, 0, StartOnLp)` 事件——`Partition()` 在任一线程启动前 `InvokeNow` re-home 到节点 LP,`StartOnLp` 的 `StartInit` 定时器 arm 落进该 LP 的事件堆。非 MTP 下 `DefaultSimulatorImpl` 忽略 context(= `Schedule`),单线程基线逐位不变(37 TestCase + 23 scratch 全 PASS)。

### 6.3 SueCbfcEngine(CBFC,UEC §5.2)
```cpp
class SueCbfcEngine : public Object {
public:
  bool CanSend(uint32_t vc, uint32_t macFrameBytes);  // 信用检查(用 MAC 帧字节数,扣除 preamble)
  void OnTx(uint32_t vc, uint32_t macFrameBytes);     // S_VC_CC += pktCredits
  void OnCfUpdate(uint32_t vc, uint32_t rVcCf);
  void GenerateCcUpdate(); void GenerateCfUpdate();
  void OnRx(uint32_t vc, uint32_t macFrameBytes);     // R_VC_CC +=
  void OnDrain(uint32_t vc, uint32_t macFrameBytes);   // R_VC_CF +=
  void OnCcUpdate(std::array<uint32_t,16> cc);
  void SuspendForLlrWindow(bool on);             // LLR 共存钩子(§5.2.9)
private:
  static const uint32_t NVC = 8;
  uint32_t S_VC_CC[NVC], S_VC_CF[NVC], S_VC_CL[NVC], S_VC_CU[NVC];
  uint32_t R_VC_CC[NVC], R_VC_CF[NVC];
  bool m_inLlrWindow;
  // 参数(§9)
};
```
- `pktCredits = roundup((macFrameBytes + PktOvhd)/CreditSize)`;**`macFrameBytes` 不含 preamble**(见 §5.4 信用一致性);门控 `S_VC_CU[x]+pktCredits ≤ S_VC_CL[x]`。
- CF_Update 生成:`R_VC_CF 更新 且 CF_min 到期` 或 `CF_max 到期`。
- CC_Update 周期发送(8 VC 单帧)。
- **LLR 共存九规则**(§5.2.9):replay 窗口不更新 S_VC_CC;丢弃窗口不更新 R_VC_CC;窗口内 CC_Update 丢弃但 CF_Update 仍更新 S_VC_CF;PFC 帧为 LLR-ineligible+BE VC 等。

  九规则实现状态(§5.2.9):
  | 规则 | 状态 | 说明 |
  |---|---|---|
  | 1 replay 窗口不更新 S_VC_CC | 结构满足 | 重传路径不调 OnTx;suspend 钩子为 no-op |
  | 2 丢弃窗口不更新 R_VC_CC | 结构满足 | DROP 路径 return 前不调 OnRx/OnDrain |
  | 3 丢弃窗口内 CC_Update 丢弃 | 已实现 | OnCcUpdate 顶部 `if (m_inLlrWindow) return;`(窗口=引擎状态驱动,§12.31 B2) |
  | 4 CF_Update 仍更新 S_VC_CF | 已实现 | OnCfUpdate/OnDrain 不受窗口影响 |
  | 5 PFC 帧 LLR-ineligible | 已实现 | 真实 64B 帧上线,tag 分发先于 LLR,无 preamble(§5.10) |
  | 6 PFC 帧 BE VC | 已实现 | PFC 帧不消耗 CBFC 信用;PFC 与 CBFC 二选一(§5.10) |
  | 7 PFC 收到时仍动作 | 已实现 | HandlePfc 不受 LLR 窗口影响 |
  | 8 CC_Update 默认 LLR-ineligible+BE | 已实现 | 真实 64B 帧上线,tag 分发先于 LLR/CBFC(§5.6) |
  | 9 replay 窗口内不发新 CC_Update | 已实现 | SendCtlCcUpdate 顶部 `if (GetInLlrWindow()) return;`(周期 OnCcUpdateTimer 同路径) |
  > **"in LLR window"唯一事实源**:`SueCbfcEngine::m_inLlrWindow`(规则 3/9 均查它)。2026-08-13 评审修正(§12.31 B2):旗标由 **LLR 引擎状态驱动**(`RecomputeLlrWindow`:`(m_tx==REPLAY) || m_nackSent`,覆盖真实的 replay 突发与丢弃窗口——跨多个事件的区间,而非仅重传 `TransmitStart` 的同步调用区间);设备经 `SetLlrWindowCb` 转发到 CBFC 引擎。类型 B(LLR-ineligible + CBFC lossless)帧在窗口内**仍计** R_VC_CC(UEC Table 5-26):OnTx/OnRx 不设窗口门(规则 1/2 由结构保证:重传不调 OnTx、LLR 丢弃帧早返回)。
- 初始化:默认配置直配两端参数(`LLDP_NEGOTIATE=0`);`LLDP_NEGOTIATE=1` 时经 `SueNetDevice::StartLldpExchange/OnRemoteLldpTlv` 走 UEC §5.3 能力交换 + §5.2.8.4 RTS/RTR VC 初始化握手(§12.32)。

### 6.4 SuePfcEngine / SueMmu 的 PFC 阈值算法(已查源码)
现有 `SwitchMmu` **已含完整 PFC 阈值算法**:
- `GetPfcThreshold(port) = (buffer_size - total_hdrm - total_rsrv - shared_used_bytes) >> pfc_a_shift[port]` -- 动态阈值(`pfc_a_shift`=alpha 移位)。
- `CheckShouldPause` / `CheckShouldResume`(`!paused && (hdrm>0 || shared≥thresh)` / `hdrm==0 && shared+resume_offset≤thresh`)。
- ingress/egress admission、headroom(`PFC` 队头预留)、resume_offset、shared pool。

-> 决策:**将该算法移植(port)到 `SuePfcEngine`**(复制逻辑,不依赖 `SwitchMmu` 类,不修改其源码)。`SuePfcEngine` 实现 8 优先级 pause/resume + 共享池动态阈值(§6.7 实现注:无独立 `SueMmu` 类),经 `SueFcController` 与 CBFC 二选一(`FC_MODE`),用于 PFC vs CBFC 对比。
  > **PFC 门控生效条件**:`SueNetDevice` 的 `drainDelay` 须 > 0 才能体现 pause 门控(drainDelay==0 时 OnRx 后立即 OnDrainPfc,缓冲不累积,pause 永不触发)。这是仿真建模约束(真机缓冲自然累积)。
  > **实现近似声明(2026-08-13 评审 + PFC-SP-03)**:动态阈值取 `(shared_pool - shared_used) >> alpha + headroom_used`,pause 条件含 SwitchMmu 的 `hdrm>0` 守卫,resume 线 = `th - resume_offset`(迟滞带,原 `PFC_RESUME_OFFSET` 死键已接线);未复刻 SwitchMmu 的 `buffer_size - total_hdrm - total_rsrv` 完整公式——**启发式近似**,阈值语义与基线不可逐字节对比,对比结论以此为准。

### 6.5 SueFcController(流控门面)
```cpp
class SueFcController : public Object {
public:
  enum Mode { OFF, CBFC, PFC };
  void SetCbfc(Ptr<SueCbfcEngine>); void SetPfc(Ptr<SuePfcEngine>);  // 顺带置 Mode
  void SetMode(Mode); Mode GetMode();
  Ptr<SueCbfcEngine> GetCbfc(); Ptr<SuePfcEngine> GetPfc();
  bool CanSend(uint32_t vc, uint32_t bytes);     // 派发到 CBFC 或 PFC
  void OnTx/OnRx/OnDrain(...);                   // 派发
  uint32_t GetRvcCf(uint32_t vc);                // CBFC 专用(设备 OnDrain 用)
  bool IsPaused(uint32_t vc);                    // PFC 专用
  void SuspendForLlrWindow(bool on);             // CBFC 专用
};
```
- **已接入设备数据路径**:`SueNetDevice` 的 Receive/OnDrain/DequeueAndTransmit/Retransmit/SuspendForLlrWindow 统一走 `m_fc` 派发(`m_fc==nullptr` 时保持 legacy 逐引擎路径,字节级不变)。
- 门面 `OnRx` 的 PFC 分支**内含 `CheckThresholds()`**(与 legacy 内联路径一致,否则门面 PFC 永不发 pause);`OnDrain` 统一处理(门面/CBFC/legacy PFC 经 `OnDrainPfc`)。
- `SueEgressQueue` 提供 `GetNextSendableQindex(Ptr<SueFcController>)` 重载:OFF→`GetNextQindex`,CBFC→`GetNextSendableQindex(cbfc)`,PFC→`GetNextSendableQindexPfc(pfc)`。
- 配置 `FC_MODE` 选择;`ApplySueConfig` 据此建门面(见 §9)。

### 6.6 SUESwitchNode(AFH 转发)
```cpp
class SUESwitchNode : public Node {   // 仿 NVSwitchNode 结构,独立实现
public:
  static const uint32_t qCnt = 8;
  static const uint32_t pCnt = 64;    // 端口 radix(可配)
  static TypeId GetTypeId(void);
  uint32_t AddPort(Ptr<SueNetDevice> dev);      // 聚合端口 + 设 receive upcall
  Ptr<SueNetDevice> GetPort(uint32_t i) const;  // GetNPorts()
  void SetRoutingTable(Ptr<SueRoutingTable> rt); Ptr<SueRoutingTable> GetRoutingTable() const;
  void SetEcmpSeed(uint32_t s);
  void SwitchReceiveFromDevice(Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h); // 值传递(解析已在设备侧完成)
private:
  int32_t GetOutPort(uint32_t xpuId) const;     // LPM + ECMP hash(无 AR)
  void SendToDev(uint32_t portIdx, uint8_t vc, Ptr<Packet> p);
  uint8_t ComputeVc(const SueHeader &h) const;
  std::vector<Ptr<SueNetDevice>> m_ports; Ptr<SueRoutingTable> m_rt;
  // AFH 计数器为裸成员 m_rxPkts/m_txPkts/m_lookupMiss/m_ttlZeroDrop/m_vcRx[8]/m_vcTx[8]
  //  + getter(GetCntRxPkts/GetCntTtlZeroDrop/GetCntVcRx/.../ResetCounters),§6.11
};
```
转发流程:解析完的 `SueHeader` -> 提取 XPU-id(MAC DA 高 10 位)-> `SueRoutingTable::Lookup` -> ECMP hash(无 AR)-> VC 派生 -> shim TTL-- -> 入 egress VC 队列。

### 6.7 SueEgressQueue / SueMmu
- `SueEgressQueue`:8-VC 队列 + 三个重载 `GetNextSendableQindex(Ptr<SueCbfcEngine>)` / `GetNextSendableQindexPfc(Ptr<SuePfcEngine>)` / `GetNextSendableQindex(Ptr<SueFcController>)`(固定 1:1 RR + FC 门控;门面重载按 Mode 派发,§6.5)。
- **`SueMmu` 类未实现(实现注)**:per-VC 计账/共享池/动态阈值算法直接落在 `SuePfcEngine` 内(`EnableSharedPool(shared,headroom,resume,alpha)` + `OnRx` 后 `CheckThresholds` 的 pause/resume,§6.4/§9),无独立 `SueMmu` 类;CBFC 的缓存计账在 `SueCbfcEngine`(`OnRx`/`OnDrain` 维护 R_VC_CC/CF)。
- **CBFC 出口 per-VC 缓冲门 + 延迟退还 + 交换机挂起(实现注,§12.30)**:交换机 CBFC 端口在 `SUESwitchNode::ApplyCbfcBackpressure` 下配置为延迟退还(`SetDeferCreditRefund(true)`),逐 VC 设出口深度门(`SueEgressQueue::SetVcLimitBytes`,auto = `creditLimit[vc]×creditSize`,§9 `CBFC_EGRESS_BUF_BYTES`)。链路方向:`Forward` 准入查 `EgressCanAccept` → 可收则 `SwitchSend` + `ingress->RefundCredit`(释放入口信用);出口 VC 队列满则帧挂起在交换机 `m_pending[port][vc]`(信用保持消耗),出口 `DequeueAndTransmit` 释放槽位后经 freed-hook 触发 `ServicePending` FIFO 补发 + 退还入口信用(钩子在 `TransmitStart(p)` **之后**触发,m_busy=true → 无重入)。入口帧由此被出口拥塞真实节流(lookup-miss/TTL-0 丢弃路径必须显式退还入口信用,§12.30)。

### 6.8 入口分发 / 拓扑 / 路由
**已实现**(`network_frontend/sue-entry.{h,cc}`)。拓扑文件格式(`'#'` 注释/空行跳过,`KEY VALUE`):
```
type=SUE           # 段标记(只解析该段)
nodes <N>          # 总节点数(host+switch)
switch <id...>     # SUESwitchNode 的节点 id;其余 0..N-1 为 host
link <src> <dst> <rate> <llr_ena> <mode> <err_rate>
  rate    : ns-3 DataRate 串,如 "100Gbps"
  llr_ena : 0|1(两端建 LLR 引擎并使能)
  mode    : CBFC | PFC | OFF(每链路流控)
  err_rate: 写入 LINK_ERROR_RATE_PER_LINK GlobalValue(记录值,§12.5;确定性 corruptor 集合仍由调用方配,§10.1)
```
- **SetupSueNetwork(topoFile, confFile="")**:清空上一网络状态 → 解析 → 建 `Node`/`SUESwitchNode` → 逐链路建 `SueNetDevice`+`SueChannel`(host 设计数 receive upcall;switch `AddPort` 置 upcall)→ 每设备 `ApplySueConfig(dev)` 填充 GlobalValue 可调参数,再按链路行重断言 `SetLlrEnabled`(**拓扑为准,§12.6**) → BFS 安装 `SueRoutingTable`(XPU-id 10 位精确 LPM)。停止时间交给调用方。
- **假设:XPU-id = host 序号**(简化映射)。`SueGetHostXpuId(hostIdx)` 直接返回 host 下标(`hostIdx`),路由安装侧 `AddPrefix(hi, 10, …)` 同样用 host 下标 `hi` 作 10 位 XPU-id 主前缀(`sue-entry.cc`)。因此 XPU-id 由拓扑文件里 host 的声明顺序决定,**不是**可独立寻址的任意 ID;报文 MAC DA 高 10 位须与之对齐,否则查表 miss。后续如需任意 XPU-id 编址,需引入 host→id 映射表(范围外)。
- **SueEntryDispatch(topoFile, confFile, externalSetup=0)**:先 `SueReadConf(confFile)` → 读 `ENABLE_SCALEUP` GlobalValue:≠0 → `SetResolution(PS)`(若仍 NS,仅升级)+ `SetupSueNetwork`;==0 → `externalSetup()`(scale-out 路径保留自身 `SetupNetwork`)。
- **契约**:这是工作树内唯一 SUE 集成点;`reference/` 镜像(gitignored)保留自己的 `main1`/`SetupNetwork`,不修改。本次不实现混合拓扑 demo(范围外)。
- **BFS 注意**:设备通道必须经 `SueNetDevice::GetSueChannel()`(子类 `m_channel`)访问——基类 `GetChannel()` 返回基类自己的 `m_channel`(Attach 只设子类成员),否则路由表空装(AFH 全 miss)。

### 6.9 SueRoutingTable(LPM 路由)
```cpp
class SueRoutingTable : public Object {
public:
  void AddPrefix(uint32_t xpuId, uint8_t prefixLen, std::vector<uint32_t> ecmpPorts);
  bool Lookup(uint32_t xpuId, std::vector<uint32_t> &ports) const; // 最长前缀
  int32_t GetOutPort(uint32_t xpuId);                              // 便利:取首端口(-1=miss)
private:
  // 按 prefixLen 降序的 (prefix,len,ports[]) 列表;或 trie
};
```
- 查表 key = **XPU-id(10 位,自 MAC DA 高 10 位提取后直接作 uint32 key)**;调用方 `GetOutDev` 先解出 XPU-id 再查表。
- 单跳 scale-up 常见为 10 位精确匹配(XPU-id->port);LPM 支持聚合前缀(多平面/层级路由)。
- ECMP 组(多 plane)确定性 hash(无 AR)。**熵(§12.33)**:`GetOutPort(xpuId, flowEntropy)` 双参重载,`flowEntropy` = FNV-1a(MAC SA | MAC DA | shim FlowLabel[仅 F=1 时存在,§5.3])——交换机逐帧构造,流内亲和(同一流固定同一平面)、流间分散;单参重载 = 旧 XPU-id 直乘公式(entropy=0 逐位兼容)。
- **VRF 分段(§12.40)**:查表键 = (XPU-id, VRF)。VRF 维度在**交换机**层——`SUESwitchNode::SetRoutingTable(vrf, rt)` 每 VRF 一张普通 LPM 表(vrf0 = 旧 `SetRoutingTable(rt)`);`SueRoutingTable` 类本身不变。VRF 由入口三元组派生(§6.6),帧不携带 VRF。

### 6.10 LLR 异常处理
| 异常 | 触发 | 处理 | 计数器 |
|---|---|---|---|
| FCS BAD,期望帧 | 误码(`SueErrorModel`) | 不上送,发 NACK,等重传 | rx_expected_seq_bad, rx_bad |
| FCS BAD,非期望帧 | 同上 | 丢弃,发 NACK(期望 seq) | rx_bad, rx_missing_seq |
| FCS POISONED 期望帧 | 错误传播 | 接受(seq++),frame_bad 上报,不发 NACK | rx_expected_seq_poisoned, rx_poisoned |
| 重复帧 | seq < next_rx_seq | 丢弃 | rx_duplicate_seq |
| 序列空洞 | seq > next_rx_seq | 丢弃后续至期望,发 NACK | rx_missing_seq |
| replay_timer 超时 & replay_ct<max | 定时器 | REPLAY 重传 | tx_replay |
| replay_timer 超时 & replay_ct==max | 定时器 | FLUSH 丢弃,上报上层丢包 | tx_discard |
| replay_ct_max=255(全 1) | 配置(Table 5-10) | replay_ct_is_max 永不 TRUE,仅 data_age / pcs_lost 触发 FLUSH | — |
| data_age_timer 超时 | 定时器 | FLUSH | tx_discard |
| pcs_lost_status_timer 超时 | 定时器 | FLUSH(**即使 replay 缓冲为空**)+ INIT 重同步;timer 仅 ADVANCE/REPLAY 且 PCS_status FALSE 时运行(OFF/INIT/FLUSH 停止);max=0 立即过期(UEC) | tx_discard |
| outstanding_seq_max 达上限 | 派发 | 阻塞新 LLR-desired 帧(不分配 seq):在途窗 = `((tx_seq-1)-acked_seq) mod 2^20`(Table 5-10 序列空间量) | (内部状态) |
| outstanding_data_max 达上限 | 派发 | 阻塞:在途字节 ≥ 上限(UEC §5.1.6 **SHOULD = 链路 BDP**) | (内部状态) |
| replay buffer 满(存储容量) | 派发 | 阻塞:已缓存帧数 ≥ `LLR_REPLAY_BUF_MAX`,不得截断 eligible 帧(UEC L303 空间不足即不收) | (内部状态) |
| NACK 丢失 | 网络丢 | replay_timer 兜底重传 | tx_replay |
| ACK 丢失 | 网络丢 | replay_timer 重传,后续 ACK 释放 | tx_replay |
| INIT 握手超时 | 链路 up | 重发 LLR_INIT(重试上限后告警) | tx_init_ctl_os |
| 未收 INIT 前的 eligible 帧 | `next_rx_seq_vld=FALSE`(local ON,§5.1.7) | DROP,**不计数、不 NACK**(filtered,非 expected/duplicate/missing) | (无) |
| INIT_ECHO init_seq≠tx_seq | 错 seq 回执(§5.1.9.1.2) | 忽略 + 告警日志;仍计 rx_init_echo_ctl_os | rx_init_echo_ctl_os |
| llr_mode_local OFF | 配置(§5.1.4) | RX 仍先剥 8B preamble 但不过滤:收 INIT 不回 INIT_ECHO/不重同步;eligible 帧直通无 seq/NACK(§5.4) | (无) |
| llr_mode_remote OFF | 配置(§5.1.4) | TX 走 BEST_EFFORT:不发 INIT、不分配 seq、不入 replay、不 gating;帧仍带 ineligible preamble(8B,§5.4) | (无) |
| INIT/FLUSH 态 LLR-desired 帧 | 状态(§5.1.4) | 按 llr_init/flush_behavior:BLOCK=取帧排队(无丢包)/ DISCARD=取走丢弃 / BEST_EFFORT=直发不 seq | DISCARD→tx_discard |
| replay 失败(FLUSH) | 重试超限 | re_init_on_discard=TRUE 自动重入 INIT(发 INIT);FALSE 停留 FLUSH,管理方 `Start()` 重入 | tx_init_ctl_os |
| 状态机非法转移 | 内部 | 记录告警 + 计数,回 INIT | (内部告警) |
| TTL==0(AFH 层) | shim TTL | 丢弃 | afh_ttl_zero_drop |
| 路由查找miss | LPM miss | 丢弃 | afh_lookup_miss |

> 所有异常**必产生计数器增量 + 可选详细日志**(level 可配),便于验证与回归。

**缓冲选型(s7 仿真验证,`scratch/sue-llr-replay-buffer.cc`)**。周期 ACK 下帧在缓冲停留 ≈ `RTT + ACK 周期`,最小缓冲(UEC L63-66"持有数据不应远大于 BDP"):
>
> `B_min(A) = rate × (RTT + A)`  帧数 = `10000 × (RTT_eff + A) / 100`
>
> 100Gbps 全速率 = **100B / 10ns = 10000 B/µs**(100B MAC + 8B preamble = 108B 线上,9ns + 1ns IFG,NS 量化;理论必须用量化 slot,连续值 10416.67 B/µs 会系统性低估 RTT_eff);`RTT_eff = 0.009 + 2·delay + 0.001 µs`(数据 + 2·传播 + CtlOS,delay=2µs → 4.01µs)。s7 Part 1 实测 vs 理论(100G):**逐帧→512 帧(500)、ACK 1µs→640(625)、5µs→1152(1125)、20µs→3072(3000)**,误差 2%。(1G 历史实测:6(5.14)/7(6.18)/12(10.35)/26(25.98),误差 <16%,§12.25。)
>
> 指定缓冲可吸收的闪断上限(恢复后仍"热"重传、不 FLUSH):
>
> `T_supported(B) = min(T_hot, pcs_lost_max, data_age_max)`,`T_hot = B×100/10000 − RTT_eff` µs
>
> s7 Part 2 实测(2a 仅缓冲约束,bufFrames→T_hot,100G):4096→10µs(理论 28.8)、16384→100(127.1)、65536→200(520.3)、131072→200(1044.6;NACK 恢复边界主导)。**B 太小 → 无损背压停传**(吞吐降、不 FLUSH);**T ≥ 定时器上限 → FLUSH(与缓冲无关,大缓冲只换大丢弃)**——2b 定时器 1ms 时 buf=131072 的 1.2ms 闪断丢弃 125000 帧(Part 3 C2),尽管缓冲(1307µs 容量)足以装下闪断期全部帧。UEC 允许 `pcs_lost_status_timer_max` 到 4.29s,100Gbps 下 4.29s ≈ 53.6GB 缓冲,芯片容量即上限,故定时器才是真实上限。
>
>
> **定时器交互表**(s7 Part 3,本引擎保真度注):`replay_ct` FLUSH 墙钟 = `replay_ct_max × replay_timer_max`,**与缓冲大小无关**——`DoReplay` 在队头出队时刻重转定时器(非 UEC 的"最后重传帧线尾"时刻);`data_age` / `pcs_lost` 在固定墙钟触发(与缓冲无关,空缓冲也 FLUSH),丢弃帧数 ∝ 缓冲当前帧数。故"大缓冲支持更长时间闪断"仅在**热态**(`T_hot` 内)成立;超定时器上限后大缓冲 = 大丢弃。

### 6.11 计数器(LLR 22 项 / CBFC / AFH;无聚合类)
```cpp
struct SueLlrCounters {   // UEC §5.1.11 Table 5-13(共 22 项,`sue-llr-counters.h`)
  uint64_t tx_init_ctl_os, tx_init_echo_ctl_os, tx_ack_ctl_os, tx_nack_ctl_os;
  uint64_t tx_discard, tx_ok, tx_poisoned, tx_replay;
  uint64_t rx_init_ctl_os, rx_init_echo_ctl_os, rx_ack_ctl_os, rx_nack_ctl_os;
  uint64_t rx_ack_nack_seq_error, rx_ok, rx_poisoned, rx_bad;
  uint64_t rx_expected_seq_good, rx_expected_seq_poisoned, rx_expected_seq_bad;
  uint64_t rx_missing_seq, rx_duplicate_seq, rx_replay;
};
struct SueCbfcCounters {  // UEC §5.2.5.3 + 调试计数(标注,`sue-cbfc-counters.h`)
  uint64_t cf_update_tx, cf_update_rx, cc_update_tx, cc_update_rx;  // UEC
  uint64_t credit_exhausted[8];            // 调试(非 UEC 标准计数)
  uint64_t cc_lost_credits_recovered[8];   // 调试(非 UEC 标准计数)
  // + 循环计数器快照 S/R_VC_CC/CF(状态,调试)
};
// AFH 计数器无独立结构:SUESwitchNode 裸成员 m_rxPkts/m_txPkts/m_lookupMiss/
//   m_ttlZeroDrop/m_vcRx[8]/m_vcTx[8] + getter(GetCntRxPkts/GetCntTtlZeroDrop/
//   GetCntVcRx/.../ResetCounters);vc_rx/vc_tx 仅统计过 ingress 且转发帧。
// PFC 引擎无计数器结构(本期仅行为验证)。无 SueCounters 聚合类。
// SueNetDevice::PrintCounters(FILE*, Time) 输出 LLR 22 项 + CBFC(仅 VC0 两项),
//   AFH 计数不在其中;写调用方传入的 FILE*(唯一调用方 sue-fc-counters -> /tmp)。
```
- **计数器实际形态**:`SueLlrCounters`/`SueCbfcCounters` 为独立 `struct`(`sue-llr-counters.h`/`sue-cbfc-counters.h`);**`SueCounters`/`SueAfhCounters`/`SuePfcCounters` 聚合类未实现**(§4.1 注已更新)。
- **恒 0 / 计数**:`tx_poisoned` 原预留 N/A(无 TX 侧 FCS 上报)——2026-08-13 已实现 TX 毒化注入路径(`SueErrorModel::SetPoisonTxOnce/Every`,`tx_poisoned` 随之计数,UEC Table 5-13 项闭环;TX 毒化帧带 `SueFrameBadTag` 到对端,收端按 POISONED 接受、不 NACK、计 `frame_bad`,§12.35)。`rx_replay` **已实现**:`m_nackSent` 为真时收到期望帧(GOOD/POISONED)即计——接受重传(§6.2);`tx_replay` 按 **UEC Table 5-13 原文语义**计数:"number of times that the transmitter exited the REPLAY state (i.e., completed a replay operation)"——GoAdvance 时若 m_tx==REPLAY 计一次(FLUSH 退出不计,§12.31 A7);逐帧重传体积另计 debug 计数 `tx_replay_frames`(非 UEC 项);`tx_init_echo_ctl_os`/`rx_init_echo_ctl_os` 已实现并递增(INIT_ECHO 握手端到端,§6.2)。
- 调试计数器(`credit_exhausted`/`cc_lost_credits_recovered`/循环快照)标注为非 UEC 标准,仅排障。
- 调试输出:`PrintCounters(FILE*, Time)` 写到**调用方传入的 FILE***,唯一调用方 `sue-fc-counters.cc` 落盘 `/tmp/sue-counters.txt`;`SUE_DEBUG_LEVEL`/`SUE_COUNTER_OUTPUT_FILE` **不做**(§12.13,不接线)。
- 用于规格验证(对照 UEC 计数器语义)与回归基线。

### 6.12 SueLiteEndpoint(host 侧)✅ 已实现(`network_frontend/sue-lite-endpoint.{h,cc}`)
- **已实现**(S5b 前置阻断项闭环,§11 S5b 行):host 侧自模拟发包(大小/间隔/目的可配),替 512 卡压测提供统一应用级发包驱动。接口摘要:`SetDevice`(装收帧 upcall,替换模块默认 `HostDefaultRx`)、`SetSourceXpu`/`SetDestinationXpu`/`SetDestinationCycle(vector,逐帧轮转)`、`SetPayloadSize`(默认 86)/`SetVc(0..7)`(兼容便捷 = 同时设 shim-DSCP 与 VLAN-PCP 流类别为 `vc&0x07`)/`SetDscp(0..63)`/`SetPcp(0..7)`(通用 6/3-bit 流类别,经 §7 映射表得 VC;**先 `SetUseShim`/`SetUseVlan` 再设类**,避免误导告警)/`SetUseShim`/`SetUseVlan`/`SetEtherType`(校验该 etype 的 HAS_SHIM 与 useShim 一致)/`SetTtl`(4 位字段,默认 8,>15 钳制+告警)、`SetInterval`/`SetFrameCount`(0=无限)/`SetStartTime`/`SetDither`(MTP 等价微偏,§12.20)/`Start()`/`Stop()`、收帧统计 `GetTxCount`/`GetRxCount`/`GetRxBytes`/`GetRxByVc`/`GetRxByDst`/`GetRxBySrc(Map)`。**VC 编码(§7)**:`SendFrame` 本地出口队列 VC = 下游任何解码器从线头解出的 VC(同一张 SueConfig 映射表;shim.dscp 全 6-bit 入表)。安装 helper `SueInstallLiteEndpoints(cfg)` 遍历所有 host 各装一个"已绑定未启动"端点。
- **预留 `AstraNetworkAPI` 风格接口**(`sim_send`/`sim_recv`/`sim_schedule`,存在 + `NS_LOG_WARN` + 返回 -1),后续对接 astra-sim workload。
- `ENABLE_SCALEUP_WORKLOAD=0`(默认)时用内置生成器;`=1` 时 Start() 记 warn 并**回退内置生成器**(非静默 inert,§12.23)。
- **MTP 安全发包(§10.5/§12.21)**:端点**继承 `Object` 非 `Application`**(`Application::DoInitialize` 用普通 `Schedule` 调度 `StartApplication` → 公共 LP,违驻留);首发 `ScheduleWithContext(nodeId, ...)` 落节点 LP,自链式普通 `Schedule` 继承该 LP;`SendFrame` 每 arm 前 `SueMtpResidencyCheck(nodeId)`。已验证 `sue-endpoint-ring`/`run-s5b.sh` 全模式(ring/alltoall × VC0/VC3)MTP 双跑 hash 逐字节一致。
- 发包时按目的 XPU-id 构造 MAC DA(高 10 位置 XPU-id,`SueMacFromXpuId` 共享 helper,sue-header.h,`SueHeader::GetXpuId` 的逆,§12.22),按 VC 配置决定 shim/VLAN(shim 的 DSCP 或 VLAN PCP),与 §5/§7 一致;便于 S1/S5 用例对齐。VC 编码与 `SueNetDevice::ComputeVc` 一致(`shim.dscp&7 > vlan.pcp&7 > VC0`),收端同一公式归因。

### 6.13 SueChannel(专用通道,评审阻塞项修正)
```cpp
class SueChannel : public PointToPointChannel {  // 仿 QbbChannel(qbb-channel.cc:106 静态分派)
  // TransmitStart 经函数指针静态调度 &SueNetDevice::Receive(而非基类 &PointToPointNetDevice::Receive)
  // -> 确保 SueNetDevice::Receive(LLR/FC RX)被调用
  // 且收侧经 Simulator::ScheduleWithContext(dstNodeId, ...) 调度(S5a 已验证此即跨 LP 接缝)
};
```
- 必要性:`PointToPointChannel` 静态分派到基类 `Receive`(point-to-point-channel.cc:101),子类 `Receive` 不被调用;必须配套专用通道分派到 `&SueNetDevice::Receive`(与 `QbbChannel->&QbbNetDevice::Receive` 同构)。
- **多线程 / 跨 LP 接缝(S5a 已验证)**:`TransmitStart` 收侧以 `Simulator::ScheduleWithContext (m_dst->GetNode()->GetId(), txTime + m_delay, &SueNetDevice::Receive, ...)` 调度(sue-channel.cc:72)——报文在 MTP 自动分区下天然带目的节点 context、落在目的节点 LP 上执行 RX 管线,整个数据平面因此都保持在节点 LP 内。这就是 §10.5 上下文路由的既有接缝(设计初未刻意做,本轮验证确认)。MPI 的 remote-channel 钩子仍挂此(后续)。

---

## 7. VC 映射规则
8 VC(0–7)。**✅ 已实现(§12.8 决策项落地)**:通用流类别映射,`DSCP_TO_VC_MAP`(64 项 dscp→vc)/`PCP_TO_VC_MAP`(16 项 `(dei<<3|pcp)`→vc,2026-08-13 DEI 扩展)/`DEFAULT_VC` 三键,host 发送/接收与交换机 `ComputeVc` **从同一张 SueConfig 表**解出同一 VC(端到端一致)。规则:
1. shim 在场:`VC = DscpToVcMap[shim.DSCP]`(默认逐项直映 `DSCP&0x7`,64 项可配)。
2. shim 不在、VLAN 在场:`VC = PcpToVcMap[(DEI<<3)|VLAN.PCP]`(默认行内恒等 `pcp&0x07`,可配;仅列 8 个键时两 DEI 行相同,等价旧 8 项语义)。
3. 均不在:`DEFAULT_VC`(默认 0)。
> **实现注**:`SueConfig::DscpToVc/PcpToVc/DefaultVc` 惰性解析 + 首用缓存(`InvalidateCache()` 随 `SueReadConf`/`Config::SetGlobal` 失效;`SueReadConf` 解析后单线程预热,免 MTP 多 LP 并发冷缓存)。**部分覆盖语义**:未列出的 dscp/pcp 保持直映默认(`dscp&0x07`/行内恒等),故默认表 = 逐项 `0:0,…,63:7` 与旧固化逻辑 `shim.DSCP&0x07 > VLAN.PCP&0x07 > VC0` **逐字节等价**(既有 TestCase + scratch 零回归)。解码点统一:三处(`SueNetDevice::ComputeVc`、`SUESwitchNode::ComputeVc`、`SueLiteEndpoint::Rx` 归因)都查这张表。端到端证据:`sue-incast-2to1` 多 VC/非恒等运行下 host2 的 `g_rxByVc` 与交换机 `GetCntVcRx` 从线头解出同一 VC(§11 S5e;TestSuite §7 专项见 §11)。

---

## 8. 关键流程时序
> 本章为概念流(2026-08-13 同步到当前 API 与 §12.30 延迟退还);以 §6/§12 为准。

**8.1 数据帧 TX(egress)**
```
SUESwitch.SendToDev / host SwitchSend -> SueEgressQueue.Enqueue(vc)
SueNetDevice.DequeueAndTransmit(自研):
  LLR gate: GetTxBehavior() BLOCK 或 (LLR_ACTIVE && !CanTx()) -> return(排队等待)
  q = SueEgressQueue.GetNextSendableQindex(fc)             // FC 门控(单参重载)
  if (q < 0) return;                                        // 信用不足/被 pause,等待
  p = Dequeue(q)
  if (m_fc) m_fc->OnTx(q, p->GetSize())                    // S_VC_CC +=(不含 preamble)
  if (llrEnabled) m_llr->TxStore(p)                        // 分配 seq, 存 replay, 装 preamble
  TransmitStart(p) -> SueChannel(wire-time = size*8/rate, 含 preamble)
  (发送完成)TransmitComplete -> 控制队列 > 重传队列 > 数据(加 m_ifg 后)DequeueAndTransmit
  唤醒源:TransmitComplete / SwitchSend / ACK·NACK 部分释放(TxOpportunityCb,§12.31 B1)
        / GoAdvance(StateChangeCb)/ CF_Update 到达 / PFC resume
```
**8.2 数据帧 RX(ingress)**
```
SueChannel -> SueNetDevice::Receive(p)   // 静态分派(§6.13)
  PCS_status FALSE? -> drop all(闪断模型)
  LLR 路径:剥 preamble -> fcsStatus(GOOD/POISONED/BAD, SueErrorModel)
  res = m_llr->RxFrame(seq, fcsStatus)
  if (res == DROP) return;              // 规则 2:LLR 丢弃帧不达 OnRx/OnDrain
  m_fc->OnRx(vc, p->GetSize())          // R_VC_CC +=(类型 B 帧窗口内仍计,UEC Table 5-26)
  drain:defer(交换机 CBFC 端口)时**不退**——由 SUESwitch Forward 准入/丢弃路径
        RefundCredit 显式退还(§12.30);非 defer 按 drainDelay 立即/延迟 OnDrain
  upperLayerCallback(p, sh)             // -> SUESwitch 转发 / Endpoint 消费
```
**8.3 LLR 重传**
```
(超时或 NACK) -> m_llr->OnReplayTimer/OnAckNack(NACK)
  -> m_tx = REPLAY(引擎驱动 LLR 窗口开,规则 9 禁发 CC_Update,§12.31 B2)
  -> 从 replayBuf 取未确认帧重发(走设备 Retransmit 路径,不再扣 CBFC 信用=规则 1;
     不绕 PFC pause,UEC §5.1)
  -> replay_ct++; 若 replay_ct==max -> FLUSH(丢弃,上报上层丢包)
  -> 全部 ACK -> GoAdvance(窗口关,tx_replay++ = 完成一次重放)
```
**8.4 CBFC 信用回收**
```
(receiver 缓存释放/交换机退还) -> m_cbfc->OnDrain -> R_VC_CF +=
  -> CF_min 到期(脏 VC)或 CF_max 到期(全量)-> 生成 CF_Update CtlOS(高优先级,不耗信用)
(sender 收 CF_Update) -> m_cbfc->OnCfUpdate -> S_VC_CF=(累计直写)
  -> 唤醒 DequeueAndTransmit(信用可能已够)
(sender 收 CC_Update) -> OnCcUpdate:窗口内丢弃(规则 3);LostCredits 经环回比较防护
  (0 < lost < CC_MOD/2 才回收,§12.31 A2)
```

---

## 9. 配置设计(SimAI.conf 风格新增项)
```
ENABLE_SCALEUP 0
ENABLE_SCALEUP_WORKLOAD 0        # 0=内置生成器, 1=对接 astra-sim workload(接口预留,=1 时 SueLiteEndpoint 记 warn 并回退内置生成器,§6.12/§12.23)
SUE_ETH_TYPE_0 0xFCF1             # 对称 SUE 协议标识 #0
SUE_ETH_TYPE_0_HAS_SHIM 0          # 该 EtherType 的帧按带 shim 解析? (§5.2)
SUE_ETH_TYPE_1 0xFCF2              # 对称 SUE 协议标识 #1
SUE_ETH_TYPE_1_HAS_SHIM 1          # 该 EtherType 的帧按带 shim 解析? (§5.2)
SUE_IFG_BYTES 12                  # IFG 线字节数;ApplySueConfig 按已设链路速率折算线时间(§5.4)
# ---- 链路/转发时延(全铜缆互联场景;0=关闭保持基线) ----
# 注:本节取值 = conf 预置(参考部署值,§12.31);GlobalValue 注册默认:PROPAGATION=0、FWD_BUDGET=0(基线,不经 conf 时生效)。
SUE_LINK_PROPAGATION_NS 10        # 每条链路单程传播延迟(10ns/跳,铜缆;控制帧同享 -> MTP lookahead=10ns);0=零传播基线(§6.8)
SUE_SWITCH_FWD_BUDGET_NS 300      # 交换机每跳转发预算:转发时延 + 出口 wire <= 300ns(拥塞排队另算,§6.8);0=直发无处理时延(基线)
# LLR(UEC §5.1.4 两相使能 + 三行为;替代原单键 LLR_ENABLE,已移除)
LLR_MODE_LOCAL 1                  # llr_mode_local:1=RX LLR 有效(可收 LLR_INIT/INIT_ECHO、eligible 帧按 seq 过滤);0=RX 直通不过滤
LLR_MODE_REMOTE 1                 # llr_mode_remote:1=TX LLR 有效(可发 LLR_INIT、eligible 帧分配 seq+replay);0=TX 走 BEST_EFFORT
LLR_OUTSTANDING_SEQ_MAX 65536
LLR_OUTSTANDING_DATA_MAX 1048576
LLR_REPLAY_BUF_MAX 1048576       # 重传缓冲存储容量(可缓存帧数,§6.10);须 ≥ 在途数据 + 闪断期发送帧量(UEC L303);与 OUTSTANDING_SEQ_MAX(在途窗)/OUTSTANDING_DATA_MAX(在途字节 BDP)独立
LLR_REPLAY_TIMER_MAX_NS 21830     # 100G 预置值 = 10×LLR RTT(§12.25/§12.27) = 21.83µs(2×wire 9.6ns + 2×prop 1µs + ackPeriod 163.84ns,×10);注册默认 0=代码自动折算(同 §12.27;自动路径 prop 读 SUE_LINK_PROPAGATION_NS,§12.31 C7)。21830ns 在 UEC Table 5-9 寄存器上限 65535ns 内(原 1G 档 203040ns 超限,已随 100G 收口移除,§12.36)。曾静态默认 1000ns,1G 下 < CtlOS 门(400B=3.2µs)+ACK 往返 → 每 µs 无谓重传 → replay_ct 耗尽 FLUSH 级联(s6-entry 回归,§12.27,历史实测 0.460 Gbps)
LLR_REPLAY_CT_MAX 5
LLR_INIT_TIMER_MAX_NS 21830       # INIT 握手重试超时(独立于 replay 定时器,§5.9);100G 预置值同 replay_timer(21830ns@100G,公式见上);注册默认 0=代码自动折算(同 §12.27)
LLR_INIT_RETRY_MAX 3              # INIT 重试上限;超限仍无对端引擎 -> GoAdvance 兜底
LLR_DATA_AGE_TIMER_MAX_NS 50000   # 经 ApplySueConfig 生效;引擎代码默认 10s 仅兜底
LLR_PCS_LOST_STATUS_TIMER_MAX_NS 0       # pcs_lost_status_timer_max(ns):PCS_status FALSE 持续超过该值 -> FLUSH(闪断时长模型,§5.1.10/§6.10/§12.18)。0=立即过期(UEC "0=immediate expiration");设非 0 阈值可区分短/长闪断(scratch s5-pcs-lost)
LLR_INIT_BEHAVIOR BLOCK           # INIT 态对 LLR-desired 帧:DISCARD|BLOCK|BEST_EFFORT;默认 BLOCK=取帧排队、无丢包(§5.1.4)
LLR_FLUSH_BEHAVIOR BLOCK          # FLUSH 态同上;默认 BLOCK(§5.1.4)
LLR_RE_INIT_ON_DISCARD 1          # FLUSH 后:1=自动重入 INIT(发 INIT 等 INIT_ECHO);0=停留 FLUSH,管理方 Start() 重入
LLR_CTLOS_TARGET_SPACING 2048     # 引擎 ACK 周期(线字节)-> 按数据率折算;取值钳制 UEC Table 5-9 范围 [400,16384](400=最大速率界"前 400 字节"下限;16384<17296B 最小速率界)。驱动引擎累积 ACK 周期(UEC §5.1.6,init_echo_sent 后每周期一条累积 ACK,空闲周期重复末 seq;NACK_SENT 抑制)。0 亦被钳到 400——**不再表示"不节流"**(旧 `LLR_CTLOS_MIN_INTERVAL_NS` 键已删除)。**设备 CtlOS pacing 门已与 ACK 周期解耦**(§12.26):门固定 UEC §5.2.11.2 的 400B floor(`CTLOS_MIN_SPACING_BYTES`),不再由本键驱动——ACK 流不再独占 pacing FIFO、低优先级 CF_UPDATE 不被 ACK 积压
# CBFC
CBFC_CREDIT_SIZE 256
CBFC_CC_MACDA 01:80:c2:00:00:01   # CC_Update MACDA(UEC §5.2.6.2 建议可配)
CBFC_CC_CID 0xFA7ACB              # CC_Update CID(24 位,建议可配)
CBFC_NUM_VCS 8                           # ⏭ 预留后续(§12.13;固定 8 VC)
CBFC_TOTAL_CREDITS 4096                  # ⏭ 预留后续(§12.13)
CBFC_VC_CREDIT_LIMIT 0:512 ... 7:512
CBFC_PKT_OVHD 0                 # MAC 帧额外开销(不含 preamble,preamble 已剥离)
CBFC_CC_MSG_TIMER_US 102   # THU 对齐:9,790/s = 102.1µs(§12.37);注册默认 100µs;0=关闭
CBFC_CF_THRESHOLD_BYTES 2075  # 数据量触发(SDK credit_threshold 对等物):累计释放信用 ≥ N 字节即上报;阈值字节 × 端口速率 = 周期字节数恒定(速率无关)。THU 对齐:2048(SDK th=4×512B)+ ~27B CF 消息自身线时 = 2074.7B 实测等效 → 取 2075,100G 6,024,096/s vs THU 6,024,685(§12.37);0=关闭。注意 N 必须 ≤ 信用窗口(CL×creditSize),否则发送端在接收端攒够阈值前停摆
CBFC_CF_MIN_TIMER_BYTES 0   # CF_min_timer(线字节)地板(UEC §5.2.6.1 触发 a);0=无引擎地板,阈值触发即时发送(THU 默认档 threshold 主导行为;设备固定 400B CtlOS 门封顶 2% 帽);>0 时 CF 节奏=1/CF_MIN(地板钳制,§12.37);注册默认 0=回退逐帧直发(§5.6/§12.20)
CBFC_CF_MAX_TIMER_BYTES 1250000 # CF_max_timer(触发 b:周期全量上报,防丢失);@100Gbps=100µs;线速下叠加阈值流 +0.66% 消息量;注册默认 0=关闭
CBFC_EGRESS_BUF_BYTES 0         # 交换机 CBFC 端口出口 per-VC 缓冲深度门(B);0=auto=creditLimit[vc]×creditSize(每 VC 至多一个信用窗);>0=统一 per-VC 上限。只作用于 CBFC 模式交换机端口(host 零变化);出口→入口背压耦合(§6.7/§12.30)
# PFC(对比用;默认值=代码默认,DYNAMIC_THRESHOLD=1 联动共享池开)
PFC_DYNAMIC_THRESHOLD 1         # 共享池动态阈值开(PFC_SHARED_POOL_SIZE>0 生效)
PFC_PAUSE_QUANTA 0xFFFF          # pause 帧 quanta(0xFFFF=暂停至显式 resume,§5.10)
PFC_BUFFER_SIZE 65536           # 接收缓冲(B)
PFC_SHARED_POOL_SIZE 32768      # 共享池(B);0=关(与 DYNAMIC_THRESHOLD 联动)
PFC_HEADROOM_SIZE 8192          # 共享池 headroom(B)
PFC_RESUME_OFFSET 4096          # 共享池 resume 偏移(B)
PFC_ALPHA_SHIFT 0               # 共享池动态阈值 alpha 移位
PFC_PAUSE_THRESHOLD 32768       # 简单模式单 VC pause 阈值(B)(DYNAMIC_THRESHOLD=0 时用)
PFC_RESUME_THRESHOLD 16384      # 简单模式单 VC resume 阈值(B)
# 流控总开关
FC_MODE CBFC        # OFF|CBFC|PFC
# 多线程(S5a,§10.5)
SUE_MTP_THREADS 1   # 线程数:1=单线程(不启用 NS3_MTP);>1=front-end 启用单进程多线程模拟器(MTP 自动分区,每节点一 LP)。sue 模块本身零 mtp 依赖,仅读键
# VC 映射(§7,通用流类别映射;端到端同一张表)
DSCP_TO_VC_MAP 0:0 ... 63:7    # 64 项 shim DSCP->VC;默认直映=dscp&0x07;未列出的保持直映(部分覆盖)
PCP_TO_VC_MAP 0:0 ... 7:7      # 16 项 VLAN (dei<<3|pcp)->VC;键 8..15 为 DEI=1 行;默认行内恒等;仅列 0..7 时两行相同
DEFAULT_VC 0                   # 无 shim 且无 VLAN 的帧回退 VC
# 链路/错误
LINK_ERROR_RATE_PER_LINK 1e-9    # 每帧 FCS 损坏概率;SetupSueNetwork 从拓扑 err_rate 写回(记录值,§6.8);
                                 # 随机注入已实现(PLAT-SP-08):SetupSueNetwork 按链路 err_rate 建概率 FCS 模型(每链路独立 RNG 流,确定性);确定性 corruptor 集合保留供场景测试(§10.1)
# 时序精度
SUE_TIME_RESOLUTION PS        # CF_min_timer 8B@800G=80ps < 1ns,需皮秒级
                              # 实现注:入口分发读到 ENABLE_SCALEUP!=0 时自动 SetResolution(PS)(仅升级,不改已设精度),见 §6.8
# 调试(— 不做,§12.13;计数器经 PrintCounters(FILE*) 输出,§6.11)
SUE_DEBUG_LEVEL 1
SUE_COUNTER_OUTPUT_FILE /etc/astra-sim/sue_counters.txt
```

> **实现状态(§9 键,以实现为准)**:
> - **端口速率预置**:速率在**拓扑** `link` 行(`sue-entry.cc` 解析),conf 无速率键。随仓库交付三个速率预置 conf(`conf/sue-100g|200g|400g.conf`),数值一致(UEC 字节键速率无关),仅折算时间间隔不同;对照表见 config-guide §2.7、机制见 §5.7。
> - **已注册 + 已实现**(`sue-config.cc` GlobalValue + `g_keys[]` 解析表 + `ApplySueConfig` 应用):`ENABLE_SCALEUP`、`SUE_ETH_TYPE_{0,1}`、`SUE_ETH_TYPE_{0,1}_HAS_SHIM`、`LLR_MODE_LOCAL`、`LLR_MODE_REMOTE`、`LLR_INIT_BEHAVIOR`、`LLR_FLUSH_BEHAVIOR`、`LLR_RE_INIT_ON_DISCARD`、`LLR_REPLAY_TIMER_MAX_NS`、`LLR_REPLAY_CT_MAX`、`LLR_DATA_AGE_TIMER_MAX_NS`、`LLR_INIT_TIMER_MAX_NS`、`LLR_INIT_RETRY_MAX`、`LLR_OUTSTANDING_SEQ_MAX`、`LLR_OUTSTANDING_DATA_MAX`、`LLR_REPLAY_BUF_MAX`、`LLR_CTLOS_TARGET_SPACING`、`LLR_PCS_LOST_STATUS_TIMER_MAX_NS`、`SUE_LINK_PROPAGATION_NS`、`SUE_SWITCH_FWD_BUDGET_NS`、`CBFC_CREDIT_SIZE`、`CBFC_CC_MACDA`、`CBFC_CC_CID`、`CBFC_VC_CREDIT_LIMIT`、`CBFC_PKT_OVHD`、`CBFC_CC_MSG_TIMER_US`、`CBFC_CF_MIN_TIMER_BYTES`、`CBFC_CF_MAX_TIMER_BYTES`、`CBFC_CF_THRESHOLD_BYTES`、`CBFC_EGRESS_BUF_BYTES`、`PFC_PAUSE_QUANTA`、`PFC_DYNAMIC_THRESHOLD`、`PFC_BUFFER_SIZE`、`PFC_SHARED_POOL_SIZE`、`PFC_HEADROOM_SIZE`、`PFC_RESUME_OFFSET`、`PFC_ALPHA_SHIFT`、`PFC_PAUSE_THRESHOLD`、`PFC_RESUME_THRESHOLD`、`FC_MODE`、`SUE_IFG_BYTES`、`SUE_MTP_THREADS`、`DSCP_TO_VC_MAP`、`PCP_TO_VC_MAP`、`DEFAULT_VC`、`LLDP_NEGOTIATE`。默认:`FC_MODE=CBFC`;`SUE_LINK_PROPAGATION_NS=0`、`SUE_SWITCH_FWD_BUDGET_NS=0`(注册默认=基线,conf 预置 10/300,§12.31 A8)。原 `LLR_ENABLE` 已移除(旧 conf 该行作未知键静默跳过,镜像 baseline;§12.6)。
> - **已注册 + 生效(PLAT-SP-08)**:`LINK_ERROR_RATE_PER_LINK`——`SetupSueNetwork` 从拓扑 `err_rate` 解析后 `Config::SetGlobal` **写回**(可观测),并按链路 err_rate 给两端设备挂**概率 FCS 错误模型**(每链路独立 RNG 流,全局种子下确定性;0=无模型)。确定性 corruptor 集合保留供场景测试(§10.1)。
> - **表中列出但未注册**:`SUE_TIME_RESOLUTION`(入口分发自动升级 PS,§6.8)及标注"⏭ 预留后续"/"— 不做"的键(§12.13 三态;解析时静默跳过,镜像 baseline)。原 `PFC_ENABLE` 已**从表与代码中删除**(§12.7)。`ENABLE_SCALEUP_WORKLOAD` 已随 SueLiteEndpoint **注册**(默认 0,§12.23),不再属"未注册"。
> - **EtherType 运行时化**:`SueConfig::EthType0()/EthType1()/HasShim()` **首次使用时缓存**(file-static `std::optional`,`SueReadConf`/`Config::SetGlobal` 后经 `InvalidateCache()` 失效),非逐帧实时 `GetValue`。配置须在流量开始前经 `SueReadConf` 应用。
> - **IFG**:`ApplySueConfig` 在 `dev->IsDataRateSet()` 时按 `SUE_IFG_BYTES`(默认 12)以链路速率折算线时间——`SetIfg (rate.CalculateBytesTxTime (ifgBytes))`(§5.4),不再硬编码 96ns。
> - **应用时序**:拓扑链路行 `llr_ena` 决定是否构造 LLR 引擎(§6.8);`LLR_MODE_LOCAL/REMOTE`、`LLR_INIT/FLUSH_BEHAVIOR`、`LLR_RE_INIT_ON_DISCARD` 经 `ApplySueConfig` **落到引擎**(设模式/行为)——引擎**是否启用以拓扑为准**,引擎内 RX/TX 行为**以 config 为准**(§6.8,决策 §12.6)。
> - `SUE_DEBUG_LEVEL`/`SUE_COUNTER_OUTPUT_FILE` 保留未实现(计数器经 `PrintCounters(FILE*)` 输出,§6.11)。

---

## 10. 仿真建议

### 10.1 链路与错误建模
- 用 **`SueChannel`**(extends `PointToPointChannel`,§6.13)而非纯上游通道--纯上游通道静态分派到基类 `Receive`,LLR/FC RX 不会执行。带宽/延迟/MTU 照常。
- `SueErrorModel`:坏帧**打 `SueFcsTag{bad}` 后照常送达**(非丢弃),`SueLlrEngine::RxFrame` 走 `fcsGood=false`。POISONED 模式(错误传播)可配概率注入,验证"POISONED 期望帧被接受"路径。
- **确定性 vs 随机注入(决策 §12.5)**:链路错误两路并存——确定性 corruptor 集合(何时哪条链路坏是确定场景,供场景测试 §10.1)与**按概率随机 FCS 注入(✅ 已实现,PLAT-SP-08/§12.5 后注)**:拓扑 `err_rate` 驱动 `SueErrorModel` 概率模型,`LINK_ERROR_RATE_PER_LINK` 写回 GlobalValue 作记录值(§6.8)。
- preamble 带宽:见 §5.4(8B Header 计入)。
- CtlOS 带宽:见 §5.7(独立 8B/64B 包串行化)。
- IFG:由 `SueNetDevice::TransmitComplete` 自管 `m_ifg`(§5.4)。

### 10.2 分层解耦
- LLR/FC 封装在 `SueNetDevice`;上层只见无丢包链路。
- LLR/CBFC/PFC 引擎可独立单测(§11)。

### 10.3 CtlOS 建模
- 见 §5.7;高优先级控制队列,不消耗 CBFC 信用,LLR-ineligible。

### 10.4 时间精度
- 定时器 ns~µs 级;但 `CF_min_timer=8B` 在 800G 下 = `8*8/800G = 80ps`,**低于 ns-3 默认 NS 分辨率(1000ps)**,会被舍入。
- 故需 `Time::SetResolution(Time::PS)`(皮秒级;入口分发读到 `ENABLE_SCALEUP!=0` 时自动升级,仅升级不改已设精度,§6.8/§9 注 `SUE_TIME_RESOLUTION` 键**未注册**、不接线),并在文档/代码注明舍入误差对极小定时器的影响。
- `CF_min/max_timer` 以 wire-bytes 给出,按链路速率换算 `t = bytes*8/rate`。

### 10.5 多线程(`NS3_MTP`,必选)——S5a 已闭环:单进程多线程 × 节点上下文路由
fork 自带 `src/mtp`(`MultithreadedSimulatorImpl` + `MtpInterface` + `LogicalProcess`)。启用:`MtpInterface::Enable(n)` + `Simulator::SetImplementation(CreateObject<MultithreadedSimulatorImpl>())`,须在建网前调用(scratch `sue-mtp-equivalence.cc` 在读回 `SUE_MTP_THREADS` 后、`SueEntryDispatch` 之前做)。`Run()` 时对全网 BFS、以链路延迟为 lookahead 自动分区:零延迟 SUE 链路 → **lookahead=0** → `grantedTime = Min(最小事件时间 + lookahead, nextPublic)` → 全局时间有序 lockstep;每节点 `SetSystemId()` 归属唯一 LP,节点状态**单 LP 独占**。

**正确性缺陷(已修,根因)**:`SueNetDevice::GetChannel()` 继承自基类,返回基类私有 `m_channel`——`SueNetDevice::Attach` 从不设置基类成员(设置的是子类 `m_channel`),于是 MTP 分区器看不到任何链路 → lookahead 取默认 `Time::Max()/2` → 每个 LP 把自身事件表跑在信箱时钟之前 → `evPast` 陡增、`delivered=0`。修复:子类覆写 `GetChannel()` 返回子类 `m_channel`(sue-net-device.cc:68;基类 `PointToPointNetDevice::GetChannel` 非虚,须 `override` 声明)。修复后 lookahead=0ns、evPast=0、delivered=1600。**这是一处真实 bug,非仅线程问题**——单线程路径因不查 lookahead 而从未暴露。

**节点状态单 owner = 上下文路由,不加锁(用户决策)**:auto-partition 下每个节点只归一个 LP,临界区问题退化为"节点定时器必须从节点自己的 LP arm"——同 LP 才能 `Simulator::Cancel`(跨 LP 的 `ScheduleWithContext` 返回 void、信箱事件 fire-and-forget、不可取消)。显式加 `explicitCriticalSection` 是全局单锁,会串行化所有 LP,否决。落地:
- `SueLlrEngine`:成员 `m_context`(默认 `NO_CONTEXT`)+ `SetContext/GetContext`;9 处定时器 arm(init×2、replay×2、data_age×2、ack×2、pcs_lost)统一走模板 `ScheduleCtx`(透传普通 `Schedule`)。设备 `SetLlrEngine` 注入 `llr->SetContext(GetNode()->GetId())`(sue-net-device.cc:111)。
- `SueNetDevice`:模板 `ScheduleSelf(delay, fn, ...)`(同透传普通 `Schedule`);7 处数据面定时器(CtlOS pacing×2、CC_Update、OnDrain、OnCcUpdateTimer、TransmitComplete、IFG DequeueAndTransmit)改走 ScheduleSelf。
- **LP 驻留保证(为何普通 Schedule 就够)**:MTP 下跨 LP 的 `ScheduleWithContext` 返回 void、信箱事件 fire-and-forget、不可取消——所以节点定时器必须 arm 在自己 LP 的堆里。两路保证驻留:① **数据平面**:收侧经 §6.13 的 channel `ScheduleWithContext(dstNodeId, ...)` 接缝落目的节点 LP;发侧由节点 context 流量生成器 / 转发链路触发,故 TX 管线与所有 RX/OnDrain/OnCcUpdate 回调都在拥有 LP 线程内——arm 点天然驻留,`Simulator::Schedule` 即入本 LP 堆;② **两处 setup 期(公共 LP)arm defer 到节点 LP**。模板集中所有 arm,是把这条驻留契约固化为单一通道。
- **两处 setup 期(公共 LP)定时器 defer**:① `SueLlrEngine::Start()` 在 MTP 下不直接 `StartInit`,改 kick `t==0` 的 `ScheduleWithContext(m_context, 0, StartOnLp)` 事件——`Partition()` 在任一线程启动前 `InvokeNow` 把它 re-home 到节点 LP,`StartOnLp→StartInit` 的 init 定时器落进该 LP 的事件堆;② `SueNetDevice::StartCcUpdateTimer` 同理 defer 到 `ArmCcUpdateTimerOnLp`。非 MTP 下 `DefaultSimulatorImpl::ScheduleWithContext` 忽略 context(= `Schedule`),以上全部**逐位无行为变化** → 单线程基线零回归(37 TestCase + 23 scratch 全 PASS)。

**CF_UPDATE flood × ackPeriod==pacing slot 耦合(验证期关键发现,场景级)→ ✅ 已由 §12.26 解耦根治**。历史:`ApplySueConfig` 把引擎 ACK 周期耦合到 CtlOS pacing slot——`SetAckPeriod (CalculateBytesTxTime (spacing))`(spacing=2048 → 163.84ns@100Gbps;1G 历史 16.4µs)。ackPeriod==slot 时,**累积 ACK 节奏单独就填满 pacing FIFO**(每 slot 一条 ACK),于是每个 CBFC `CF_UPDATE`(§12.2 定时器门控前为每 drain 一帧直发)排到 ACK 之后 → ACK 返回被 CF_UPDATE 积压延迟 → 短 replay 定时器(5ms)在缓冲非空时触发 → `m_tx=REPLAY`、`CanTx()` 阻塞新 TX → 队列积压 → dataAge(10ms)FLUSH → 丢弃 + seq 重启后重复投递(delivered=1398/1600)。**当时解法(S8 场景)**:全部 LLR 定时器设非绑定(1s,15ms 运行期内不触发),只让确定性单帧损坏的 NACK 驱动重传(`OnAckNack→DoReplay` 即时路径,非定时器依赖);坏帧 `SetCorruptOnce(100)`(非 `SetBadEvery`,避免重传帧再被破坏)。CF_min/CF_max 定时器门控落地后(§12.2),`CF_UPDATE` 按 1ms/10ms 批量上报、不再逐帧直发,该积压耦合进一步缓解。**根治(§12.26)**:门与 ACK 周期解耦——门固定 400B floor,ACK 周期单独由 `LLR_CTLOS_TARGET_SPACING` 驱动(钳制 [400,16384]),ACK 流不再独占 pacing FIFO。S8 场景保持非绑定 LLR 定时器仅为确定性 FLUSH/NACK 双模式测试所需,耦合本身已解除。

**等价性验证(S8,§11)**:`sue-mtp-equivalence.cc` 多平面 topo(8 host + 2 ToR + 2 spine = 2-plane ECMP,全部链路 CBFC+LLR + CF_min/CF_max 定时器门控),**双坏帧模式**:NACK=`SetCorruptOnce(100)`(1 NACK → 对端缓冲重传 83 帧 → 干净收敛)、FLUSH=`SetCorruptFirstN(10)`(确定性连坏 → replay_ct_max 仍用非绑定定时器 → dataAge 触发 FLUSH+INIT 重同步)。每 host 起始偏移加互异 dither(0/137/…/959ns,§12.20)保证出发时刻全局唯一。线程数经 `SUE_MTP_THREADS` 读回;>1 时启 MTP。断言:baseline(threads=1)与 MTP-8 的**聚合结果 hash**(全设备 LLR+CBFC 计数器 + delivered/bytes)逐字节一致;12 逻辑进程自动分区(分区表打印,信息);NACK/FLUSH 两模式均 `./run-s8-mtp.sh [N]` 双跑 diff hash → `S8 MTP EQUIVALENCE PASS`。FLUSH 模式的全 hash 在 spine→ToR 链(n8d4)`rx_ack` 有一处 ±1——残留 ACK 时钟决胜(周期 ACK 定时器与跨 LP 数据到达同时间戳,Default/MTP 全序不同),等价判据用**行为 hash**(`S8 BHASH`,掩 ACK/NACK CtlOS 计数层,§12.28),双模式现均 PASS。

**构建**:`build.sh` configure 加 `-DNS3_MTP=ON`(并入默认);`run-sue-tests.sh` 将 MTP 库加入 runner 链接;`src/point-to-point/CMakeLists.txt` 链接 `${mtp_libraries}` 解析 `rdma-hw.cc` 的 `MtpInterface` 符号。**sue 模块零 mtp 依赖**:`sue-mtp.h` 仅按活跃 Simulator 实现的 TypeId 名检测(`"ns3::MultithreadedSimulatorImpl"`),不 include 任何 mtp 头。全量回归(NS3_TESTS=ON):46/46 TestCase + 26 scratch PASS。**注意**:NS3_TESTS=ON 下 `all` 目标会触发 fork 既有 bug——`src/mtp/test/mtp-test-suite.cc` include 不存在的 `ns3/mtp.h`,须建定向目标(`libsue-test`, `scratch_*`)而非 `all`。

**目标规模**:512 卡 / 32 交换机 / 多平面;S5b 压测(`SueLiteEndpoint` 前置阻断项已闭环,§6.12/§11;512 卡压测本身待跑)。

**S5 收尾清单(评审提醒,随 S5b)**:① **MTP 驻留不变量**(✅ 已落地,§12.19)——`ScheduleCtx`/`ScheduleSelf` 靠调用方 LP 驻留而非显式上下文校验,`SueLiteEndpoint` 等新发包路径**必须用节点 context**(`ScheduleWithContext`)驱动,否则定时器落公共 LP、`Cancel` 失效。已加 debug 断言 `SueMtpResidencyCheck`(sue-mtp.h):MTP 下校验 `当前 LP == NodeList::GetNode(nodeId)->GetSystemId()`,违规即 `NS_ASSERT_MSG` abort;负测试已证——去掉 s8 SendOne 的节点 context 后 MTP-8 在首个 TX 定时器 arm 触发 `timer armed on LP 0 but node 0 is owned by LP 1`。② **FLUSH+INIT 重同步路径的 MTP 等价性补测**(✅ 已闭环,§12.20)——S8 增 `SetCorruptFirstN(10)` FLUSH 模式,`run-s8-mtp.sh` 双模式(NACK+FLUSH)双跑 diff hash 全 PASS。③ **等价性场景约束**(✅ 已落地,§12.20)——任何依赖同时间戳跨 LP 事件顺序的场景,须保证出发时刻全局唯一(每 host 互异非 20us 倍数 dither),否则 Default(FIFO)vs MTP(LP 序)"内容→槽位"分配互换 → 刀锋翻转。④ **基线 `all`@`NS3_TESTS=ON` 失败**(fork `mtp-test-suite.cc` include 不存在 `ns3/mtp.h`)记录于 AGENTS.md。MPI 后续(remote-channel 钩子挂 §6.13)。

### 10.6 scale-up / scale-out 共存
- 节点类型由拓扑 `type` 决定;`SUESwitchNode` 与 `SwitchNode` 可同图;`ENABLE_SCALEUP` 仅控入口默认。

---

## 11. 验证计划(小步)
| 阶段 | 目标 | 用例 | 状态 |
|---|---|---|---|
| S0 | 框架 | `sue` 模块编译;空设备/节点/通道可实例化;`ENABLE_SCALEUP=0` scale-out 回归不破 | ✅ 已闭环(`sue-afh-device-dispatch`) |
| S1 | AFH 转发 | 2 host + 1 SUESwitch;四帧组合(±shim±vlan);XPU-id LPM;TTL;VC 派生;ECMP | ✅ 已闭环(`sue-afh-forward`/`sue-afh-header-parse`) |
| S2 | CBFC 单链路 | 信用耗尽停发/CF_Update 回收续发/CC_Update 恢复;preamble 不计入信用;8 VC 独立 | ✅ 已闭环(`sue-afh-wire-time`/`sue-cbfc-engine-credit`/`sue-cbfc-e2e-gating`) |
| S3 | LLR 单链路 | 误码触发 NACK->重传/ACK 释放/超时重传/replay_ct_max->FLUSH;POISONED 路径 | ✅ 已闭环(`sue-llr-engine`/`sue-llr-preamble-fcs`/`sue-llr-flush-init`/`sue-llr-poisoned`/`sue-llr-timer-wakeup`) |
| S4 | LLR+CBFC 共存 | 九规则计数器验证 | ✅ 已闭环(`sue-fc-llr-cbfc-coexist`/`sue-cbfc-cc-update-onwire`/`sue-fc-counters`/`sue-fc-facade`) |
| S4b | PFC 对比 | 同拓扑换 FC_MODE=PFC;与 CBFC 对照 | ✅ 已闭环(`sue-pfc-gating`) |
| S4c | 控制帧真实化 | PFC/CC_Update 线上满 64B 以太网帧 + CtlOS 短报文+速率门控(`sue-fc-ctl-onwire.cc`);报文头字节级布局(MACDA/SA/EtherType/Opcode/CID/PEV/quanta/20-bit 打包/FCS=CRC-32)与往返(`sue-fc-frame-headers.cc`);`SuePfcHeader`/`SueCbfcCcHeader` 单测 | ✅ 已闭环(`sue-fc-ctl-onwire`/`sue-fc-frame-headers`) |
| S4c′ | LLR 闪断(pcs_lost) | 短闪断(< timer_max)→replay 恢复无 FLUSH;长闪断(≥ max)→pcs_lost FLUSH 丢弃在途帧 + re-INIT 重同步;空闲链路长闪断→空缓冲也 FLUSH(`sue-llr-pcs-lost.cc`,最小 PCS 模型:场景驱动 `SetPcsStatus`) | ✅ 已闭环(`sue-llr-pcs-lost`) |
| S6 | 入口集成 | `sue-config`(SueReadConf/ApplySueConfig)与 `sue-entry`(SetupSueNetwork/SueEntryDispatch)端到端:配置解析 + BFS 路由 + 帧投递;100G 默认配置端到端回归(`SueDefaultConfig100GTestCase`,§12.27:定时器 0=自动折算,单帧收 1、tx_replay=0) | ✅ 已闭环(`sue-cfg-parse-apply`/`sue-entry-dispatch`;S6 ENTRY PASS;1G 默认配置回归纳入 ctest) |
| S5a | 单进程多线程(适配) | `NS3_MTP` 自动分区 × 节点上下文路由;baseline(threads=1) vs MTP-8 **聚合结果 hash 逐字节一致**;12 逻辑进程自动分区;delivered=1600/1600、无 tx_discard、无 FLUSH;NACK(`SetCorruptOnce(100)`)与 FLUSH(`SetCorruptFirstN(10)`)双模式 MTP 等价 | ✅ 已闭环(`sue-mtp-equivalence.cc`/`run-s8-mtp.sh`;39 TestCase + 23 scratch 零回归;S8 NACK/FLUSH MTP EQUIVALENCE PASS,FLUSH 经行为 hash 判据 §12.28) |
| S5b | 端到端(压测) | 多 XPU + 多 SUESwitch + 多 plane ECMP;`SueLiteEndpoint` 驱动;**正常无丢包(LLR 重试耗尽仍 FLUSH 丢包,见 §6.10)**;512 卡规模压力 | ✅ 已闭环(`sue-endpoint-ring`/`run-s5b.sh`:ring/alltoall × VC0/VC3 四组合双跑 hash 逐字节一致,ΣRx==ΣTx==800/800 无丢包无 FLUSH;**512 卡满负载压测(E7,§12.34,`sue-scale-stress`)**:64 机/512 XPU/578 节点/640 链路——clean(63G/XPU,32T 需求)无损 98368/98368、持续 31.4T;sat(168G/XPU,86T 需求)织物饱和 48.0T=93.7% 截面积(51.2T)、credit_exhausted=221528 全帧投递 261760/261760;双平面 49.7/50.3 均衡;MTP-8 结果逐位一致但墙钟 69.5s vs 单线程 9.3s(LP 同步开销 > 并行收益,低负载同结论);无负载 578 LP/~43MB RSS/1.65M 事件/ms-sim) |
| S5e | CBFC 出口→入口背压(incast) | 3 XPU 全 200G、host0+host1 双发 host2 的 2:1 incast;出口拥塞真实节流发送端 + 无损 + 200G 线顶 + `credit_exhausted>0` + 交换机挂起 | ✅ 已闭环(`sue-incast-2to1`/`run-s5e-incast.sh`:CREDITS=1024 INCAST PASS,credit_exhausted=188515、pendMax=196、delivered==sent=122072 无损、195.871 Gbps 窗顶;512 双背压并存 PASS;§12.29 发现/§12.30 决策,CBFC_EGRESS_BUF_BYTES 默认开)。**VC 映射端到端(§7)**:`[DSCP0] [DSCP1] [DSCP_TO_VC_MAP]` 位置参数 → `INCAST_DSCP0/1` + 内联 conf 注入;多 VC(0/3)与 3:7 非恒等映射都 PASS——host2 `g_rxByVc` 归因 == per-source、交换机 `GetCntVcRx` 从线头解出同一 VC、双 VC `credit_exhausted>0`(per-VC 独立背压) |
| S7 | 缓冲选型 | LLR 三门限独立生效 + `LLR_REPLAY_BUF_MAX` 配置路径(`SueLlrCanTxLimitsTestCase`);闪断上限 × ACK 间隔 → 最小缓冲 + 定时器交互(`sue-llr-replay-buffer.cc`,§6.10) | ✅ 已闭环(`s7-replay-buf-rtt`/`SueLlrCanTxLimitsTestCase`) |
| S8(后续) | workload 对接 | astra-sim workload 接口 | 后续 |

每阶段:单测 + 计数器输出 + 回归基线。**TestSuite**:`src/sue/test/sue-test.cc` 61 例(`./run-sue-tests.sh`,含 S4 特性镜像与门面/PFC 共享池/规则 3/9 单测 + §12.10 的 LLR INIT 握手专项 + §12.6 的 LLR 两相 mode/三行为专项 + §12.15 的 preamble/vld/INIT_ECHO/ACK 周期专项 + §12.16 的 acked_seq 初始化专项 + §12.18 的 pcs_lost / LLR_status / replay_ct=255 专项 + §6.10 的 S7 三门限/`LLR_REPLAY_BUF_MAX` 专项 + §6.12 的 SueLiteEndpoint 生成器专项(shim-vlan VC/预留 stub/`ENABLE_SCALEUP_WORKLOAD` 门) + §12.27 的 100G 默认配置端到端专项(`SueDefaultConfig100GTestCase`:定时器 0=自动折算,单帧收 1、tx_replay=0、无 discard) + §7 的 VC 映射专项(`SueVcMapConfigTestCase`:默认直映 0/3/5/8→0/12→4/63→7 + 部分覆盖 `0:7,63:0,5:5` + 端到端 `5:7` 交换机 `GetCntVcRx(7)` + `3:7` 端点 `GetRxByVc(7)`;每场景恢复默认防串扰) + §12.31 评审修复专项(TTL 丢弃信用退还口径 / LostCredits<0 防护 / FLUSH 不被迟到 ACK 拉起 / PFC pause 不阻重放 / 部分 ACK 唤醒 / 引擎驱动 LLR 窗口) + §12.32 的 LLDP RTS/RTR 握手专项(头 round-trip / 握手完成前门控 0 送达 / 非对称 want / no-peer 兜底 / active 门单测 + 采纳不一致告警观测 + want 动态撤销) + A/B 类补齐专项(§5.6 CC_Update MACDA/CID 可配)(§12.2 CF_Update 补偿与节奏 / §12.35 TX 毒化;LLDP LLR 能力四象限;CFG-VC DEI 行选为 `SueVcMapConfigTestCase` 场景 5) + §12.38 的 2^20 seq 回绕专项(`SueLlrSeqWrapTestCase`:全空间 2^20+100 帧零 NACK/零丢弃) + §12.40 的 VRF 分段专项(`SueVrfTestCase`:分段装配/DSCP>VLAN>port 优先级阶梯/未编程表 miss 计数) + §12.42 的延迟退款 preamble 不变性专项(`SueDeferredRefundPreambleTestCase`:空闲出口 SwitchSend 同步前插 8B LLR preamble in-place → 退款按 SwitchSend 前帧长,S_CF==S_CC/CU=0))。

---

## 12. 决策记录(开放问题闭环)

> S0–S4 核对出的"文档/实现"分歧逐项决策并闭环(§5.5–§5.7、§6、§9、§11 实现注已同步)。状态三态:
> - **✅ 已实现**——本轮已改代码并验证;
> - **⏭ 简化/降级**——决策为保持现状或降级,已记录原因,移出本轮承诺;
> - **🅂 留 S5/S7**——明确迁移点,不再作为待决策项。

1. **PFC 默认值分歧(§9 表 vs 代码)** → **✅ 改表不改代码**。§9 表已更新为代码默认:共享池开(`PFC_SHARED_POOL_SIZE 32768`/`HEADROOM 8192`/`RESUME_OFFSET 4096`),简单模式阈值 `32768/16384`;修掉表内"0=关"矛盾(`DYNAMIC_THRESHOLD=1` 与共享池开联动)。

2. **CF_Update 无 CF_min/max 定时器门控(§5.6)** → **✅ 实现 UEC §5.2.6.1 双触发(推翻早前"记为简化")**。恢复 `CBFC_CF_MIN_TIMER_BYTES`(=125000,10µs@100Gbps)/`CBFC_CF_MAX_TIMER_BYTES`(=10×CF_min=1250000)键,`CalculateBytesTxTime` 折算为线时间:触发 (a) `R_VC_CF` 更新(OnDrain→OnCfDirty 置脏位)且 **CF_min one-shot** 到期 → `SendCfUpdateBatch` 批量上报脏 VC;触发 (b) **CF_max 周期定时器**到期 → 全量上报(防丢失,自 rearm + Cancel 未决 one-shot)。两键均 0 保留逐帧直发旧行为(测试/非配置设备)。2026-08-13 增数据量触发键 `CBFC_CF_THRESHOLD_BYTES`(SDK credit_threshold 对等物):OnDrain 累计释放字节 ≥ 阈值即上报(余量结转 = floor(总字节/阈值) 次;CF_min>0 时走同一地板网格,CF_min=0 时立即批量发送)——CF 计数 ∝ 数据字节、与端口速率无关(§12.36;TS-54 场景 4)。2026-08-14 多量子修复:单 drain 跨多个阈值量子(帧长 ≥ 阈值)逐量子各发一条、dirty 循环外清零(原单 if 大帧下速率塌到帧率,§12.37);预置阈值 2075 对齐 THU 默认档(§12.37)。one-shot fire 时刻取**绝对** `max(Now, m_lastCfUpdate+CF_min)` 再 `arm-Now` 相对调度,与"哪个 drain 触发 arm"无关 → fire 时刻/发送计数确定性,不依赖线程数(S8 NACK/FLUSH 双模式 MTP EQUIVALENCE PASS 佐证,§10.5)。

3. **IFG 硬编码 96ns(§5.4/§9)** → **✅ 按速率折算**。`SUE_IFG_BYTES`(默认 12)注册;`ApplySueConfig` 在 `dev->IsDataRateSet()` 时 `SetIfg (rate.CalculateBytesTxTime (ifgBytes))`;`SetupSueNetwork` 不再硬编码 96ns。

4. **SueConfig 无首次使用缓存(§9)** → **✅ 加缓存**。`EthType0()/EthType1()/HasShim()` 首次使用时缓存(file-static `std::optional`,懒填充),`SueReadConf`/`Config::SetGlobal` 后 `InvalidateCache()` 失效;非逐帧实时 `GetValue`。

5. **err_rate 解析后未生效(§6.8/§9)** → **✅ 写回 + 修注释;🅂 随机注入留 S8(workload 阶段,2026-08-13 评审改挂,原"留 S5"已闭环悬空)**。`SetupSueNetwork` 解析 `err_rate` 后 `Config::SetGlobal (LINK_ERROR_RATE_PER_LINK)` 写回;头文件注释已修。按概率随机 FCS 损坏注入**已实现**(PLAT-SP-08,2026-08-13):拓扑 err_rate 驱动概率模型;确定性 corruptor 集合保留供场景测试(§10.1)。

6. **LLR_ENABLE 单键不符 UEC(§6.2/§9)** → **✅ 对齐 UEC §5.1.4,彻底移除 `LLR_ENABLE`**。单键拆为两相 `LLR_MODE_LOCAL`(RX 可收 LLR_INIT/过滤)+ `LLR_MODE_REMOTE`(TX 可发 LLR_INIT/分配 seq+replay)+ 三行为 `LLR_INIT_BEHAVIOR`/`LLR_FLUSH_BEHAVIOR`(DISCARD|BLOCK|BEST_EFFORT,默认 BLOCK)+ `LLR_RE_INIT_ON_DISCARD`(默认 1)。默认(两相开 + BLOCK + 自动重入)精确复刻原 `LLR_ENABLE=1` 行为,**27 个既有 TestCase 零回归**。旧 conf 中残留的 `LLR_ENABLE` 行作未知键静默跳过(镜像 baseline)。引擎是否存在仍以拓扑 `llr_ena` 为准;模式/行为经 `ApplySueConfig` 落引擎(§6.8/§9)。

7. **PFC_ENABLE 死键** → **✅ 移除**。PFC 开关唯一事实源为 `FC_MODE=PFC`;`PFC_ENABLE` 已从 §9 表与 `g_keys[]` 删除。(`CBFC_CF_MIN/MAX_TIMER_BYTES` 于 §12.2 随 UEC 双触发模型恢复,不再随本决策删除。)

8. **VC 映射表可配置未实现(§7)** → **✅ 已实现(通用流类别映射端到端落地)**。`ComputeVc` 解码统一查 SueConfig 映射表(`DSCP_TO_VC_MAP`/`PCP_TO_VC_MAP`/`DEFAULT_VC`),host 发送/接收与交换机同一张表;默认直映(逐项 `DSCP&0x07`/PCP 恒等/VC0)与旧固化逻辑逐字节等价,零回归。2026-08-13 DEI 扩展:`PCP_TO_VC_MAP` 8→16 项(`(dei<<3|pcp)` 寻址,DEI 行独立),端点 `SetDei`/线头 DEI 全程贯通(CSV ULL-CBFC-CFG-VC-003 ⚠️→✅)。验证:s5e 多 VC/非恒等映射(§11 S5e,`GetCntVcRx` 从线头解出同一 VC)+ 新 TestCase `SueVcMapConfigTestCase`(§7/§11,场景 5 DEI 行选)。

9. **计数器保真(§6.11)** → **✅ 补齐/改语义/标注**。`rx_replay`: `RxFrame` 在 `m_nackSent` 时接受期望帧即 `++rx_replay`(GOOD/POISONED 两分支);`tx_replay` 为 **REPLAY 状态退出(完成重放)计数**(UEC Table 5-13 原文;初版曾误按逐帧计数并声称"与 UEC 对齐",2026-08-13 评审修正见 §12.31 A7,逐帧体积移入 debug 计数 `tx_replay_frames`);`tx_poisoned` **已实现**(2026-08-13 TX 毒化注入:`SetPoisonTxOnce/Every` → 帧挂 `SueFrameBadTag` → 对端 POISONED 接受不 NACK,§12.35)。

10. **无 INIT_ECHO 专项验证(§6.2)** → **✅ 补专项 TestCase**。新增 `SueLlrInitHandshakeTestCase`:场景1 INIT/INIT_ECHO 互达 → 四计数(init/init_echo)各 1、`a==ADVANCE`、`b==OFF`;场景2 无对端引擎 → INIT 超时×retryMax 后 `GoAdvance`,INIT 发送计数=retryMax+1。TestSuite 26→**27** 例(§11)。

11. **控制队列无子优先级、无 BE VC(§5.7)** → **⏭ 标注简化,不做**。egress 队列单 FIFO、控制帧经 `TransmitControl` 绕队列,为有意简化;子优先级/独立控制队列建模不做。(Task B 复核:CF_Update 最低优先级、纯 FIFO 排 ACK/NACK 后,被 ACK 流积压是**预期规格行为**(UEC Table 5-12),非缺陷;§12.26 解耦后 ACK 不再独占 pacing FIFO,CF 响应恢复,400B 门对 CF_Update 的线速上限仍按规格成立。)

12. **CF count 11b vs UEC 15b(§5.6)** → **✅ 扩到 15b**。`SueCbfcCtlOsHeader` 改 15b(与引擎 `CF_MOD=2^15` 一致);11b 在 ≥2048 静默截断已消除。8B 载荷布局(5b VC + 15b count)×2 已落地,2047/2048/8191/32767 往返验证 PASS。

13. **列而未实现项(§6.10/§9)** → **⏭ 三态标注,移出承诺**。按"✅ 已实现 / ⏭ 预留后续 / — 不做"标注:
    - **预留后续**(标注在 §9 表/正文):`CBFC_NUM_VCS`/`CBFC_TOTAL_CREDITS`(固定 8 VC)、多线程 `NS3_MTP`(✅ **§12.19 已闭环**)、`SueLiteEndpoint` 对接外部 workload(S7,§6.12;内置生成器已实现)、helper/topology/mmu。(`LLR_PCS_LOST_STATUS_TIMER_MAX_NS` 已随 §12.18 落地;`LLR_INIT_BEHAVIOR`/`LLR_FLUSH_BEHAVIOR`/`LLR_RE_INIT_ON_DISCARD` 已随 §12.6 落地移除;LLDP 协商已随 §12.32 落地移除。)
    - **不做**:`SUE_DEBUG_LEVEL`/`SUE_COUNTER_OUTPUT_FILE`(计数器经 `PrintCounters(FILE*)` 输出,§6.11)。
    以上不再列为待决策项。

14. **LLR 两相 mode/三行为无专项验证(§6.2/§9)** → **✅ 补专项 TestCase**。新增 `SueLlrModesBehaviorsTestCase`(§11 27→**28** 例):A `mode_remote=OFF` 不发 INIT 且 `GetTxBehavior()==BEST_EFFORT`;B `mode_local=OFF` 收 INIT 不回 INIT_ECHO、BAD 帧直通; C `llr_init_behavior` DISCARD/BLOCK/BEST_EFFORT(默认 BLOCK,`DiscardNew()` 计 tx_discard);D `re_init_on_discard` FALSE 停留 FLUSH(`tx_init_ctl_os==0`)vs TRUE 自动重入 INIT;E 经 `ApplySueConfig` 从 `LLR_MODE_*`/`LLR_*_BEHAVIOR`/`LLR_RE_INIT_ON_DISCARD` 落到引擎。scratch `sue-cfg-parse-apply` 同步断言新键读回与引擎落地。

15. **LLR 链路保真四修订(UEC 原文核对评审,§5.4/§6.2/§6.10)** → **✅ 一并落地**。① **ineligible preamble**(#1,必须):数据帧一律 8B preamble——`LLR_ACTIVE` 发 eligible(B1=0x7+seq),BEST_EFFORT/`mode_remote OFF` 发 **ineligible**(B1=0x55,`SetEligible(false,0)`);RX 有 `m_llr` 时先剥 8B 再按 `mode_local` 门控(ON→LLR 过滤 / OFF→直通)——修复 config-guide §3"关闭 TX 相"实验的解析错位。② **next_rx_seq_vld**(#3,建议):`OnInit` 置 TRUE、`FlushAndInit` 清 FALSE;FALSE 时 eligible 帧 DROP **不计数、不 NACK**(§5.1.7 filtered)。③ **INIT_ECHO 校验**(#4,可选):`init_seq==tx_seq` 才退出 INIT,错 seq 忽略 + 告警但仍计 `rx_init_echo_ctl_os`(§5.1.9.1.2)。④ **ACK 周期**(UEC §5.1.6):`SetAckPeriod`(默认 0=per-frame,**零回归**);`ApplySueConfig` 将 `LLR_CTLOS_TARGET_SPACING` 钳制到 [400,16384](Table 5-9)后按数据率折算为引擎 ACK 周期——每周期至多一条**累积 ACK**(`next_rx_seq-1`)、有推进才发,NACK 仍即时单发;设备 CtlOS 门同界(双保险)。⑤ **真握手入口**:`MakeDeviceOn` 改 wire-then-Start(`ApplySueConfig` 落 config 后才 `Start()`,§6.8),标准流 INIT 握手真实执行,`LLR_MODE_REMOTE 0` 真抑制 INIT。TestSuite 28→**32** 例(新增 vld gating / INIT_ECHO match / ineligible preamble / ACK 周期 4 用例);全 scratch 重建 PASS。#5(acked_seq 初始化,UEC Table 5-10)单独评估,已闭环于 #16。

16. **acked_seq 初始化(UEC Table 5-10,§6.2)** → **✅ 已实现**。20 位寄存器三处初值对齐 UEC:`m_ackedSeq` **初值全 1**(`SEQ_MOD-1`);`SetModeRemote` 在 llr_mode_remote **改变时**重置全 1;`StartInit`/`OnInitTimeout` 每次发 LLR_INIT 置 `init_seq-1`(init_seq = tx_seq%2^20)。副作用分析:全 1 / init_seq-1 初值根治"首帧 seq=0 与 ackedSeq=0 碰撞",**删 `m_anyAcked`**——首 ACK(0) 正常处理,真重复 ACK(`seq==ackedSeq` 且非 NACK)仍跳过。**公开观察口默认值改动**(`GetAckedSeq()` 0→1048575)仅影响未收任何 ACK 的观测点,scratch 均断言处理后的终值(engine==2 / wake==1),零回归。`OutstandingSeq()` 改 **replay buffer 长度**(acked_seq 经重置非可靠窗口锚点;option b)。行为改善:接收端尚未收好帧时(`next_rx_seq=0`)首帧坏→NACK(0-1 mod = 全 1),旧代码因 `(1048575-0) mod 2^20 > 2^19` 被判超范围忽略、依赖 replay_timer 兜底;新代码全 1 初值使其合法、立即触发 REPLAY 且不释放(正确语义:全部重传),终态不变。TestSuite 32→**33** 例(新增 `SueLlrAckedSeqTestCase`:INIT init_seq-1 / mode 改变全 1 / 首 ACK 非 dup / dup 跳过);全 scratch 重建 PASS。

17. **`LLR_CTLOS_MIN_INTERVAL_NS` 键删除(§5.7/§9)** → **✅ 移除(其双语义耦合后经 §12.26 解耦)**。`LLR_CTLOS_TARGET_SPACING`(钳制 [400,16384] → 按数据率折算)曾同时承担设备 CtlOS 最小间隔门与引擎 ACK 周期,旧的直接给 ns 间隔的 `LLR_CTLOS_MIN_INTERVAL_NS` 不再有独立用例——GlobalValue 注册、`g_keys[]` 解析项、`ApplySueConfig` 中 `dev->SetCtlOsMinInterval(NanoSeconds(...))` 一行、conf 默认值行全部删除。设备 `SetCtlOsMinInterval(Time)` 保留(被 `SetCtlOsTargetSpacing` 内部调用 + 引擎级测试直接使用),仅配置入口收敛到 `LLR_CTLOS_TARGET_SPACING`。旧 conf 该行作未知键静默跳过(镜像 baseline 行为)。

18. **UEC Table 5-10 六项核对落地(§6.2/§6.10/§9)** → **✅ 一并实现**。对 §12.16/§12.17 之外的 UEC Table 5-10/5-11 变量逐项核对:① **tx_seq 清零**(#2):`SetModeRemote` 在 llr_mode_remote 改变时 `m_txSeq=0`(原只清 acked_seq);连带清空 replay 缓冲、归零 replay_ct、取消 replay/data-age/pcs-lost 定时器,保证 ③ 的 outstanding_seq 公式与缓冲一致。② **LLR_status 查询接口**(#3):`GetLlrStatus()`——mode_remote ON 且 ADVANCE/REPLAY 为 TRUE;mode_remote OFF 时跟踪 PCS_status(直通),设备 `GetLlrStatus()` 透传。③ **outstanding_seq 语义修正**(#4):由 §12.16 的"replay buffer 长度"回退为 **UEC 原式 `((tx_seq-1) - acked_seq) mod 2^20`**(序列空间量,非缓冲长度);因 ① tx_seq 清零 + acked_seq 重置,公式在全部状态下与缓冲大小一致(§6.2 注:早前 §12.16 的 option b 作废)。④ **ACK 触发源对齐 init_echo_sent**(#5):ACK/NACK 机改为**以首个 LLR_INIT_ECHO 发出(init_echo_sent)为进入 SEND_ACKS 的触发源**(UEC §5.1.6),`OnInit` 时 arm 一次、`OnAckTimer` 自转——每周期一条**累积 ACK**(`next_rx_seq-1`),空闲周期重复末 seq(发送端 dup 跳过),`m_nackSent` 期间抑制;删除 `m_pendingAck` 逐帧请求逻辑。⑤ **replay_ct 语义对齐**(#6):`replay_ct_is_max = replay_ct >= replay_ct_max`(预先检查,先判后增),`replay_ct_max` 全 1(255)时**永不触发** FLUSH——仅 data_age / pcs_lost 可触发;可观测 replay 轮数与 UEC 一致。⑥ **pcs_lost_status_timer 仿真方案**(#1):**最小 PCS 模型**(无 PCS 子层)——设备 `SetPcsStatus(bool)` 由场景驱动(scratch 用 `Simulator::Schedule` 模拟闪断),PCS FALSE 时设备丢弃入帧(闪断期间不可解码 → 对端 replay 缓冲增长),引擎 `pcs_lost_status_timer` 仅在 ADVANCE/REPLAY 且 PCS FALSE 时运行(OFF/INIT/FLUSH 停止),超时(即使缓冲为空)**FLUSH + INIT 重同步**,过期标志锁存至 PCS 恢复;`pcs_lost_status_timer_max=0`=立即过期(UEC),配非 0 阈值可区分短/长闪断。新键 `LLR_PCS_LOST_STATUS_TIMER_MAX_NS`(默认 0)注册 + `ApplySueConfig` 落地。TestSuite 33→**36** 例(新增 `SueLlrPcsLostTestCase` / `SueLlrStatusTestCase` / `SueLlrReplayCtUnlimitedTestCase`);新增 scratch `sue-llr-pcs-lost`(短闪断 replay 恢复 / 长闪断 FLUSH+re-INIT / 空闲链路空缓冲 FLUSH 三场景)。⑦ **过时注释清理**(#7):头文件覆盖注释、`OutstandingSeq` 注释、ACK 周期文档同步改写。

19. **单进程多线程(S5a,§10.5)** → **✅ 已实现(上下文路由方案)**。fork `NS3_MTP` 自动分区 + 节点上下文路由,不加全局锁。关键决策与结论:
    - **临界区策略 = 上下文路由,不加锁**(用户决策):auto-partition 下节点状态单 LP 独占,`explicitCriticalSection` 全局单锁会串行化所有 LP → 否决,锁/MPI 留后续。
    - **根因 bug 已修**:`SueNetDevice::GetChannel()` 覆写返回子类 `m_channel`(基类非虚返回基类私有成员,Attach 不设置 → MTP 分区器见不到链路 → lookahead=`Time::Max()/2` → evPast=16、delivered=0)。修复后 lookahead=0ns、evPast=0、delivered=1600。
    - **上下文路由落地**:`SueLlrEngine::ScheduleCtx`(9 处定时器)+ `SueNetDevice::ScheduleSelf`(7 处),两模板透传普通 `Schedule`,路由靠 **LP 驻留**(数据平面经 channel context 接缝在节点 LP 执行;§10.5);`SetContext` 在 `SetLlrEngine` 注入;`Start()`/`StartCcUpdateTimer` 两处 setup 期定时器 MTP 下 defer 到节点 LP(`StartOnLp`/`ArmCcUpdateTimerOnLp`,t==0 context 事件由 `Partition()` re-home)。非 MTP 下全部零行为变化 → 37 TestCase + 23 scratch 零回归。
    - **构建**:`build.sh` 并入 `-DNS3_MTP=ON`;`run-sue-tests.sh` + `src/point-to-point/CMakeLists.txt` 补 MTP 库链接;sue 模块零 mtp 依赖(`sue-mtp.h` 仅按 TypeId 名检测)。
    - **等价性**:`sue-mtp-equivalence.cc` baseline vs MTP-8 聚合结果 hash 逐字节一致;delivered=1600/1600、无 discard/FLUSH;12 LP 自动分区。`run-s8-mtp.sh` 双跑 diff hash。
    - **验证期发现**:ackPeriod==CtlOS pacing slot 耦合 → CF_UPDATE 排 ACK 前 → ACK 返回延迟 → replay/dataAge 风暴 FLUSH(场景级,§10.5 详述;模型行为符合设计耦合)。
    - **驻留不变量可执行化(§10.5 收尾清单 ①)**:`sue-mtp.h` 新增 `SueMtpResidencyCheck(nodeId)` + `SueMtpCurrentLp()`。`SimulatorImpl::GetSystemId()` 为纯虚、MTP 实现返回当前线程 LP(`MtpInterface::GetSystem()->GetSystemId()`),故断言 MTP 下校验 `当前 LP == NodeList::GetNode(nodeId)->GetSystemId()`;`ScheduleCtx`/`ScheduleSelf` 每个 arm 前调用,并断言 MTP 下引擎/设备必有节点(否则定时器落公共 LP、`Cancel` 失效)。负测试:去掉 s8 `SendOne` 的节点 context 后 MTP-8 首 arm 即 `NS_ASSERT_MSG` abort(`LP 0 vs node 0→LP 1`),证明断言有效;非 MTP 短路,37 TestCase + 23 scratch + S8 双跑零行为变化。
    - 新键 `SUE_MTP_THREADS`(默认 1)。S5 拆为 S5a(适配,✅)/S5b(压测,`SueLiteEndpoint` 前置阻断项已闭环 §6.12/§12.21–§12.23)。

20. **S8 等价性刀锋 + 结构场景碰撞(§10.5,与 §12.2 联动)** → **✅ 场景级修复(非模型缺陷)**。恢复 CF_min/CF_max 后 FLUSH 模式先绿,但 **NACK 模式 rx_ack 出现 197 vs 196(threads=1 vs 8)**。全链路定位为**结构场景碰撞 + 同时间戳排序差异**的复合,而非模型 bug:
    - **根因链**:`1000us = 50×20us` → host h 的 frame#(k+50) 与 host (h+1) 的 frame#k **同一时间戳共到达** ToR0 egress(host0#50 与 host1#0 同在 1000864,不同 ingress d=0/1、同一 egress d=4)。Default(同时间戳 FIFO/插入序)vs MTP(本地 stage-1 先于跨 LP stage-3 消息,uid/LP 序)的**入队顺序互换** → ToR0 egress 线上槽分配互换(1001728/1002688)→ ToR9 egress 分配互换(d=0↔d=1)→ host4 到达相位漂移 ~960ns(=data wire 864+IFG 96,第二个出队帧排首个之后)→ host4 ACK 采集**刀锋翻转** → rx_ack 197 vs 196。**wire 报文多集字节一致,仅"内容→槽位"分配随线程数互换**——即同时间戳跨 LP 事件在两种模拟器下的全序不同,任何对顺序敏感的行为都会分歧。(本节相位数值为 1G 历史实测;机制与速率无关,100G 重测见 §12.36。)
    - **修复(dither)**:每 host 起始偏移加**互不相同且非 20us 倍数**的子间隔偏移(0/137/274/411/548/685/822/959ns)→ 所有帧出发时刻全局唯一 → 两模拟器共享**单一总事件序** → 无论 ACK 刀锋落哪,哈希逐字节一致。NACK 双模式 `MTP EQUIVALENCE PASS`;FLUSH 模式等价判据经 §12.28 修正为行为 hash(掩 ACK/NACK CtlOS 层)后,双模式现均 `MTP EQUIVALENCE PASS`。
    - **次级现象(良性)**:host0 frame#0 的 TX 时刻 16552 vs 16456(96ns 偏移)——16456 处两个同时间戳事件:host0 本地 `TransmitComplete`(其 paced INIT_ECHO wire 16384-16456 的完成)vs host4 跨 LP INIT_ECHO 到达(均 pacing 到 16384)。MTP 本地优先 → TransmitComplete 先 → ADVANCE-after → 数据 16456 出发;Default FIFO → INIT_ECHO 先 → ADVANCE 见 m_busy → 等 IFG → 16552。仅 frame#0,frame#1+ 重新对齐;计数非时间敏感,已验证对 rx_ack 无影响。
    - **约束(写入 §10.5 收尾清单 ③)**:任何**依赖同时间戳跨 LP 事件顺序**的等价性场景,须保证出发时刻全局唯一(如 dither),否则 Default vs MTP 的 FIFO/LP 序差异会让"内容→槽位"分配互换。CF_min one-shot 的绝对 fire 时刻(`max(Now, last+CF_min)` + `arm-Now`)与线程数无关,天然规避该刀锋(§5.6/§12.2)。

21. **SueLiteEndpoint 基类选 `Object` 非 `Application`(S5b,§6.12)** → **✅ 已实现**。`Application::DoInitialize`(application.cc)用普通 `Schedule` 调度 `StartApplication` → 定时器落**公共 LP**,MTP 下违驻留(§10.5);SueLiteEndpoint 继承 `Object`,发包首发走 `ScheduleWithContext(nodeId, ...)`(Partition 重挂到节点 LP)、自链式普通 `Schedule` 继承该 LP,`SendFrame` 每 arm 前 `SueMtpResidencyCheck(nodeId)`(非 MTP 短路零开销)。`sue-endpoint-ring`/`run-s5b.sh` 四组合(ring/alltoall × VC0/VC3)baseline vs MTP-8 聚合结果 hash 逐字节一致佐证。

22. **共享 `SueMacFromXpuId`/`SueGetHostNodeId`(S5b,§6.12)** → **✅ 已实现**。各 scratch 重复的 static `MacFromXpuId` 提升为 sue-header.h 自由函数 `SueMacFromXpuId`(`SueHeader::GetXpuId` 的逐字节逆,新代码一律用共享版;既有 scratch 的 static 版共存无害);`SueEntry` 新增 `SueGetHostNodeId(hostIdx)`(返回 `g_hostIds[hostIdx]`,越界 0),`SueInstallLiteEndpoints` 据此把端点绑到各 host 设备(替换 `HostDefaultRx` 收帧 upcall)。

23. **`ENABLE_SCALEUP_WORKLOAD` 注册与 `=1` 兼容性(S5b,§6.12)** → **✅ 已实现**。键从"表中列出但未注册"(解析静默跳过)改为**注册 GlobalValue**(默认 0)+ 进 `g_keys[]`;`SueLiteEndpoint::Start()` 读之,`=1` 时 `NS_LOG_WARN` 并**回退内置生成器**(外部 astra-sim workload 未接线,`sim_send/recv/schedule` 预留 stub 返回 -1)。兼容性:默认 0 行为不变;旧 conf 该行由静默跳过变解析写 0,无功能差异;TestCase 断言 `=1` 时帧仍照发(非 inert)。

24. **LLR replay_timer 规则 4 重置缺失(模型缺陷,§6.2;Task B 定位)** → **✅ 已修复**。UEC §5.1.4 语义核对(§5.1.4 定时器定义 + L305/397):replay_timer 是 **ACK 超时检测器**——新/重发帧 TX 时 arm(规则 2/3)、ACK 释放缓冲帧时**重置为 0**、outstanding_seq 仍非 0 时重新激活(规则 4)、outstanding_seq==0 保持 0(规则 1)、到期 → REPLAY(规则 6);**data_age_timer 是 FLUSH 判决**(丢弃过老数据),不是 ACK 超时判决。模型此前只在缓冲**全空**(GoAdvance)时取消定时器,ACK 释放**部分**缓冲帧时不重置 → ACK RTT ≈ replay_timer_max 时定时器穿越在途窗口到期 → 无谓重传在途帧。修复:`OnAckNack` 在 ACK 释放部分缓冲帧(缓冲非空)时 Cancel+重新 arm(规则 4)。replay_timer_max 配置应随 RTT 关联(§12.25),否则 ACK RTT≈timer_max 处该缺陷放大。

25. **1G 无法线速根因 = replay_timer 配置;CF/64B 门假设推翻(§5.7/§9/§11 Task B)** → **✅ 定位 + 配置级修复**(2026-08-13 起最小端口速率 100G,本节 1G 实测为历史实验数据,§12.36)。固定 50µs replay timer ≈ 1G ACK RTT(35–51µs:2×wire + 2µs 传播 + ackPeriod 网格)→ **84% 帧被重传**,重传占带宽 → 0.733–0.757 Gbps(实测);400G 不受影响(ACK RTT ~124ns ≪ 50µs,缓冲在两 ACK 间排空 → GoAdvance 取消定时器)。**配置级修复(replay 定时器与 RTT 关联)**:`LLR_REPLAY_TIMER_MAX_NS` 按 **10× LLR RTT** 自算(`2×wire + 2×prop + ackPeriod`),`sue-thpt-line-rate` 缺省即此(THPT_REPLAY_NS=0 自动);§12.24 模型修复落地后固定 50µs@1G 亦恢复线速(0.974 Gbps,replay=0)。**用户 64B pacing 假设测试为负**:cfmin=400/1000 与 64B 门探针均无变化(0.733–0.762,replay ~86%)——400B 门远不绑定(CtlOS 需求 ~62k/s vs 门容量 ~312k/s),**1G 非线速与 CF 节流无关**;"pacing 粒度调小到 64B 可提 CF 响应、解决 1G"不成立,且 64B 门使 CtlOS 占带宽 12.5%(超 §5.2.11.2 的 2% 上限 6×),维持 400B floor。**CF_min 的 UEC 语义 = ≥8B 线时**(§5.2.6.1,非 0);按 400B 配置时低优先级 CF_Update 确实被 400B 门限速——**预期规格行为**(CF_Update 最低优先级 + 门限),不是缺陷。loss 1/10000 下 LLR NACK-replay 完整恢复(重传计数 = NACK 计数),吞吐损失 <0.02%。**规范矩阵在 CREDITS=1024 测得(信用窗口不绑定)**;`THPT_CREDITS=512` 时信用窗口成为绑定约束(400G credit_exhausted≈9.2 万、两速率均 ~91.75% 线速)——CBFC 小信用窗口的机制行为,非缺陷(信用窗口分析用 `THPT_CREDITS` 扫描,§12.26 后 `run-s5c-thpt.sh` 默认 1024)。

26. **pacing-gate 与 ACK 周期解耦(§5.7/§9;Task B)** → **✅ 已实现**。`LLR_CTLOS_TARGET_SPACING` 原同时驱动设备 CtlOS pacing 门(`SetCtlOsTargetSpacing`→`m_ctlOsMinInterval`)与引擎 ACK 周期(`SetAckPeriod(CalculateBytesTxTime(spacing))`,§12.17/§10.5 耦合)。解耦:门**固定** UEC §5.2.11.2 **400B floor**(`CTLOS_MIN_SPACING_BYTES`,CtlOS<2% 带宽的硬下限),ACK 周期单独由 `SetAckPeriod(CalculateBytesTxTime(spacing))` 驱动、spacing 钳制 [400,16384]。理由:① 门语义 = "任意两条 CtlOS ≥400B",绑到 ACK 节奏会让**累积 ACK 流单独填满 pacing FIFO**、低优先级 CF_UPDATE 被 ACK 积压(§10.5 早期发现);② 门(线速上限)与 ACK 节奏(报文周期)是**两个独立语义**,共享一键造成耦合。后果:`LLR_CTLOS_TARGET_SPACING` 现**仅指 ACK 周期**;门恒 400B floor。s5c 验证:1G clean/loss 均 0.974 Gbps 线速(§12.25),400G 391.7 Gbps,38 TestCase + 双速率 4 场景全 PASS。

27. **LLR replay/init 定时器默认改 0=自动折算(s6-entry 1G 回归,§5.1.4/§9/§11 S6)** → **✅ 已实现**。用户回报 s6-entry 默认配置(1G)下 host2 收到 2 帧,二分定位到 97c9b96(Task B 模型修复);根因 = **静态定时器默认 < 1G ACK 往返**。`LLR_REPLAY_TIMER_MAX_NS`/`LLR_INIT_TIMER_MAX_NS` 默认原为 1000ns,而 1G 下 CtlOS pacing 门 **400B = 3.2µs**(§5.2.11.2,§12.26)——INIT 握手 + ACK 的 CtlOS 往返 ≥ 门 ≥ 1µs → replay_timer 在 ACK 到达前到期 → **每 1µs 无谓重传** → `replay_ct_max=5` 耗尽 FLUSH → 丢弃缓冲帧;FLUSH 后 INIT 重同步又撞 1000ns init 超时 → 级联重试 → host2 收到重复帧(g_rx 1→2)。**修复(取方案 b,非 a)**:
    - **0 = 自动折算 sentinel**:两键默认改 `0`,`ApplySueConfig` 在 `dev->IsDataRateSet()` 块内(after ACK 周期已算)自动折算 `timer = 10 × LLR RTT`(`RTT = 2×wireCycle + 2×prop(1µs) + ackPeriod`,`wireCycle = rate.CalculateBytesTxTime(108) + rate.CalculateBytesTxTime(12)`,§12.25 公式)— 随链路速率缩放,不再有"静态默认"命题。显式 `>0` 仍固定。
    - **方案 a(5000ns)被实证否决**:静态 5000ns 能修 s6-entry 单帧(门 3.2µs < 5µs),但 1G 负载下仍 < ACK 周期(2048B = 16.4µs) → 持续重传,吞吐崩到 **0.460 Gbps 且真丢帧**(rx=561821 < sent=610352,s5c 1G loss 实测);自动折算(1G ≈203µs,公式 §12.25 = 203040ns)→ **0.974 Gbps 全量送达**。⇒ 单一静态默认无法同时服务 400G(5µs 够)与 1G(需 ~203µs),必须随速率派生。
    - **引擎无配置 floor**:不经过 `ApplySueConfig` 的引擎(手搓 scratch/TestCase)静态默认 1000→**5000ns**,须 > 1G CtlOS 门(400B=3.2µs)。这些引擎无 ACK 周期/速率上下文,floor 仅兜底,配置路径一律自动折算。
    - **预置按计算后推荐值直写**:`conf/sue-default.conf` 与 `sue-100g|200g|400g.conf` 两键改为**计算后的推荐值**并附公式注释(原 5000):`sue-default.conf` 203040ns@1G、100G 21830ns、200G 20915ns、400G 20458ns(= 10× RTT,`RTT = 2×wire + 2×prop(1µs) + ackPeriod`,各 conf 键注释注明公式与原理;仍保留 0=自动折算语义,效果等同)。折算值 ≈ 203µs@1G / 22µs@100G / 21µs@200G / 20µs@400G(比固定 5000 更保守,覆盖 ACK 周期与 2µs prop)。(2026-08-13:1G 档已随 §12.36 移除,默认档即 100G 21830ns。)
    - **回归护栏**:新增 **`SueDefaultConfig1GTestCase`**(ctest 38→**39** 例):两节点 1G 通道 + 双方 `ApplySueConfig(A)`(走默认 GlobalValue = 自动折算)→ 单帧 86B 端到端 → 断言 `g_rxCount==1`、A 引擎 ADVANCE、`tx_replay==0`、`tx_discard==0`——**默认配置 1G 端到端纳入 TestSuite**,杜绝该病理再溜进 ctest(此前 s6-entry 是 scratch,不在 TestSuite 内,故"38/38 全过"仍漏掉它)。
    - 全量验证:TestSuite **39/39 PASS**、s6-entry PASS、s5c 400G 391.743/391.69 Gbps、s5c 1G 0.97429/0.974183 Gbps、23 scratch PASS=23、S5b MTP EQUIVALENCE PASS、S8 NACK MTP EQUIVALENCE PASS、S8 FLUSH MTP EQUIVALENCE PASS(后经 §12.28 判定:FLUSH 模式 MTP 等价 FAIL 的根因是**同时间戳跨 LP 事件顺序决胜差异**(周期 ACK 定时器 vs 跨 LP 数据到达相撞)导致的 n8d4 `rx_ack` ±1,非模型缺陷;等价判据改为**行为 hash**(掩 ACK/NACK CtlOS 计数层),全帧/重传/丢弃/FLUSH/CBFC 计数双跑逐字节一致,双模式现均 PASS)。

28. **S8 FLUSH MTP 等价判据:行为 hash 掩 ACK/NACK CtlOS 层(§10.5 残留 ACK 时钟决胜,§12.20 延续)** → **✅ 用例级判据修复(非模型缺陷)**。§12.27 记录 FLUSH 模式 MTP 等价 FAIL(n8d4 L[10] `rx_ack` 328 vs 327),本轮全链路定位:
    - **非模型缺陷**:逐项排除——确定性(328/328/327/327,多次同配置同值);Stop 无关(15/20/25/30ms 不变);全 hash 唯一差异 = n8d4 `rx_ack_ctl_os` ±1(其余 21 项计数逐字节一致,含 delivered=1396/1396、bytes=139600、rx_ok/tx_ok/replays/discards/FLUSH/CBFC)。
    - **根因链**:spine10(节点 9)的累积 ACK 周期定时器在 `t=8126536000` 与一个跨 LP 数据帧到达**同时间戳相撞**。Default(FIFO 堆,同时间戳按插入序)先 ACK 定时器 → 该周期 ACK 带 seq=709;MTP(本地 stage-1 先于跨 LP stage-3 消息)先数据帧 → 该周期 ACK 带 seq=710 → 累积 ACK 流 seq 永久 +1。dither(§12.20)保证数据**出发**唯一,但**无法**让数据**到达**对本地周期 ACK 时钟唯一——同时间戳跨 LP 事件在两种模拟器下的全序本就不同(§12.20 已声明"任何对顺序敏感的行为都会分歧")。
    - **行为等价仍成立**:接收端 dup-skip 后双跑各收 915 条 ACK,仅"新 ACK/重复 ACK"分类 328/587 vs 327/588,终态 `ackedSeq=795` 双跑一致;全帧/重传/FLUSH/INIT 重同步等可观测行为逐字节一致。
    - **判据修复**:`sue-mtp-equivalence.cc` 的 `ResultHash(bool behavioral)` 掩掉 5 个顺序敏感计数(`tx_ack/tx_nack/rx_ack/rx_nack/rx_ack_nack_seq_error`)为 `x`,输出 `S8 HASH`(完整,诊断)与 `S8 BHASH`(行为 hash,等价判据);`run-s8-mtp.sh` 改 diff `S8 BHASH`。验证:NACK 双跑 BHASH 逐字节一致(其全 hash 本就一致),FLUSH 双跑 BHASH 逐字节一致 → **NACK 与 FLUSH 双模式均 `MTP EQUIVALENCE PASS`**;FLUSH 全 hash 仍仅 n8d4 `rx_ack` ±1 一处差异,作为诊断输出保留。模型 `src/sue/` 零改动(临时探针已全量回退,`git diff` 干净)。
29. **2:1 CBFC incast 观察暴露"出口→入口背压缺失"模型缺口(§6.7/§9/§11 S5e)** → **发现**。`sue-incast-2to1`(3 XPU、全 200G 口、host0+host1 双发 host2,2:1 incast)实测:
    - **发送端从未被真实节流**:CREDITS=1024 下两发送端各自持续 ~190G 线速 offer,出口 host2 聚合钳在 200G 线顶(~195.87 Gbps,非 380G 溢出——出口链路本身永不过速,`TransmitStart` 按线速串行化);**超额进交换机出口队列而非"溢出出口"**,无界出口队列 5ms 内吞 6.3 万帧再缓慢排空,`credit_exhausted=0`(窗口从未耗尽)。
    - **根因(三处耦合缺口)**:① `SueNetDevice::m_drainDelay=Seconds(0)` → `Receive()` 中 OnRx 后**同步** OnDrain,信用立即退还,与出口拥塞完全解耦;② `SueEgressQueue::Enqueue` 无界;③ 无 CBFC 出口/共享缓冲配置键。
    - **伪背压陷阱**:CREDITS=512 时 `credit_exhausted=31379`,看似"被节流"实为**信用窗口饥饿**(窗口满 → 发送端自然停发),并非出口拥塞驱动——任何出口拥塞场景都能被小窗口掩盖,计数不可作背压判据。
    - **期望行为(用户确认"这样肯定不对")**:2:1 超额应**在发送端/交换机入口侧缓冲**(发送端被出口拥塞真实节流到 ~100G 各、聚合仍 200G 线顶、无损),而非无界吞进交换机出口队列。
30. **CBFC 出口→入口背压耦合:实现、默认开(§6.7/§9/§11 S5e)** → **✅ 已实现**。修复链:出口 per-VC 有界缓冲 → 信用退还门控于实际排空 → 排空门控于出口吸收 → 出口队列满 → 交换机入口侧持有帧(不退还信用)→ CF_Update 上报 0 → 发送端被阻 → 真实 2:1 背压。
    - **新键 `CBFC_EGRESS_BUF_BYTES`**(uint32 B,**默认 0=auto**)。auto = 每个 CBFC 模式交换机端口按 VC 取 `creditLimit[vc]×creditSize`(conf 默认 512×256B=128KB/VC;1024×256=256KB/VC)→ 出口每 VC 至多一个信用窗;>0 = 显式统一 per-VC 上限。**只作用于 CBFC 模式交换机端口**(host 设备零变化,`ApplyCbfcBackpressure` 按 `SueFcController::GetMode()==CBFC` 筛)。
    - **机制**:`SUESwitchNode::Forward` 准入查 `egress->EgressCanAccept(vc,size)`(→ `SueEgressQueue::CanAccept` = per-VC 深度门)→ 可收则 `SwitchSend` + `ingress->RefundCredit`(释放入口信用,走 `OnDrain`,仍经 CF_min/CF_max 定时器门控,CF_Update 节奏不变);出口 VC 队列满则帧挂起 `m_pending[port][vc]`(FIFO,信用保持消耗),出口 `DequeueAndTransmit` 释放槽位后经 freed-hook 触发 `ServicePending` 补发 + 退还挂起帧自带的 ingress 信用。
    - **重入回避**:freed-hook 在 `DequeueAndTransmit` 末尾 `TransmitStart(p)` **之后**触发(m_busy=true)→ `ServicePending` 里的 `SwitchSend` 只入队不重入 `DequeueAndTransmit`,无递归。
    - **丢弃路径退还**:defer 下入口信用只由交换机侧显式退还 → lookup-miss/TTL-0 丢弃必须 `dev->RefundCredit(vc,size)`,否则信用泄漏、发送端窗口永不重开;非 defer 设备已同步 OnDrain,**退还必须门控 `GetDeferCreditRefund()`**(双重退还会虚增信用)。DISCARD 路径不触发 freed-hook(记录简化;incast 无损场景不进入 DISCARD)。
    - **MTP 驻留**:Forward 调度、ServicePending、RefundCredit 全部跑在交换机节点 LP,无跨 LP(驻留不变量保持,§10.5)。
    - **验证(S5e,配置驱动=走 SueEntryDispatch 全链路)**:`run-s5e-incast.sh` 默认 CREDITS=1024 → INCAST PASS,`credit_exhausted=188515`(发送端被出口拥塞真实节流,两源各 ~97.9G、50/50 分拆)、`pend=121817`/`pendMax=196`(交换机挂起确实发生)、`delivered==sent=122072` 无损、replay=0、discard=0、incast 窗 195.871 Gbps(200G 线顶);**每 200µs 瞬时速率曲线 `INCAST RATES`**(src0/src1 双入口 + egress 出口):t=200µs 爬坡 → 400µs–10.2ms 稳定 97.9/97.9/195.87 Gbps(瓶颈全程饱和)→ 10.4ms 排空尾 ~109G → 10.6ms 归零;CREDITS=512 亦 PASS(窗口饥饿 + 出口门**双背压并存**,`credit_exhausted=222700`、`pendMax=84`,183.5G=91.75% 窗顶)。零回归:39 TestCase 全 PASS、s5b/s5c/s8-mtp(NACK+FLUSH 双模式 hash 逐字节一致——s8 无 2:1 拥塞,门从不触发)全 PASS。
    - **TestSuite 不加用例(39 例不变)**:suite 全手工建 topo、不经 `SUESwitchNode` 转发路径(TestSuite 里交换机门本就不可达);背压验证由配置驱动的 s5e scratch 承担(与 s5c/s8 同模式)。(此决策后 VC 映射新增第 40 例 `SueVcMapConfigTestCase`,§7/§11。)

31. **六维度评审修复(2026-08-13,计划与状态见 `docs/review-fix-plan.md`)** → **A 阶段(几行级 P0)已落地**:
    - **A1 TTL==0 丢弃信用退还字节口径**:退还须用**含头完整帧**字节数(与计费侧 OnRx 口径一致,§12.30 不变量"退还==计费"),否则帧长跨信用边界时每丢一帧泄漏 1 credit(`sue-switch-node.cc`,RemoveHeader 前取 size;新 TestCase `SueTtlDropRefundTestCase`)。
    - **A2 OnCcUpdate 环回比较防护(UEC §5.2.4.2/§5.2.5.7.4)**:`lost = (cc-R_VC_CC) mod 2^20`,仅 `0 < lost < CC_MOD/2` 才回收;`lost ≥ CC_MOD/2`(LostCredits<0,严重错误)只重同步不污染 R_VC_CF,NS_LOG_WARN(新 TestCase `SueCcUpdateNegativeLostTestCase`)。
    - **A3 FLUSH 不被迟到 ACK/NACK 拉起(UEC §5.1.5)**:OnAckNack 缓冲空分支仅 ADVANCE/REPLAY 才 GoAdvance;INIT/FLUSH 态只更新 acked_seq/缓冲——否则 re_init_on_discard=0 下 FLUSH↔ADVANCE 振荡、绕过 INIT 重同步(新 TestCase `SueLlrFlushStaysFlushTestCase`)。
    - **A4/A5 MTP 数据竞争**:`g_hostRx` 改 `std::atomic<uint32_t>`(默认 host upcall 按节点 LP 并发);`SueReadConf` 预热 EtherType/*HAS_SHIM 首用缓存(原仅预热 VC map,首帧冷缓存并发写 std::optional 为形式 UB)。
    - **A6 PFC pause 不阻止 LLR 重放(UEC §5.1 明文)**:Retransmit/DrainRetx 删除 pause 门——pause 仅作用于新数据帧;重放被门会导致 pause+NACK 下 replay_ct 堆积 FLUSH 丢帧(新 TestCase `SueLlrReplayDuringPfcPauseTestCase`)。
    - **A7 tx_replay 改 UEC Table 5-13 语义**:由逐帧计数改为"REPLAY 状态退出(完成一次重放操作)"计数(GoAdvance 时 m_tx==REPLAY 计一次;FLUSH 退出不计)——修正 §6.11/§12.9 的"已对齐"错误论断(原论断与 UEC 原文相反)。
    - **A8 §9 默认值口径**:§9 表取值 = conf 预置(参考部署值);GlobalValue 注册默认:SUE_LINK_PROPAGATION_NS=0、SUE_SWITCH_FWD_BUDGET_NS=0、LLR_REPLAY/INIT_TIMER_MAX_NS=0(自动折算)、CBFC_CF_MIN/MAX_TIMER_BYTES=0(逐帧直发)。不经 conf 的 ApplySueConfig 路径以注册默认生效;conf 预置 10/300/21830/125000/1250000 仅参考部署值。
    - TestSuite 40 → **44 例**,全部 QUICK,与既有用例同构。
    - **B 阶段(设计级)已落地**:
      - **B1 部分 ACK 唤醒(停-走根除)**:引擎新增 `TxOpportunityCb`——ACK/NACK 部分释放窗口(缓冲非空)即回调设备重试 `DequeueAndTransmit`(不清 m_retxQueue,区别于 StateChangeCb)。此前唤醒源仅缓冲全空 GoAdvance / CF_Update 到达 / 新帧入队,`FC_MODE=OFF/PFC` 下吞吐按窗口批次停-走,末帧 ACK 丢失 + replay 定时器非绑定时无限期停滞(新 TestCase `SueLlrPartialAckWakeTestCase`:窗口 4 + 1ms 周期 ACK + 6 帧突发,1.05ms 即全送达)。
      - **B2 rules 3/9 真实窗口**:窗口旗标改由 **LLR 引擎状态驱动**——`RecomputeLlrWindow`:`(m_tx==REPLAY) || m_nackSent`,在 GoAdvance / OnAckNack(NACK)/ OnReplayTimer / StartInit / OnInit / FlushAndInit / RxFrame(nackSent 置位与清除四处)全部转移点推送 `SetLlrWindowCb` → 设备 → CBFC 引擎 `m_inLlrWindow`。删除 Retransmit/DrainRetx 同步包裹与 Receive DROP 死代码;CBFC `OnTx`/`OnRx` **撤窗**(规则 1/2 由结构保证:重传不调 OnTx、LLR 丢弃帧早返回;类型 B 帧窗口内仍计 R_VC_CC,UEC Table 5-26);规则 3(OnCcUpdate)与规则 9(SendCtlCcUpdate)沿用引擎旗标守卫,现覆盖真实窗口。新增 `GetRvcCc` 观测口(新 TestCase `SueLlrWindowDrivenTestCase`:窗口启闭力学 + 设备接线规则 9 + 规则 3 丢弃 + 类型 B 计数)。
      - **C7 自动折算 prop 脱钩**:`ApplySueConfig` 自动折算公式 `RTT = 2×wire + 2×prop + ackPeriod` 的 prop 项由硬编码 1µs 改为读 `SueConfig::LinkPropagation()`(默认 0=零传播基线)——硬编码下 prop>1µs 链路会重演 §12.27 的 FLUSH 级联,10ns 预置下则 44 倍过保守。conf 预置**显式值**保持原公式(保守 1µs prop,§9 注);(原 1G 预置 203040ns 超 UEC Table 5-9 上限 65535ns 的仿真放宽注已随 1G 档移除;100G 默认档 21830ns 合规,§12.36)。
    - TestSuite 44 → **46 例**,全 QUICK;计数口径同步 §4.1/§10.5/§11。
    - **D 阶段(P1 验证地基)已落地**:
      - **D1 s5c 吞吐护栏**:clean 模式断言零重放 + 速率 ≥ 95%×线速×payload 占比——§12.25/§12.27 的"无损但吞吐崩塌"失败模式从此有断言守卫。
      - **D2 BFS 真 ECMP**:`InstallRouting` 收集等代价多路径(按 first-hop 口分别扩展共享中间节点)装多端口前缀;s8 增双平面分布断言(spine10/11 tx 800/800)——"2-plane ECMP"由名实不符改为端到端验证。
      - **D3 MTP 判据硬化**:s8 输出 `S8 ACKSUM` 聚合行;`run-s8-mtp.sh` 对被掩 ACK/NACK 计数断言 |delta|≤1("ACK 被吞"类 bug 不再被行为 hash 掩蔽)+ 线程数扫描参数(2,4,8)。
      - **D4 run 脚本可诊断化**:5 个脚本的崩溃/timeout、缺行、BUILD_TYPE 回退(pipefail 死代码)、非法线程数均显式报错;`timeout` 包裹。
      - **D5 拓扑校验**:link 端点/switch id ≥ nodes 即拒绝建网(原为段错误);`SueLiteEndpoint::SetDevice` 空节点可读断言。
    - **E 阶段(P2 收尾,部分)已落地**:
      - **E1 conf 冒烟**:新 scratch `sue-e1-conf-smoke`——四个 conf 预置逐一 `SueReadConf`+`SueEntryDispatch` 建网+端到端投递(conf/ 预置从此有自动化覆盖)。
      - **E3 配置健壮性**:CL==0 VC 回填 512+告警(UEC Table 5-27,静默死锁根除);CF_MIN 单键配置生效(OnDrain 判据加 CF_min);新键 `PFC_DRAIN_DELAY_NS`(配置路径 PFC 门控可达成,原经 conf 静默等效 OFF);混合 FC_MODE 交换机双重 OnDrain 门控(`GetDeferCreditRefund()` 判据)。
      - **E4 防御层**:Receive 非 SUE EtherType 丢弃(误注入帧不再被静默解析转发);`SueSetGlobal` 改 `SetGlobalFailSafe`+告警;`SueMtpContractCheck`(MTP 晚于缓存首查即 abort,静默退化变显式违约);`SetupSueNetwork` 的 confFile 死参数落地(接口承诺兑现)。
      - **E5 UEC 口径**:replay_ct 仅 ACK/INIT 清零(UEC Table 5-10);`SetOutstandingSeqMax` 钳制 UEC 上限 524288。
      - **E6-lite 回归基线**:新脚本 `run-scratch-checks.sh` 一键全跑 27 个 scratch 判语(不触碰 fork 的 scratch/CMakeLists)。
    - **剩余(计划文档跟踪)**:E2 共享 test-util 去重、E7 512 卡压测冒烟 + S5b 验收标准(§11)、TestSuite 按阶段拆分、scratch 注册 ctest。

32. **UEC 规格收口:CBFC-SP-13 RTS/RTR 握手 + SP-20/SP-11 标 ⚪(2026-08-13,§5.2.8.4)**:
    - **CBFC-SP-13(✅ 已实现)**:§5.2.8.4 无损 VC 初始化握手的最小语义模型,仅 `LLDP_NEGOTIATE=1` 时启用(默认 0 直配零回归):
      - **头扩展**:`SueLldpHeader` 42→45B,新增 3 个 1B 位图(NVC=8)`R_VC_Want`/`S_VC_RTS`/`R_VC_RTR`;字节布局仍属管理面不复刻(§5.3.2 偏离延续)。
      - **状态机**(`SueNetDevice::OnRemoteLldpTlv`,事件驱动、无新定时器):本端 `m_lldpWant`=直配 CL>0 快照;每收一帧 `rts = remote.want`、`rtr = remote.rts & want`(**赋值语义**,静态输入与旧 OR 单调同收敛、每端≤3 帧);`active[x] = rts[x] && remote.rtr[x]` 时经 `SetVcLosslessActive` 开 VC + `DequeueAndTransmit` 唤醒挂起数据(新唤醒源;LLR 侧由既有 `OnLlrStateChange` 接力)。**动态撤销(2026-08-13,ULL-NEG-CBF-RCV-006 / §5.2.8.5 部分)**:`SetVcWant(vc,on)` 改 want 快照 + `rtr = rts & want` 即时重导 + 重发 LLDP;赋值语义使 want TRUE→FALSE 在 ≤3 轮内清对端 S_VC_RTS 并关闭其 active 门(act 1→0,帧退回排队——无 BE 通道,§12.32 最小模型);方向独立:撤销方自己的发送方向不受影响。
      - **adopt-once**:参数采纳(creditSize/pktOvhd/vcLimit)只在首帧执行一次(UEC 5.2.8.4 末段:VC 激活期间 creditSize 不得变更);`m_lldpApplied` 一次性闸门重构为 `m_lldpFirstRemoteApplied` + 每帧状态机(旧闸门会吞掉 fallback 后迟到的真实 TLV)。
      - **采纳一致性告警(ULL-CBFC-INIT-001,§5.2.8.1,2026-08-13)**:首帧采纳时比较本地快照与远端 TLV 的**结构性**字段——NumVCs 不一致、无损 VC want 集合不一致 → `NS_LOG_WARN` + 可观测标志(`GetLldpNumVcsMismatch`/`GetLldpLosslessSetMismatch`);数值型参数(creditSize/pktOvhd/vcLimit)按方向独立可合法不同(SC-26 256↔64),不告警;告警不阻断采纳(RTS/RTR 门已功能性兜底,TS-50)。
      - **引擎门**:`SueCbfcEngine::m_vcLosslessActive[8]`,`Init()` 默认全 true(直配路径零回归,`SetVcCreditLimit` 不碰该位);`CanSend` 首行加门。retx/控制帧绕过门是正确语义(握手前 LLR 未 Start、replay buffer 空;控制帧不走信用)。
      - **兜底**:10µs 无对端 → 自协商不动点(≤3 轮 want→rts→rtr→active);直配设备(`LLDP_NEGOTIATE=0`)忽略 LLDP 帧——否则未协商对端的无 want 响应会取消对方 fallback 造成死锁。慢链路 prop>10µs 时自协商可能先行(与旧 fallback 语义一致);如需严格化按链路 prop 折算窗口。
      - **非对称语义**:对端未 want 的 VC 在本模型里"排队不发"(引擎无 best-effort VC 通道,单 CL 数组简化的延伸)——由 TestSuite 非对称用例固化。
      - **验证**:TestSuite 48→**53** 例(新增 LLDP 头 round-trip / 握手门控 E2E(握手完成前 0 送达)/ 非对称 want / no-peer 兜底 / active 门单测 5 例);矩阵 NEG-SP-01/02 的 in-suite 验证同步补齐,scratch 载体 `sue-lldp-negotiate` 已恢复入库并与新握手状态机兼容(LLDP NEGOTIATE PASS)。
    - **LLR-SP-20 标 ⚪**:CtlOS 子优先级(UEC Table 5-12:INIT_ECHO=INIT > ACK=NACK > CF_Update)维持 §12.11 决策"单 FIFO、不做子优先级建模"——CF_Update 排在 ACK/NACK 之后为预期规格行为(§12.26 解耦后无积压病理)。
    - **CBFC-SP-11 标 ⚪**:非理想信用大小(多信用/单元差异释放)不入仿真——仿真缓冲粒度恒 1 信用单元,本地优化无建模价值。

33. **ECMP hash 熵扩展(§6.9,AGENTS 推迟项闭环)** → **✅ 已实现**。旧哈希 `(xpuId^seed)×2654435761` 使 2-plane 选择退化为**目的 XPU-id 奇偶**——同一宿主的两个流必走同一平面,单目的组永远只压一个平面(scale 冒烟需凑偶/奇目的各半才能双平面,§12.32 前观察)。现 `GetOutPort(xpuId, flowEntropy)` 双参重载:熵 = FNV-1a(MAC SA 6B | MAC DA 6B | shim FlowLabel 1B[F=1]),交换机 `SwitchReceiveFromDevice` 逐帧构造;**流内亲和、流间分散**;`flowEntropy==0`(单参重载/旧测试)保留旧公式逐位一致。验证:TestSuite `SueRoutingTestCase` 增 3 断言(0=legacy/确定性/64 熵扫描覆盖全组);s8 NACK+FLUSH MTP 等价 PASS(hash 逐字节不变);scale-smoke 双平面 1180/1380 自然分拆(不再依赖奇偶凑组)。

34. **E7 512 卡满负载压测(S5b 验收,§11)** → **✅ 已执行**。新 scratch `sue-scale-stress`(全端点跨机环流连续注入 + 500µs 排空;启动交错改槽位级 dither ≤1µs——smoke 的 700ns×主机号交错会把半数端点推出压测窗口;持续速率按**注入停止瞬间采样**,排空尾不计入,否则虚报超截面积):
    - **clean**(63G/XPU 需求,32T):98368/98368 无损、credit_exhausted=0、持续 **31.4T**、run 6.4s 墙钟(560µs sim)/10.5M 事件、双平面 49.7/50.3。
    - **sat**(168G/XPU 需求,86T vs 51.2T 截面积):**织物饱和 48.0T = 93.7% 截面积**(缺口 ≈ 控制帧/前导/IFG 开销)、credit_exhausted=221528、超额被信用窗全额吸收(零拒绝)261760/261760 全投递、9.3s 墙钟/12.6M 事件。
    - **MTP-8**:结果逐位一致(等价性保持到 512 卡满负载),墙钟 69.5s vs 单线程 9.3s(**7.5× 慢**——LP 同步开销 > 并行收益,与低负载结论一致;加速比改善为后续工作)。
    - **无负载**:578 LP 分区、~43MB RSS、1.65M 事件/ms-sim(ACK 节奏 + CF 定时器主导)。
    - **BFS setup 超线性实测**:64 机 227ms / 128 机 1.39s / 256 机 7.87s(每翻倍 ×6 左右)——per-switch 全图 BFS × 每节点 std::map/std::set 开销;64 机(目标规模)227ms 可接受,256+ 规模优化候选已记 `docs/scaleup-topology.md`(扁平数组 BFS / 按机分组),本轮不做。

35. **TX 侧 FCS 毒化注入 + `tx_poisoned` 计数(2026-08-13,B 类补齐)** → **✅ 已实现**。原 `tx_poisoned` 预留 N/A(无 TX 侧 FCS 上报路径)。现 `SueErrorModel::SetPoisonTxOnce/SetPoisonTxEvery` + `CheckPoisonTx`(与 CtlOS 丢帧同模式);设备 LLR_ACTIVE 发送路径命中时帧挂 `SueFrameBadTag`(包 tag 随通道存活到对端)+ 引擎 `IncTxPoisoned`;接收端 fcs 判定 **tag 优先**(POISONED:接受、不 NACK、计 frame_bad),ACCEPT_POISONED 挂 tag 幂等(TX 毒化帧已带 tag)。验证:TS-19 场景 2(第 2 帧 TX 毒化 → tx_poisoned==1、rx_poisoned==1、无 NACK 无重放、frame_bad 恰一次;CSV COUNTER-007 ⚠️→✅)。
36. **ScaleOut 最小端口速率 100G:1Gbps 样例全量上移(2026-08-13)** → **✅ 已执行**。ScaleOut 场景端口速率下限 100G,工程既有的 1Gbps 测试/样例全部上移:
    - **接线**:TestSuite ~80 处、19 个 scratch 的 `DataRate("1Gbps")`→100Gbps;`SetIfg(96ns)`(12B@1G)→ `SetIfg(GetDataRate().CalculateBytesTxTime(12))`(=1ns,设备同源量化);topo 链路串(endpoint-ring/mtp-equivalence/entry-dispatch/cfg-preset-smoke)同步。
    - **断言重推导**:TS-08/afh-wire-time 线时等式改 `CalculateBytesTxTime` 同源路径(100B@100G=8ns);TS-26 ACK 界 [3.2µs,138.4µs]→[32ns,1384ns]、ackPeriod 改 2048B 字节折算(10µs 直定时器在 100G 超 17296B 上限);TS-34 界改 `CalculateBytesTxTime(2048)-1ns`(163.84ns 在 NS 截断下量化 163);TS-53 更名 `SueDefaultConfig100GTestCase`(1G 病理在 100G 不复现,退化为默认配置自动折算守护)。
    - **replay-buffer 理论模型**(§6.10):量化 slot 10ns → RATE_MAC 10000 B/µs;mbps 纳秒化(µs 截断 +1% 偏差);Part 2 网格 ×64(4096..131072)、nFrames=(400+tu)×111(突发必须越过 glitch 尾,否则误分类——实测踩坑);C1 nFrames=30000(大缓冲不节流时 10000 帧 90µs 即发完,glitch 前缓冲已空——实测踩坑);ti=1 okFlush 去掉 tFlush 下界(100G 下 data_age 按前端帧龄计时,网格冲刷点落在 NACK 恢复边界而非 cap)。
    - **conf 预置收口 3 档**:`sue-default.conf` 改 100G 值(LLR 定时器 21830ns),删除 sue-100g.conf(合并),1G 档(203040ns,超 UEC Table 5-9 上限 65535ns 的仿真放宽)整体移除;cfg-preset-smoke 扫 3 档。已知不一致记录:显式 21830 与自动量化 21840 差 10ns,显式值生效。
    - **MTP 等价**:endpoint-ring/mtp-equivalence 转 100G 后 s8 2/2 与 s5b 等价 PASS;`SueScratchAppendDeviceCounters` 增 maskCfUpdate——CF_Update 为 CtlOS 节奏类消息,计数对 CF_min 边界 drain 交错敏感(100G 下 CF_min=10µs,线程数不同时 1ns 级边界翻转移相 ~4 次;接收侧幂等、数据路径不受影响),与 ACK/NACK 掩码同设计原则。
    - **credits-vs-rate 重测**(100G clean,8 点):64→11.47、128→22.94、256→45.88、512→91.75、768+→97.93 Gbps——与 1G 列逐点 ×100 重合,机制速率无关结论保持。
    - **文档**:§5.7/§6.10/§9 常数 100G 化;§12.25-12.28 的 1G 实测保留为历史实验数据;config-guide §2.7 删 1G 列;CSV CTL-RATE-001 界注 [32ns,1384ns]@100G、CFG-TIM-001 超限 caveat 解除;uec-spec-matrix 预置 3 档;test-report §4 + credits-vs-rate.html 100G 列。
    - **验证**:TestSuite 58/58、scratch 33/33、s8 NACK+FLUSH 2/2、s5b ENDPOINT MTP EQUIVALENCE PASS、残留 grep 清零。

37. **THU-SPEC 三速率表对齐:预置 3 档按 THU 实测收口 + 多量子阈值触发修复(2026-08-14)** → **✅ 已执行**。THU 更新 CF_Update 数据为三速率表(标题改"与端口速率的关系"——字节键语义被实测确认),预置按实测对齐(详情 thu-cf-update-verify §3.13):
    - **THU 表映射**:default 列 = threshold 等效周期 2074.7B/2074.5B/2074.5B(2048 SDK 默认 th=4×512B + ~27B CF 消息自身线时计入计数器),三速率字节周期恒定;max 列 = 960B 字节地板(13,020,601/26,046,236/52,091,984,×2/×4 精确,速率相关 = 字节键);min 列(仅 100G)= 993,479/s = {timer=511, spacing=1} 芯片寄存器怪值(有效 12,582B,§thu-cf-update-verify 3.5/3.6)——仿真触发口径可复现(CF_MAX=12582 无打流 → 993,479/s/触发,消息 ×4 批量差异),真义待 THU A' 定标矩阵。
    - **预置改动(3 档同值,字节键/时间键速率无关)**:`CBFC_CF_THRESHOLD_BYTES` 0→**2075**(100G 6,024,096/s vs THU 6,024,685,差 0.01%;200G 差 0.02%;400G 41.5ns 落 NS 网格 41ns → +1.2%,1ns=50B 分辨率下 2075 非整数 ns;THU 计数器口径若修正回 2048);`CBFC_CF_MIN_TIMER_BYTES` 125000→**0**(threshold 主导——CF_min>0 时 CF 节奏 = 1/CF_min,阈值流被钳,与 THU 默认档 spacing=32KB 不绑定的实测矛盾;0=无引擎地板,设备固定 400B CtlOS 门仍封顶 2% 帽,§12.26);`CBFC_CC_MSG_TIMER_US` 100→**102**(THU 9,790/s = 102.1µs;max 档 7,814,426/s = 128ns 低于 µs 键分辨率,不表达);`LLR_CTLOS_TARGET_SPACING` 仅 400G 档 2048→**2050**(2048B=40.96ns 被 NS 截断 40ns → ACK 偏快 +2.4%;2050B 精确 41.0ns,与 THU 24,414,902/s 差 -0.1%;100G/200G 档保持 2048 = THU 配置值,实测速率差为 64b/66b 口径待确认,thu-spec-compliance 疑点 5)。CF_MAX=1250000 不动(周期全量叠加仅 +0.66% 消息量;THU 默认配置的 timer 观测量近似看门狗不叠加,差异记录)。thpt-line-rate/incast-2to1 内联镜像同步(THPT_CFMIN 默认 0;thpt 保持 ACK 2048B,其线速分析按 2048B 推导)。
    - **多量子阈值触发修复**(sue-net-device.cc OnDrain):原单 `if` + 循环内清 dirty → 帧长 ≥ 阈值时(如 4096B 帧 vs 2075B 阈值)每次 drain 只发 1 条、速率塌到帧率,与"floor(总字节/阈值) 次触发"的注释语义不符(100B 小帧场景从未暴露);改 `while` 逐量子发送、dirty 循环外清零。TS-54 S4(100B 小帧)路径不变;扫描 4096B 大帧验证 60,239 条 = 125M 字节/2075 精确。
    - **验证**:scratch sue-cf-rate-scan 增 `CF_FRAME_BYTES`(默认 4096;地板档须 100B 小帧,帧节奏密于地板)+ 打流速率窗口锚定数据接收区间(排除打流后空闲期 CF_max 尾);run-scratch-checks 注册 11 配方(裸跑/400B 门顶/1KB 地板/CF_max 1MB/预设 3 速率/960B 地板 3 速率/12582 min)全 PASS;TestSuite 60/60、scratch 44 判语回归。

38. **LLR 2^20 seq 回绕缺陷修复(2026-08-14)** → **✅ 已修复 + 回归用例**。512B 小帧 10Gbit 长突发(244 万帧 = 2.33 次 2^20 seq 回绕;2048B 帧的 61 万帧从不回绕,故从未暴露)实测每回绕丢 ~3 帧:接收侧 `m_nextRxSeq` 裸递增,越过 2^20 后与 wire seq(取模空间 0..2^20-1)永不相等 → 回绕后全部帧判 missing → NACK → REPLAY 数轮 → FLUSH 重同步(丢缓冲)。修复:GOOD/POISONED 分支 `++m_nextRxSeq` 改 `(m_nextRxSeq + 1) % SEQ_MOD`(其余使用点已全部取模,兼容);TX 侧 `m_txSeq` 裸计数 + 全取模算术本就正确。验证:512B/1024B 长突发全收、nack/discard/replay 全 0;新增 TestSuite `SueLlrSeqWrapTestCase`(引擎级全空间 2^20+100 帧驱动:逐帧 ACCEPT、零 NACK、ACK 全在界内,0.44s),59/59 PASS。

39. **信用窗口曲线的时延依赖模型(2026-08-14 实测定标)** → **✅ 公式 + 扫描验证**。THU 对齐预设(阈值即时退款)下窗口曲线为**台阶形**,三段模型(**离散量化精确版**;§thu-cf-update-verify 姊妹结论,phase-summary §3.3):(a) CL < ⌈F/CS⌉ → 死锁 0;(b) 在途批次 k×F < Q(k = ⌊CL/⌈F/CS⌉⌋,Q=阈值)→ 批次跨不过阈值,靠 CF_max 周期兜底,速率 ≈ 2.2×kF/CF_max 周期,**与时延无关**(实测 prop 0/1000ns 相同);(c) k×F ≥ Q → 批次内跨阈值 → 速率 = 线速×kF/(kF+RTT×线速)。**跳变点 = ⌈Q/F⌉×⌈F/CS⌉**(Q=2075/F=526/CS=56 → CL=40,实测 CL=39 节流 0.171/CL=40 跳变 92.49;Q=8192→CL=160 92.90、Q=16384→CL=320 93.33,THPT_CF_THRESHOLD 扫描);**释放量子 Q = 跳变窗口 + 控制报文频率(数据率/Q)的权衡键**。时延/延迟释放扫描(THPT_PROP_NS/THPT_FWD_NS):速率 = 线速×W/(W+RTT×线速)(prop 0/100/500/2000ns 实测 92.49/42.95/13.76/3.87 Gbps;fwd=300ns 时 CL=47 掉到 37.50、CL=128 完全恢复——转发预算计入退款 RTT)。**几何意义:W = RTT×线速 时恰为半速——事件驱动退款下 BDP 是半速点而非线速点**;跳变点本身与时延无关。工程含义:窗口选型 = 防阈值死锁下界 + RTT 补偿余量;**拥塞限速靠延迟释放(出口服务时间计入退款 RTT),不靠窗口值**。scratch `sue-thpt-line-rate` 增 `THPT_PROP_NS`/`THPT_CF_THRESHOLD`/`THPT_FWD_NS` 三旋钮供复现。


---

40. **交换机 VRF 转发域(Switch-ASIC-AFH.md §6.6.1/6.6.4,2026-08-14)** → **✅ 已实现**。转发库按 VRF 分段,查表键 = (目的 XPU-id, VRF);VRF 由入口三元组派生,优先级 **DSCP > VLAN > 端口**(用户拍板),未命中任何映射 = VRF0;帧不携带 VRF(线上无字段,VRF 纯交换机本地配置,与芯片一致)。
    - **实现结构**:VRF 维度在交换机层——`SUESwitchNode::SetRoutingTable(vrf, rt)`/`GetRoutingTable(vrf)`(vrf0 = 旧 `SetRoutingTable(rt)` 别名);每 VRF 一张普通 LPM 表(`SueRoutingTable` 类不变);VRF>0 未编程表 = lookup miss,**不回退 VRF0**(分段语义)。`AddPort` 填 dev→portIdx 反向表;`ComputeVrf(portIdx, h)` = shim DSCP 映射命中 → VLAN vid 映射命中 → 端口映射 → VRF0。
    - **配置键**(照 DSCP_TO_VC_MAP 模板四段:GlobalValue + g_keys + 缓存 accessor + SueReadConf priming):`DSCP_TO_VRF_MAP`("dscp:vrf",0..63)/`VLAN_TO_VRF_MAP`("vid:vrf",0..4095)/`PORT_TO_VRF_MAP`("portIdx:vrf",AddPort 序 0..255),默认空 = 全 VRF0;映射值 0 与未映射同义(均为 VRF0)。accessor `SueConfig::DscpToVrf/VlanToVrf/PortToVrf`。
    - **计数器**:`GetCntVrfRx(vrf)`(转发)/`GetCntVrfMiss(vrf)`(该段 miss,AFR_DST_LOOKUP_MISS per segment);全局 `GetCntLookupMiss` 不变。
    - **路由装配**:`InstallRouting`(入口自动建路)仍只建 VRF0(scale 场景单 VRF 零回归);VRF>0 由用例/自定义代码显式装配。
    - **测试**:TS-60 三节——分段单元(表装配/未编程=null)、配置 accessor 默认与部分覆盖、e2e 优先级阶梯(全未映射→vrf0→hA;port→vrf1→hB;VLAN 覆盖 port→vrf2→hA;DSCP 覆盖 VLAN+port→vrf3 未编程→miss 计数)。TestSuite 60/60、scratch 44/44。

41. **VRF 32 平面拓扑:8 GPU × 32×200G 口、单台 256 口交换机 all-to-all(2026-08-17)** → **✅ 已实现 + 全断言 PASS**。任务:每 GPU 32 个 200G 口只用 XPU ID 标识(无 port-id 细分)→ 交换机 VRF 把 256 口划成 **32 个平面**;GPU 口只与其它 GPU **相同端口号**通信;只用 XPU ID 完成 8 GPU 互通并验证 all-to-all 性能。用户决策:**LLR+CBFC 全栈**(256 链路 INIT 握手 + 累积 ACK + replay 保护;零 replay/discard 断言)。
    - **寻址模型**(§12.40 语义的直接应用):8 个 XPU ID;平面 = 发送端口的选择——帧 dst 只有 XPU j,**帧不携带 VRF**;交换机口 (i,k)(GPU i 第 k 口,topo 行序 = AddPort 序 i×32+k)经 `PORT_TO_VRF_MAP` 映射 VRF k+1,每 VRF 内 8 条精确路由 `XPU j → 口 j×32+k`。VRF0 = 空表(默认平面不用,所有流量命中 VRF1..32,miss=0)。**自动建路只支持 VRF0**(且会把每 host 建成 32 口 ECMP 大组,与本设计冲突)→ scratch 显式覆盖:`SetRoutingTable(0, 空表)` + 32 张 `AddPrefix(j, 10, {j×32+k})`。
    - **流量条纹化**:56 对 × `pairGb×1e9/8/frameBytes` 帧(默认 1 Gbit/对 → 61024 帧/对,帧序 %32 = 发送口 k → 每平面每对 1907);1792 条流,注入间隔 = 7×`CalculateBytesTxTime(2140)` = 599ns(7 个竞争源共享一条 200G 出口,注入节拍 = 满载线速);直接 `SwitchSend` 注入,egress 队列 + CBFC 信用门自然节拍。
    - **断言六组**:①每 GPU 从其余 7 GPU 各收 61024(XPU-id-only 可达);②delivered==sent(3,417,344/3,417,344 零丢失);③逐平面 `GetCntVrfRx(k+1)==56×1907` × 32 + `GetCntVrfRx(0)==0` + `GetCntLookupMiss()==0`;④每 GPU 口 k 收 7×1907(同口通信);⑤LLR replay/discard/nack 全 0;⑥聚合 ≥ 0.95×天花板。
    - **实测**:LLR=1 全栈 48,982.8 Gbps = 天花板 48,998.9(51,200 × 2048/(2048+14+8+12+2×29))的 **99.9673%**;per-GPU 均匀 6.99872 Gbit × 8;交换 pend=0、所有 CU=0、CF/CC 收支平衡;LLR=0 同判语 PASS。墙钟 ~93s/档。
    - **配置扩容**:`SueConfig::PortToVrf` maxKey 63→255、端口缓存 64→256;`SUESwitchNode` pCnt 64→256(仅声明,m_ports 本就动态)。
    - **注册**:scratch `sue-vrf-planes`(= **SC-34**,test-cases.md)+ run-scratch-checks 2 配方(LLR=1 默认 + `VRF_PL_LLR=0`),scratch 判语 44→46;旋钮 `VRF_PL_PAIR_GBITS`/`VRF_PL_FRAME_BYTES`/`VRF_PL_LLR`。**后续扩展**:§12.43 在同场景上加 CBFC vs PFC A/B 臂(`VRF_PL_FC=PFC` 第 3 配方,判语 47;PFC 生效时更差)。

42. **延迟退款尺寸不变性缺陷修复(§12.41 事故,2026-08-17)** → **✅ 已修复 + TS-61 回归用例**。现象:LLR=1 32 平面 all-to-all 在 ~1% 进度全停(switch swRx 平台化),诊断见 S_VC_CU≈32767 = 信用窗口永久毒化。
    - **根因**(两个既定机制的交互,各自单独正确):§12.30 延迟退款下 `Forward()`/`ServicePending()` 在 `SwitchSend` **之后**量 `p->GetSize()` 作退款尺寸;而 LLR 链路的 `DequeueAndTransmit` 在出队时**原地**(in-place)给同一个包对象前插 8B preamble(§5.x 规则:OnTx 记账在 prepend 前、wire 尺寸在 prepend 后)。空闲出口上 `SwitchSend` 同步跑 `DequeueAndTransmit` → 退款量到的是 2056B 而非 2048B,跨 256B 信用边界(9 信用 vs 计费 8 信用——入口 OnRx 量的是无 preamble 帧)→ **S_CF > S_CC 过度退款** → CU = (S_CC−S_CF) mod 2^15 ≈ 2^15−δ ≫ CL → `CanSend` 永假。诊断钥匙:超退 28 信用 ≡ 4 (mod 8) 无法用帧数错配解释(帧 = 8 信用/帧);窗口 316 = 28×9 + 8×8 恰好 → 28 帧按 9 信用退。LLR=0 从不触发(无 preamble 变异)。
    - **修复**:`Forward()` 与 `ServicePending()` 在 `SwitchSend` **之前**快照 `const uint32_t frameSize = p->GetSize()`,`EgressCanAccept` 与 `RefundCredit` 一律用快照值——不变性陈述:**退款释放的 PktCredits 必须恰等于入口 Receive 计费的数量**(与 TTL 丢弃退款同一口径)。两条准入路径同改(ServicePending 当前不活——freed hook 触发时出口必忙,同步前插不成立;仍改,使两条路径不可能独立回归)。
    - **回归用例**:**TS-61** `SueDeferredRefundPreambleTestCase`(host A—交换机—host B,p0 延迟退款、p1/B 开 LLR 且空闲出口接第一帧 = 同步前插事故现场;断言 2 帧全收、`R_VC_CF(0)==16` = 2×8 无 preamble 信用、CC 更新后 `S_CC==S_CF` 且 `CU==0`)。
    - **为什么此前未暴露(逃逸分析)**:缺陷需四条件同时成立——①交换机 CBFC 口延迟退款(退款点在准入路径);②出口链路 LLR(才有 in-place prepend);③帧恰中**空闲出口**(同步出队 → prepend 先于测量完成;忙出口走排队路径,测量仍干净);④帧长+8B **跨信用边界**(仅 size mod CS ∈ (CS−8, CS],CS=256 时约 3% 帧长窗口)。逐一对照既有矩阵:LLR 专项族全是两节点直连、无交换机(①不成立,准入路径从不执行——且这些测试恰好证明 prepend 本身正确);incast 帧 2062B(CS=256 余 14)、scale-stress 帧 2014B(余 222)均不跨界(④不成立,**只差 14 字节的帧长运气**,两者都吃 `CBFC_CREDIT_SIZE` 默认 256);`SueTtlDropRefundTestCase` 退款点在丢弃路径、不经 `SwitchSend`,结构免疫——它已确立"退款=计费"不变性,但只守护了丢弃口、未守护准入口。VRF 32 平面第一次凑齐四条件:200g 预置 CS=256 + 帧长取**总帧 2048 = 8×256 整倍数**(整个帧长空间最危险的尺寸,+8B 必然跨界)+ 256 链路全 LLR + 全网背压 + 1792 流每个出口冷启动必遇空闲同步路径。
    - **教训(防再犯)**:信用/记账尺寸**只在计费点量取或在任何 in-place 变异前快照**;任何会把包交给发送路径(`SwitchSend`/`Send`/`TransmitStart`)的代码,之后不得再读 `GetSize()` 作记账用;**同一不变性必须在每个退款点显式守护**(丢弃口与准入口同权);跨模块组合场景设计流量前,先列前置条件交集——单模块专项全 PASS 不代表交集被覆盖。失败模式是硬停(窗口永久毒化)而非偶发丢包,未命中场景 100% 干净、命中场景直接停死,无中间态可追。run-scratch-checks 同时钉 LLR=0/1 两档(47 判语),事故现场本身就是常驻回归。
43. **CBFC vs PFC 同量 A/B:VRF 32 平面 all-to-all(§12.41 场景,2026-08-17)** → **✅ 结论:同等数据量下 PFC 更差(生效时);仅配置默认 drain=0(PFC 实质未生效)时与 CBFC 持平**。任务:同一 all-to-all 数据量,把流控从 CBFC 换成 PFC,判定效果更好还是更差。
    - **方法**:SC-34 `sue-vrf-planes` 扩 FC A/B 两臂——`VRF_PL_FC=CBFC|PFC` + `VRF_PL_PFC_DRAIN_NS`(接收驻留);两臂**同 offered load**(注入节拍同一 wireBytes = 帧+14+8+12+2×29,56 对 × 1 Gbit 不变)、同天花板标尺(断言沿用 CBFC 载荷份额口径 48,998.9 Gbps,A/B 必须同一标尺)、同六组断言;注入门控 FC 感知(CBFC 窗口满 / PFC pause 时按 100ns 节拍重试,不积压埋控制流);200µs 采样器对全织构 512 口普查 `IsPaused(0)` 累计 pause-samples。PFC 臂 conf 显式写出全部门限 GlobalValue(防默认漂移改口径)。
    - **PFC 建模要点(§6.4)**:接收侧占用 = OnRx 计费后经 `PFC_DRAIN_DELAY_NS` 才 OnDrain 的在途字节;**drain=0(配置默认)→ 立即排空 → 缓冲永不累积 → pause 永不触发,PFC 等价关闭**(GlobalValue 注释既定的建模约束)——要做有意义的对比必须给 drain > 0,占用 ≈ 速率 × drain(Little 定律,200Gbps = 25KB/µs)。
    - **实测**(墙钟 ~93s/档):

      | 臂 | 聚合 Gbps | 天花板占比 | pause-samples(25 点×512 口) | 判定 |
      |---|---|---|---|---|
      | CBFC(LLR=1 基线) | 48,982.8 | **99.9673%** | —(无 pause 机制) | PASS |
      | PFC drain=0 | 48,983.0 | 99.9675% | 0(实质未生效) | PASS |
      | PFC drain=1µs | 36,890.9 | 75.2892% | 2,528(≈20% 口·时刻 paused) | 低于标尺 |
      | PFC drain=2µs | 22,343.9 | 45.6008% | 4,320(≈34%) | 低于标尺 |
      | PFC drain=5µs | 9,658.6 | 19.7119% | 11,328(≈88%) | 低于标尺 |

    - **机制**:all-to-all 按目的分相,每相每个出口 7:1 incast(7 源 burst 顺序打同一 200G 出口)。CBFC 逐 VC 信用窗 + threshold 2075B 近即时退款维持"7 源各得 200/7 Gbps"的连续均衡 → 出口恒满载。PFC 是**阈值-迟滞 stop-and-go**:占用越过共享池门限 → pause **整条链路整个优先级**(无 per-VC 粒度)→ 排空至 resume 门限 → burst 回填 → 再 pause。drain=1µs 稳态占用 25KB 逼近 pool 32KB → 临界震荡(吞吐 −24.7%);2µs → 50KB > pool → 持续 pause 循环(−54.4%);5µs → 125KB ≫ 64KB buffer → 近全程 paused、只靠 resume 窗滴灌(−80.3%)。**无损契约所有臂均保持**(delivered==sent 3,417,344/3,417,344、replay/discard/nack=0、平面隔离精确、pend=0)——PFC 的代价纯是带宽,不是丢包。
    - **口径与局限**:本模型 PFC 缓冲预算(64KB/口、pool 32KB)相对 200G 的 25KB/µs 驻留偏小,真实 MB 级缓冲 + 调优门限会缓和曲线,但**链路级 stop-and-go vs 逐 VC 信用均衡**的定性差距是机制性的。A/B 的公平性由"同注入纪律、同标尺、同断言"保证,PFC 臂甚至略占便宜(无 CF 返回份额、物理天花板更高),仍在同一标尺下大幅落后。
    - **注册**:常驻回归取 `VRF_PL_FC=PFC`(drain=0,六组断言全绿)钉住 PFC 全链装配/注入门控/采样诊断路径,scratch 判语 46→**47**;drain>0 三档不满足 95% 标尺不注册,数字入本记录与 SC-34 作为实验结论。

44. **VRF 32 平面拓扑的 MTP 适配与等价性(§12.41 场景,2026-08-18)** → **✅ 已闭环:四臂 BHASH 逐字节一致**。SC-34 此前 conf 硬编码 `SUE_MTP_THREADS 1`,未做 MTP 验证(§12.43 A/B 均单线程)。
    - **scratch 改造**(`sue-vrf-planes`,行为对单线程基线保持:三注册臂全绿、聚合 99.8327% 不变):新旋钮 `VRF_PL_THREADS`(默认 1);`MtpInterface::Enable` + impl 换装先于建网(同 sue-scale-smoke 套路);流首发 `ScheduleWithContext(宿主节点)` 驻留 LP(§10.5/§12.21)+ 互异 1ns dither(流索引 0..1790,§12.20 出发时刻全局唯一);诊断采样器驻留交换机 LP;末尾输出 `VRF-PLANES BHASH`(`SueScratchAppendDeviceCounters` 掩 ACK/NACK + CF_Update + 8×8 交付矩阵)+ `VRF-PLANES WALL_MS`。
    - **MTP 数据竞争修复(§12.31 A4 同类,scratch 层)**:首跑 threads=4 验证段整体缺失——`g_streamsLeft`/`g_rxBytes`/首末收包时刻为普通标量,被 8 个宿主 LP 并发递减/累加,丢失更新使 `--g_streamsLeft==0` 永不成立。改 `std::atomic`(fetch_sub/CAS-max);per-GPU 行计数各 LP 只写自己的行(收包 upcall 在收包宿主 LP),保持普通标量。教训:凡"收包侧聚合"变量,LP 归属分析必须落在**写入者**维度,而非读取者。
    - **等价判据与实测**(`./run-vrf-mtp.sh [2,4,8]`,每臂 threads=1 vs N 双跑 diff BHASH):四臂——LLR=1+CBFC / LLR=0 / PFC drain=0 / PFC drain=2000(§12.43 生效档,性能断言双跑同 FAIL 是预期,判据放宽为"判定一致 + BHASH 一致")——1/2/4/8 线程 BHASH **逐字节一致**,聚合速率同为 99.8327%。掩掉的 ACK/NACK 由 CHECK 4(replay==discard==nack==0)与 delivered==sent 兜底。
    - **性能结论(诚实披露)**:该拓扑 9 节点 = 9 LP,交换机单 LP 承载全部转发/CF 退款事件(约半数事件),并行天花板 ≈2×;实测墙钟 1/2/4/8 线程 = 89.0/90.4/89.6/90.9s——**无加速也无倒退**(对比 64 机 fat-spine 满负载 MTP-8 慢 7.5×:LP 数 578 vs 9,跨 LP 同步摊薄)。加速比改善(粗粒度 LP 合并/批处理 ACK 定时器)仍为开放项,本拓扑不是其受益场景。

45. **VRF 平面拆 LP:拓扑分解变体获得 MTP 正加速(§12.44 延伸,2026-08-18)** → **✅ MTP-8 首次正加速 1.89×**。观察:§12.44 无加速的根因是**分区单位 = 节点**——单台 256 口交换机是 1 个节点 → 1 个 LP,全部转发/退款事件串行;而 VRF 平面本就隔离,可把交换侧按平面摊开。
    - **等价性论证(拆分的前提)**:① `SUESwitchNode` 无跨端口功能状态——`m_pending` 按端口、egress 队列/LLR/CBFC 引擎全在设备级,节点级仅统计计数器;② VRF k 的路由出口全在同平面(`XPU j → 口 j×32+k`),帧进入平面 k 后只在平面 k 内闭环;③ 宿主侧口 k 只连平面 k。故"1×256 口交换机 + PORT_TO_VRF_MAP"与"32×8 口平面交换机"转发严格等价——split 单线程全断言 PASS、聚合 99.8327% 与单机臂逐位相同(交付/平面/端口/LLR 计数)。
    - **实现(scratch 层,零 `src/sue/` 改动)**:`VRF_PL_SPLIT=1` → topo 40 节点(node 8+k = 平面 k 交换机,链路序保持 (i,k));平面交换机每台 VRF0 表 8 条精确路由 `XPU j → 口 j`(PORT_TO_VRF_MAP 为全局 per-portIdx 表,无法按交换机区分,split 臂不用——单机 VRF 语义由默认臂钉住);交换机侧断言/诊断改跨 32 台聚合;BHASH 自然覆盖 40 节点(split 臂内跨线程 diff,不与单机臂比——节点编号不同)。
    - **实测**:MTP 分区 = 40 LP(8 宿主 + 32 平面交换机);1/2/4/8 线程 BHASH 逐字节一致、全 PASS;墙钟 **88.4/83.5/65.8/46.8s → 1.89×**(vs 单机模式 MTP-8 90.9s = 1.94×)。残余串行瓶颈 = 宿主 LP(每宿主 32 口引擎 + 224 流,8 宿主 LP / 8 线程恰每线程一宿主 + 4 平面交换机)。
    - **边界与教训**:该变体建模为"32 颗 8 口交换芯片",与"1 颗 256 口芯片 + VRF"是**两种硬件口径**——验证 VRF 机制必须用默认臂,split 臂只用于 MTP 并行收益与等价性;两者结果一致性本身即"VRF 隔离 = 可分解"的实证(与 §12.40 三源映射无关的口闭合)。MTP 加速比开放项(§12.44)在本拓扑以此方案收口;64 机 fat-spine 的 7.5× 倒退不变(578 LP 同步开销主导,非分区结构问题)。

46. **64 机 fat-spine MTP 7.5× 倒退根因:lookahead=0 致轮数暴雨(§12.34/§12.44 开放项,2026-08-18)** → **✅ 已定位并验证修复路径(prop≥100ns 即 2.19× 正加速)**。
    - **MTP 执行模型**(读码,`src/mtp/model/`):全局按"轮"推进——每轮 stage1 各 LP 只执行 `时间 ≤ min(全局最小下一事件 + 本 LP lookahead, 公共 LP 时间)` 窗口内事件 → 双自旋屏障 → stage3 收跨 LP 信箱 → 串行扫描全部 LP 求新最小时间。**lookahead = 到远端 LP 的最小链路时延**。
    - **根因链**:scale-smoke/stress 的 conf **不写 `SUE_LINK_PROPAGATION_NS`**(注册默认 0)→ 全部链路 0ns → LP lookahead=0 → 每轮只能执行"全局最小时间戳"上的事件 → **轮数 = 全局不同事件时间戳数**(64 机 sat 实测 582,446 轮);每轮固定成本 ∝ LP 数(578 LP 两级派发 + 每 LP 2 次 `system_clock::now` + 579 次 heap peek + 双屏障,实测 ~118µs/轮)→ 58 万轮 × 118µs ≈ 69s,淹没 7.3s 的有效事件工作 = 7.5× 倒退。轮数还随 N 增长(时间戳更密)+ 每轮成本随 N 增长 → 开销 ~N²,规模越大爆炸越狠(n=16:253k 轮 × 33µs = 8.3s)。
    - **为什么 VRF 拓扑没倒退(§12.44 的反差解释)**:vrf conf 写了 `SUE_LINK_PROPAGATION_NS 10` → lookahead=10ns,轮数有界;且 9/40 LP 每轮成本小——同一机制的两面。
    - **验证(MTP-8,64 机 sat,`SCALE_PROP_NS` 旋钮)**:prop=0 → 68.9s/582,446 轮;prop=100ns → **3.57s/3,987 轮(轮数塌缩 146×)**,vs 同配置单线程 7.80s = **2.19× 正加速**;t2/t4 = 6.71/4.19s。prop≥1000ns 时 sat 截面积断言 FAIL(28.4T < 90%)——**lookahead 不是免费的**:传播时延进入 RTT,信用窗口需求随之上升(§12.39 W=RTT×线速 半速点),100ns 恰在断言容忍内。
    - **处置(2026-08-18 落地)**:smoke/stress conf 预设改 `SUE_LINK_PROPAGATION_NS 100`(stress 旋钮 `SCALE_PROP_NS` 默认 100,=0 可复现历史口径;历史 prop=0 数字在 `docs/scaleup-topology.md` 括注保留)。**新口径基线(全 PASS)**:smoke 64/128/256 机 t1 = 7.5/21.2/98.7s,t8 = 4.3/9.9/39.1s(**1.74×/2.14×/2.53×**;原 64 机 30.5s 慢 2.7×;256 机 setup 串行 BFS 7.9s 占墙钟 ~20%);stress clean 31.0T(60.6%)、sat 46.5T(**90.85%**,RTT 代价,§12.39 预测内)、sat MTP-8 3.64s(**3.33×**,聚合逐位一致,credit_exhausted Δ21/263k 为重试边界决胜差、单线程复跑逐位稳定)。候选后续(评审):② fork 级优化(向上游报):lookahead=0 时按"同时间戳批"合并轮、跳过空 LP、去 per-LP per-round `system_clock::now`、屏障改条件变量——不动模型代码。

47. **AXI-over-SUE 事务型 workload:GoBackN 事务仿真 + 4-VC 通道分离(2026-08-19)** → **✅ LLR=1 零回滚 vs LLR=0 回滚实测闭环**。平台从「流量仿真」升级到「事务仿真」的首个 workload，验证 LLR 对上层 AXI 语义的保护效果。
    - **模型**:scratch `sue-axi-workload`(SC-35)。事务级 GoBackN(无 RH 头,DESIGN §1.2 预留):master 发起 read(AR→K×R)或 write(AW+K×W→B)事务,slave 被动响应;超时→整事务回滚重发(attempts 递增,`AXI_MAX_RETRY`=8 后 abort)。
    - **AXI→VC 映射(4 通道分离)**:AW+W → pcp 1 → VC1;AR → pcp 2 → VC2;B → pcp 3 → VC3;R → pcp 4 → VC4(`PCP_TO_VC_MAP 1:1,2:2,3:3,4:4`,无 shim)。每 VC CBFC 独立信用窗——通道间无循环依赖(读路径 VC2/VC4 分向、写路径 VC1/VC3 分向),与实物 AXI-over-SUE 的 VC 分配一致。
    - **LLR=0 模型**:topo llrEna=0 + 全设备 legacy 分支(剥前导、只丢 BAD,同 THPT_LLR 修复);丢失帧在接收侧直接丢弃→事务层超时→GoBackN 整消息重传。
    - **实测**:`AXI_TXN_BYTES=32768(32KB)`,K=16 帧,`AXI_OUTSTANDING=16`,`AXI_TXNS=500`,200Gbps,100ns prop,loss=1/100:
      - **LLR=1:retries=0**,aborted=0,avg latency=28µs——1/100 坏帧率下**零事务级回滚**,LLR 完全吸收链路层损失;
      - **LLR=0:retries=55**(250 笔写事务中 55 笔 B/W 帧丢失触发回滚),aborted=0;
      - **loss=0/1/10000/1/1000/1/100 × LLR=1 全 PASS**:retries=0 恒成立;
      - 每-VC 帧计数与 txn/retry 数精确匹配(switch `GetCntVcRx` 逐 VC 一致)。
    - **验证报告**:`docs/validation-report.md` §1 增加 AXI workload 用例表(输入/预期/实测/结论)。
    - **设计闭环**:验证报告 §1.5 的结论「LLR ~3% 开销换零丢失 + 应用零感知」现在有了事务级实测支撑——不是「丢帧不恢复上界」了;RL=1 下 500 笔事务零回滚,从数据面到应用面完整验证了 LLR 的价值。

48. **AXI workload RTT 扫描:两个 LLR/CBFC 配置缺陷定位与修复(2026-08-24)** → **✅ prop≤700ns 全 PASS 零回滚;RTT=1µs 从 0 Gbps 恢复到 61.6 Gbps**。用户质疑:SDK 204 credits×56B=11.4KB 即可线速,仿真 4096×256B=1MB 反而在 RTT=1µs 完全失败——是不是死循环?
    - **结论:不是死循环,是两个配置缺陷叠加造成的假死循环 + 一个真实瓶颈**。
    - **缺陷①(假死循环,已修)**:axi conf 未设 `LLR_DATA_AGE_TIMER_MAX_NS` → 注册默认 50µs。长 RTT 下 replay 风暴使帧在 replay buffer 滞留 >50µs → 数据过期 FLUSH → 丢帧(master discard=3504、交换机 rx_missing=16201 实锤)→ 事务超时 → 回滚重发 → 又风暴,直到 maxRetry=8 abort(retries=672)。thpt 显式设 4s 并注释"只有真断链才触发 FLUSH",axi 漏配。**修复**:conf 加 `LLR_DATA_AGE_TIMER_MAX_NS 4000000000`。
    - **缺陷②(超时未随 RTT 缩放,已修)**:Timeout 固定 200×K×wire+200000,prop≥800ns 时事务在退款延迟下误超时。**修复**:加 8×prop(RTT)项。
    - **真实瓶颈(未修,机制已实证)**:CtlOS 时隙被 ACK 流占满。`LLR_CTLOS_TARGET_SPACING` 同时驱动 ACK 周期(2048B→82ns@200G,空闲也发)和 CtlOS 时隙;CF_Update 最低优先级排队等时隙 → **退款延迟 ∝ RTT**(sue-net-device.cc 注释原文)。修复后 prop 曲线:100/300/500/700ns → 110.6/100.7/61.6/13.6 Gbps,零回滚——窗口 1MB 远超 BDP(700ns 时 52.5KB)仍随 RTT 暴跌,瓶颈在退款节奏而非窗口大小。候选修复:ACK 与 CF_Update 时隙解耦(单独 spacing)、或退款不依赖 CtlOS 队列(模型级,留评审)。
    - **SDK 204 credits 差异的定量解释**:204×56B=11.4KB 窗口 = SDK 假设的 BDP(200Gbps×457ns 发送时间)→ SDK 在 RTT≤~450ns 时线速,与我们 prop=100ns(110.6Gbps)吻合;SDK 退款为量子级即时,窗口只需覆盖 BDP;仿真初始"1MB 反而失败"是缺陷①假象,修复后窗口远超 BDP 的场景全部零回滚。
    - **验证**:计数器诊断(dev[n][d] llr/cbfc)入 scratch;CF_min=0(阈值即发)vs 10µs 节拍实测无差别,保持原配置最小化回归。
    - **ACK/CF_Update 优先级结论(用户提出,探针实证)**:UEC 优先级是"同频率下 ACK 优先"而非"ACK 独占时隙"。实现中 CtlOS 发送节奏 82ns/条正常(TransmitControl 探针),CF_Update 每 4 帧阈值触发正常(OnDrain 探针),**ACK 并未饿死 CF_Update**;CF_Update 插队修复实测无效果,印证此点。芯片行为(ACK/CF 计数 1:1)在我们实现中同样成立(cf_update_tx ≈ 帧数)。
    - **CF_Update 逐跳语义(用户纠正)**:CF_Update 只回直接邻居(单跳传播),不存在端到端 RTT;此前"3×RTT 退款环路"框架错误,已修正。
    - **✅ 长 RTT 带宽暴跌真根因(FC gate 探针定位,2026-08-24)**:master 的 CBFC **CU(在途信用)假满**——探针实测稳态 CU:VC1=21272、VC2=32709、VC0=4094(窗口均 4096,master 甚至从未发过 VC0)。CU=(S_VC_CC−S_VC_CF) mod 2^15;巨大 CU ⇔ **S_VC_CF(退款)超前 S_VC_CC(计费)约 11496 credits≈3832 帧(多退 ~23%)** → CanSend 假阻塞 → 逐帧等 CF_Update 推进(每帧 ~600ns) → 吞吐 13.6Gbps。prop=100ns 时退款快,超前量尚未积累到 mod 空间翻转点,表现为轻微损耗;长 RTT 下积累更快。**根因方向:某路径双重退款(§12.30 曾修"双重退还会虚增信用",未修全)或 bytes 口径不一致(计费/退款取的帧大小不同)——待下一轮定位具体双退点。**

49. **CBFC CC_Update 双退缺陷修复(§12.48 延伸,2026-08-24)——按 UEC §5.2.9 note 3 语义** → **✅ TestSuite 62/62 + 关键 scratch 全 PASS**。
    - **缺陷**:`RxFrame` 对 GoBackN 乱序帧 `return DROP`(spec §5.1.7)→ NACK 清除(第一个重传帧到达)后,后续重传帧(seq+1..seq+k)仍在途;此间隙到达的 CC_Update 按 §5.2.5.7.4 计算 `LostCredits = S_VC_CC − R_VC_CC > 0`(在途重传未计费)→ 虚退 `R_VC_CF` → 重传帧到达后又正常退款 → **双退** → CU=(CC−CF) mod 2^15 翻转到巨大值(实测 21272/32709,窗口 4096)→ CanSend 永久假阻塞(prop=700ns 时吞吐 13.6Gbps,FC gate 探针实证)。
    - **规范依据(用户指引查 spec)**:UEC §5.2.9 note 3(line 837):"previously discarded LLR-eligible packets **make the CC counter checks invalid**"——只要丢弃帧未全部被重传接受,CC 检查即无效;note 1(line 793)明确"they will be updated later when the packets are accepted during retransmission"。spec 字面 discard window 到"第一个重传帧",但 note 3 的逻辑覆盖整个重传 burst。
    - **实现(最小侵入,不动 spec 字面窗口)**:
      - `SueLlrEngine` 增 discard 跟踪(`m_discardMaxSeq`/`m_discardActive`):missing DROP 时记录最高丢弃 seq;`next_rx_seq` 越过它时清除(全部重传已接受);INIT 重置。公开 `HasPendingDiscard()`。
      - `SueNetDevice` CC_UPDATE 接收分支:LLR 引擎 `HasPendingDiscard()` 时丢弃整条 CC_Update(同 rule 3 窗口内语义,NS_LOG_LOGIC 可观测)。
      - **不动 `RecomputeLlrWindow`**(窗口保持 spec 字面:NACK 清除即关)——rule 3/9 的既有语义和测试(`SueLlrWindowDrivenTestCase`)不变。
    - **实现教训**:两次错误尝试留档——① 扩窗到 guard 时间(50µs):窗口过宽使 rule 9 禁发 CC_Update,84% 时间窗口开 → retries 暴涨;② 扩窗判据直接用 `m_discardMaxSeq` 但初始 0 是合法 seq,首次比较语义错 → 清除条件永不可达 → 窗口卡死。正确做法:首次丢弃无条件记录 + 窗口不扩、只门控 CC_Update 处理。
    - **回归**:TestSuite **62/62**(含 `SueLlrWindowDrivenTestCase` spec 字面断言、CC_Update lost-recovery 专项);thpt clean+loss 双 PASS;**s8-mtp NACK+FLUSH 双等价 PASS**;llr-engine/ctlos-loss/cbfc-credit/fc-coexist 4 单测 PASS;AXI 双臂 PASS(LLR=1 retries=0 / LLR=0 completed=500)。
    - **AXI scratch 附带修复**:`CheckTimeouts` 超时扫描上限 10ms→800ms(覆盖 Stop=1s;LLR=0 长 RTT 下最后 1 笔事务的重试在 10ms 后,无人扫描导致 completed=499)。
50. **AXI workload CU 假满真根因:qIndex/PCP VC 错配(§12.49 延伸,2026-08-24)→ ✅ 带宽 ×2.6,credit 对账清零**。§12.49 的 CC_Update 门控修复正确(spec 合规)但未消除 CU 假满——credit 绝对值对账(AUDIT 输出)揭示真正根因:**scratch 层 6 个发送函数硬编码 `SwitchSend(0, p)`(qIndex=0→VC0 队列),而帧的 VLAN pcp=1/2/3/4 让交换机转发/退款走 VC1/2/3/4 → 计费(master OnTx)在 VC0、退款(RefundCredit)在 VC1 → CU(VC1)=(0−CF) mod 2^15 假满 → CanSend 永久阻塞 → 逐帧等待 → 带宽腰斩**。修复:qIndex=PCP 值(PCP→VC 恒等映射),计费/退款同 VC。
    - **对账实证(修复后,prop=500ns)**:vc1 CC=48250=switch RCC ✓,CF=15482=switch RCF ✓,CU=0 ✓;vc2 CC=250=RCC,CF=250=RCF ✓——信用账目完全平衡。
    - **带宽(prop=100/300/500/700/900ns,LLR=1,loss=1/100,零回滚)**:修复前 110.6/100.7/61.6/13.6/4.8 Gbps → **修复后 290.7/199.8/130.8/14.6/8.1 Gbps**(双向口径,×2.0-2.6)。**ScaleUp 目标 RTT=1µs(prop=500ns)下 130.8Gbps 双向(单向 65.4G=32.7% 线速)零回滚**。prop=1000ns(RTT=2µs,超目标)仍 FAIL(retries=2316)——长 RTT 瓶颈(信用窗 BDP 或退款节奏)待后续。
    - **参数调优**:timeout 200×K×wire+8×prop+200µs → 500×K×wire+16×prop+500µs(长 RTT CBFC 瞬态余量,消除 prop=700ns retries=3);maxRetry 8→16(LLR=0 确定性 SetBadEvery 的多次碰撞,aborted=2→0)。
    - **AUDIT 工具**:scratch 终态打印 master CC/CF/CU 与 switch RCC/RCF(per-VC 绝对值)——credit 对账四值一致是 VC 映射/退款链正确性的直接判据,纳入 scratch 诊断输出。
51. **VC 计数防护体系:防御断言 + 用例 + 对账断言三层(2026-08-24,用户指令"绝对不能再出现 VC 计数映射到错误的 VC")**。§12.50 类缺陷(计费/退款 VC 分裂)的根防:
    - **第 1 层 API 防御断言**:`SueNetDevice::SwitchSend` 内 `NS_ASSERT_MSG(qIndex == ComputeVc(h))`——调用者的队列索引与帧派生 VC 不一致即 abort(debug 构建),把这类"接口两侧语义各自正确、合成错误"的静默 bug 变成响亮失败。**上线即抓到 2 个存量错配**:TestSuite 辅助函数 `SendOne` 硬编码 qIndex=0(shim dscp/VLAN PCP 帧全错配)——该辅助函数服务 S0-S5 约 30 处调用,credit 计费 VC 一直错但用例只断言转发侧所以从未暴露;修复:SendOne 改为按帧派生 VC 入队(shim DSCP > VLAN PCP+DEI > default)。
    - **第 2 层 TestSuite 新用例(62→64)**:
      - `SueVcCreditAuditTestCase`("VC credit audit: multi-VC charge==refund end-to-end"):A↔B 全链路(SueChannel + CBFC 引擎 + CF_Update 流动),VC1-4 经 VLAN PCP、VC5-6 经 shim DSCP 各发 N 帧,断言五值:master S_VC_CC==N×credits(计费)∧ switch R_VC_CC==(计费一致)∧ R_VC_CF==(全额退款)∧ S_VC_CF==(CF_Update 采纳)∧ CU==0(无 mod 翻转);未触碰 VC 零污染。
      - `SueCcUpdateDiscardGateTestCase`("CC_Update dropped while GoBackN retransmits in flight"):§12.49 回归——NACK→missing DROP(discard 跟踪记录最高丢弃 seq)→首重传落地(NACK 清但 pending 仍真)→CC_Update 被门控(R_VC_CF 不变)→全部重传落地(pending 清除)。锁定 §12.49 语义不被回归。
    - **第 3 层 scratch 对账断言**:axi-workload 的 AUDIT 输出升级为硬 CHECK(计费==账单、采纳==退款、CU≤窗口),每臂每 VC 执行——VC 错配/退款链断裂在任何 AXI 运行中立即 FAIL。
    - **回归**:TestSuite **64/64**;axi 双臂×2 prop PASS + audit 0 fail;llr-engine/cbfc-credit/fc-coexist PASS;thpt 双 PASS。

52. **prop≥700ns 带宽断崖根因:坏帧 × LLR GoBackN 交互(2026-08-24 定位)**。二分排除链(AXI_FC/AXI_FWD_NS 旋钮):FC=OFF 与 CBFC 逐位相同(14.58 vs 14.58Gbps)→ CBFC 排除;fwd=0 与 fwd=300 相同 → 交换机转发预算排除;**loss=0 时 prop=700ns 跑到 357.8Gbps(线速级,LLR=0/1 均如此)→ 崩塌只由坏帧注入触发**。
    - **机制**:SetBadEvery(100) 每 100 帧坏 1 → LLR NACK → **GoBackN 接收端丢弃全部乱序帧 + 发送端重传整个 replay 窗口**。RTT 内在途帧数 N ≈ RTT/wire(prop=700ns: 2µs/21.8ns ≈ 92 帧)——每次坏帧的代价 ∝ N,且**重传帧仍过 corruptor(每 100 帧又坏 1)→ 级联放大**。
    - **实测效率**:prop=100/500/700ns + loss=1/100 → 290.7/130.8/14.6 Gbps,对应 loss=0 基线的 81%/36%/4%——**RTT 越长 GoBackN 代价越大**(用户最初洞察的定量印证;这正是 UEC 为何预留选择性重传 AR)。
    - **待查差距**:GoBackN 理论效率 ≈ (1-p)/(1+pN) = 51% @p=1%、N=92,实测 4%——疑重传窗口比 RTT 大(replay buffer 未 ACK 全量)或 NACK 处理延迟放大窗口;下一轮对照。
    - **旋钮**:AXI_FC(CBFC|OFF)、AXI_FWD_NS(转发预算)入 scratch,供二分诊断。

53. **GoBackN 放大源闭环:REPLAY 中 NACK 被忽略 × replay_timer 过大(§12.52 延伸,2026-08-25)** → **✅ conf 改 auto(10×RTT),prop=700ns 14.6→38.4Gbps**。
    - **机制链(代码+规范双重确认)**:`OnAckNack` 的 `if (m_tx == REPLAY) return` 使 REPLAY 期间的新 NACK 被忽略——重放中又坏帧(92 帧窗口 ~0.92 个)只能等 replay_timer 到期再重放。**这符合 UEC §5.1.4**:line 275 nack_received "set to FALSE ... upon entry to the REPLAY state"(NACK 驱动重放进入,重放中是 timer 驱动);GoBackN 在高 RTT×高误码下的级联代价是协议固有的。
    - **真正的缺陷是 conf**:axi 硬编码 `LLR_REPLAY_TIMER_MAX_NS 50000`(50µs),而规范 line 276 明确 "**should count 10x the expected LLR round trip time**"(RTT≈2µs → 20µs)。50µs = 25×RTT,每轮重放间等待 50µs → 每个坏帧恢复 ≈ 12 轮×50µs ≈ 600µs(几何级数)→ 效率 4%。
    - **验证(AXI_REPLAY_TIMER_NS 扫描,prop=700ns,loss=1/100)**:50µs→14.6 / 20µs→31.1 / 10µs→49.8 / **5µs→71.0 Gbps**——timer 线性主导确认。**修复**:默认改 0(auto = 10×RTT,§12.27 机制)→ **38.4Gbps(×2.6)**;prop=100/500ns 不受影响(290.7/130.8 不变,短 RTT 下 timer 本就不是主导)。
    - **取舍**:5µs 更快(71Gbps)但 < RTT 时 ACK 在途误超时会引发无谓重放(流量放大);auto 10×RTT 是规范推荐的安全值。AXI_REPLAY_TIMER_NS 旋钮保留供扫描。
    - **回归**:6 场景(2 臂×3 prop)全 PASS,audit 0 fail,LLR=1 零回滚。

54. **VRF 实验需求变更:IO Die 编址,隔离域 32→8 平面(2026-08-27)** → **✅ sue-vrf-planes 重写 + 全臂重验(PASS)**。
    - **需求**:实物 8 卡机每 GPU 的 32 口由 4 个 IO Die 提供(Die 占连续 8 口,口序 k=die*8+stripe),且 Die ID 编址进 XPU-ID——目的地址自含 Die 信息,交换机不再需要靠入口端口位置消歧出口列,隔离域从 32(VRF 按口列)收缩到 8。
    - **新映射**:`XPU-ID = gpu*4 + die`(0..31);`PORT_TO_VRF_MAP: p%8+1`(每个 Die 内同 stripe 的口同平面);平面 v 表:`x=(j,d) -> 口 j*32 + d*8 + (v-1)`——出/入同一 stripe,帧在哪个平面进入就在哪个平面出去。SPLIT 变体(§12.45 v2):按 stripe 拆成 8 台 32 口平面交换机(node 8..15 = 16 LP;平面机设备序 = i*4+die,VRF0 表 `x -> 端口 x`)。
    - **负载守恒**:pairFrames 对齐 128 帧的整数倍;流粒度改为 (src口, dst gpu, dst die)、每流 N'=pf/128 帧,节拍 ×28(7 对端 × 4 目的 Die 并发轮流占线速)——与旧 32 平面口径相同 offered load。
    - **断言升级**:逐-(出口口, 源 XPU-ID)cell == N'(自身 GPU 的源 ID 置零;几何上每条流的全部帧落在唯一出口),逐平面 == 896N',逐端口 == 28N',折叠 (dst-gpu, src-gpu) == pairFrames;全部整数精确。
    - **结果(2026-08-27)**:默认 CBFC 臂 PASS——aggregate=48,658.4 Gbps(**99.31%** 天花板),replay/discard/nack=0;PFC A/B 四档与 MTP 等价五臂(BHASH 逐字节一致)同日重验(`logs/vrf8-*`)。§12.41–45 记录的历史数字为 32 平面口径,保留作演进记录;对外表述自本条起以 8 平面为准。

55. **LLR 重传环 RTT 口径修正 + 五档链路时延扫描(2026-08-27)** → **✅ 五档全 PASS,梯度放缓(v2)**。
    - **口径修正(需求方确认)**:LLR 的 ACK/NACK 由邻接接收端就地生成并原路返回,不进芯片流水线——实现证据:`SueNetDevice::Receive` 经 `HandleCtlFrame` 就地终结 CtlOS(sue-net-device.cc:951 起),fwd 预算仅由 `SUESwitchNode::SendToDev` 施加于被转发数据帧。故 LLR 回环 **RTT = 2×链路时延**,此前"含 fwd 300ns"的表述仅适用于 CBFC/E2E 口径。
    - **重测**(AXI workload,LLR=1,loss∈{0,1/100},链路时延 {20,100,200,300,500}ns,payload/负载同 §12.47):丢 1/100 吞吐 335.9 / 290.7 / 241.6 / 199.8 / 130.8 Gbps,效率 92/80/67/55/36%(各对自身基线);基线漂移仅 ±1%。历史三档(§12.52–53)中 prop=100/500 两格与本轮逐位一致。
    - 演进含义:横向对比 CBFC(退款环含 fwd,E2E 口径)与 LLR(逐链路口径)时不可混用同一 RTT 数值。
    - **附:LLR 前导带宽口径**(2026-08-27 追记):UEC 将序号承载于既有以太网前导区,端口带宽(MAC 口径)不为其额外计费;当前仿真以包内 8B 头串行化并计入线时,属保守建模——对外报表统一双口径披露(线时口径 +0.39pp,MAC 净开销≈ACK ~0.2pp)。若需实现层零成本计时需评审后另行处理。

56. **CBFC CreditSize 异粒度(双端 MMU cell 不一致)+ UEC §5.2.5.8 method-2 对账(2026-08-31)** → **✅ 引擎双粒度落地,四臂场景全 PASS**。专题文档(原始问题/方案/反事实对比 A.rx=32 的影响):`docs/cbfc-credit-size-design.md`。
    - **需求**:直连两端 MMU cell 不一致(设备 A 原生 128B、仅支持 2 的幂;交换机原生 56B、不可配)。协议建议双向取 min(=32) 会让交换机侧 56B cell 记账失真、缓存浪费且可能掉出线速。方案(标准背书):**每方向以 RX 端原生 cell 为准**——A→SW 方向 A.tx snap 到 32(≤56 的最大支持值,UEC §5.2.8.2 保守方向硬规则),SW 按 56 记账;SW→A 方向 SW.tx=A 原生 128。A 每 32B 记 1 CC,SW 每 56B 记 1 CF,**不足一个 cell 的报文也占满一个信用**;粒度差 `ceil(F/32)-ceil(F/56)` 从 A 的 CU 泄漏,由周期 CC_Update 对账回收(§5.2.5.8 method-2:LostCredits = S_CC − R_CC → R_VC_CF,基线重置)。
    - **实现**:`SueCbfcEngine` 拆双粒度(`m_txCellSize`/`m_rxCellSize` + `SetTxCellChoices` 支持集 + `AdoptTxCellFromPeerRx` snap-down);`PktCredits`(发送侧)/`RxPktCredits`(接收侧)分轨,`OnRx/OnDrain` 走原生 rx cell——`OnCcUpdate` 对账公式零改动(粒度差被既有"泄漏回收"语义自然覆盖)。LLDP 语义修正:广播值 = 本端 rx 原生 cell;采纳只写 tx、**不再覆写 rx**(旧代码 `SetCreditSize(remote)` 会把接收端自己的 cell 也改掉)。新 GlobalValue `CBFC_TX_CELL_CHOICES`(CSV 支持集)。`GetCreditSize()` 语义改为返回 rx cell(= LLDP 广播值 = auto egress buffer 的记账基准,两处调用点语义自动正确)。CU=CC−CF 差分代数与 mod 2^15/2^20 回卷**零改动**。
    - **规范要点**:TotalCredits 必须按 SW 的 56B cell 数配置(buffer÷56,非 buffer÷32),否则 A 误以为有更大窗口而超发(§5.2.8.2);A 侧 PktOvhd ≥ SW 实际 per-packet overhead(保守方向)。15 位 CF 计数器安全约束:在途 credits = RTT×速率÷cell < 16384(半模)——scale-up DAC(RTT~1µs)安全,长距互联须校核。
    - **四臂场景**(`scratch/sue-cbfc-cellsize.cc`,100G/1518B/窗口 4096 credits):a) min() 32/32 基线 100.36Gbps;b) 方案 32/56+CC(10µs)100.31Gbps(CU 锯齿 2388 峰,299 个 CC 帧);c) 反例 32/56 无 CC——**硬停**:仅送出 203 帧(≈ CL/(1−56/32 折算))后 CU 卡死 4060/4096,B 排空后再无返还来源,泄漏**不可自愈**(比"掉到 57%"的直觉更严重,定量钉死"CC 周期必须满足 dT ≤ W−BDP"约束);d) 理想 56/56 100.36Gbps。SW cell 峰值计数 a=432 vs d=252(**min() 口径放大约 71%** = 缓存浪费的定量)。
    - **回归护栏**:rx==tx 时与旧单 cell 引擎逐位一致(TS-63 双引擎对照);67 例 TestSuite 全绿(新增 TS-62~65:48/28/20 逐帧走账、snap 语义、等价性、e2e 对账收敛);LLDP 采纳用例断言更新为"tx 采纳 128 / rx 保持 256"。

## 附录 A:类依赖图

```
SUESwitchNode ──uses──> SueHeader, SueEgressQueue, SueRoutingTable (AFH 计数器为裸成员,§6.6)
SueNetDevice ──has──> SueLlrEngine, SueFcController, SueEgressQueue, SueErrorModel, SueLlrCounters, SueCbfcCounters
SueNetDevice ──attached-to──> SueChannel
SueChannel ──dispatch──> &SueNetDevice::Receive (静态分派,仿 QbbChannel)
SueFcController ──has──> SueCbfcEngine | SuePfcEngine
SueNetDevice ──extends──> PointToPointNetDevice (upstream;自研 TX/RX 管线)
SueChannel ──extends──> PointToPointChannel (upstream)
SUESwitchNode ──extends──> Node (upstream)
SueLiteEndpoint ──extends──> Object (非 Application,§12.21;MTP 驻留) ──uses──> SueNetDevice, SueHeader
Headers(SueLlrPreamble/SueHeader/SueShim/SueLlrCtlOs/SueCbfc/SuePfc) ── 独立(不依赖 CustomHeader)
```

## 附录 B:规范条款对照
| 功能 | 规范 | 实现处 |
|---|---|---|
| AFH Gen1 转发 | SUE §3.5.3, AFH §6.6 | SueHeader, SUESwitchNode |
| shim 字段 | AFH Table 29 | SueShimHeader (§5.3) |
| XPU-id 高 10 位 LPM | SUE §3.5.3 | SueRoutingTable (§6.9) |
| LLR preamble/CtlOS/状态机 | UEC §5.1 | SueLlrEngine + SueLlrPreambleHeader |
| LLR 异常 | UEC §5.1.5/.7/.10 | §6.10 |
| LLR 计数器(22) | UEC §5.1.11 Table 5-13 | SueLlrCounters (§6.11) |
| LLR 保真度 | UEC §5.1(INIT 握手/超时/老化) | FLUSH→INIT 等 INIT_ECHO;独立 `m_initTimerMax`/`m_initRetryMax`;独立 buffer 驱动 `m_dataAgeTimer`(DoReplay 内嵌检查兜底);无对端引擎 INIT 超时×retry → GoAdvance(§6.2) |
| LLR 链路保真 | UEC §5.1.1.1(数据帧恒 8B preamble)/§5.1.6(ACK 周期)/§5.1.7(next_rx_seq_vld)/§5.1.9.1.2(INIT_ECHO 校验)/Table 5-10(acked_seq/outstanding_seq/LLR_status/pcs_lost/replay_ct)/§5.1.4(replay_timer 规则 4) | ineligible preamble 仅 B1=0x55;ACK 周期 = `ctlos_target_spacing` 线时间(钳制 [400,16384],init_echo_sent 触发 SEND_ACKS;与 pacing 门解耦,§12.26);`next_rx_seq_vld` 未置→DROP 不计数不 NACK;INIT_ECHO `init_seq==tx_seq` 才 ADVANCE;acked_seq 初值全 1 / mode 改变全 1 / INIT 置 init_seq-1;outstanding_seq = `((tx_seq-1)-acked_seq) mod 2^20`;LLR_status=mode_remote ON 且 ADVANCE/REPLAY;最小 PCS 模型 pcs_lost_status_timer + 设备 PCS down 丢帧;replay_ct_max=255 永不触发 FLUSH;replay_timer = ACK 超时检测器,ACK 部分释放缓冲帧时重置(规则 4,§12.24),`LLR_REPLAY_TIMER_MAX_NS` 按 10×RTT 配置(§12.25)(§12.15/§12.16/§12.18/§12.24/§12.25) |
| CBFC 计数器/伪代码 | UEC §5.2.5 | SueCbfcEngine |
| CBFC 报文 | UEC §5.2.6 | SueCbfcHeader; SueCbfcCcHeader 已按满 64B 以太网帧落地(20B 帧头 + 40B 打包 16×20b CC + 4B CRC-32 FCS,§5.6) |
| LLR+CBFC 共存 | UEC §5.2.9 | 九规则表(§5.2.9);规则 3/9 **已实现**(`OnCcUpdate` 与 `SendCtlCcUpdate` 的 `m_inLlrWindow` 守卫;唯一事实源 `SueCbfcEngine::m_inLlrWindow`,由 LLR 引擎状态驱动,§12.31 B2) |
| FC 门面 | 统一流控 API | `SueFcController` 已接入设备数据路径(RX/OnDrain/gate/dequeue/Suspend 双模式分发;`m_fc==nullptr` 保持 legacy 逐字节不变,§6.5) |
| PFC 帧格式 | IEEE 802.1Qbb, SUE §3.5.6 | SuePfcHeader (§5.10), SuePfcEngine |
| PFC 阈值算法 | SwitchMmu(port) | 算法已内联进 `SuePfcEngine`(8 优先级 pause/resume + 共享池动态阈值;`sue-mmu` 为后续工作,§4.1) |
| 配置系统 | SimAI.conf 风格 | `SueReadConf`/`ApplySueConfig` + GlobalValue(§9) |
| 入口分发/拓扑 | SUE 集成契约 | `SetupSueNetwork`/`SueEntryDispatch`(`network_frontend/sue-entry`,§6.8) |
| 自研管线 + 专用通道 | qbb-net-device/qbb-channel 范式 | SueNetDevice (§6.1), SueChannel (§6.13) |
| 链路协商 | UEC §5.3 + §5.2.8.4 | `SueLldpHeader`(语义最小模型)+ `SueNetDevice::StartLldpExchange/OnRemoteLldpTlv`;`LLDP_NEGOTIATE=1` 时能力 TLV + RTS/RTR 握手(§12.32;字节布局属管理面不复刻) |
