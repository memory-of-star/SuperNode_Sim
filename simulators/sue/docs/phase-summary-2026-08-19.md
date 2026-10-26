# SUE Scale-Up 仿真平台阶段性总结——报文级观测能力(2026-08-19)

> 承接 [phase-summary-2026-08.md](phase-summary-2026-08.md)(基线 2026-08-17,TestSuite 61 例 / scratch 47 判语全 PASS)。
> 本篇覆盖其后增量:**交换机 VRF 演进(§12.40→§12.45 提交链)**、MTP 数据补齐(a74ea41:smoke 128/256 机 MTP-8 加速 **2.14×/2.53×**,§12.46 新口径)、**旁路抓包工具与 Wireshark 解析插件(含 tshark 4.6.4 全量回归 9718 帧/0 失败)**,以及由此获得的四组协议行为实证。
> 入库状态:抓包工具第一版(`38703f3`)与本篇所述其余修订(工具扩展、插件、回归脚本、稳态场景、本篇文档)均已提交归档,清单见 §6。

## 1. 旁路抓包工具(`scratch/sue-scratch-pcap.h`)

**设计原则**:零侵入旁路——`SueScratchPcapAttach(ch, tag)` 一行挂接,`SUE_PCAP` 环境开关**默认关闭**(关闭时纯 no-op,回归已验证 PASS 且零文件产生);捕获点为 `SueChannel` 的 `TxRxSue` trace source(每帧上线回调,含收发设备/线时/传播)。标准 `EnablePcap` 抓不到 SUE 流量(数据面绕过基类收发路径),必须走本工具。

**输出**:每链路一对文件——`.pcap`(标准 pcap)+ `.txt`(人类可读摘要,时间/收发设备/长度/字段级解析/前 32B hex)。`SUE_PCAP_DIR`/`SUE_PCAP_PREFIX`/`SUE_PCAP_DLT` 可配。

**字段级解析**(与接收侧同一套 `Header::Deserialize` 代码,copy+RemoveHeader 链,非手工抠字节):

| 帧型 | 解析内容 |
|---|---|
| 数据帧 | 8B LLR 前导(eligible+20bit seq,B0=0xFB/B7=0xD5 判别)+ AFH(DA/SA+XPU-id、VLAN PCP/DEI/VID、EtherType、shim AR/F/C/CNM/TTL/DSCP/ECN/FlowLabel) |
| LLR CtlOS | ACK/NACK(seq)、INIT/INIT_ECHO(init_seq+init_data) |
| CF_Update | 两条 (VC 5bit, CF_count 15bit) 条目 |
| CC_Update | 真以太 64B:CID + 16×20bit 逐 VC CC 计数 + FCS |
| PFC / LLDP | PEV+逐 VC quanta / 类型名 |

**DLT 两档**(`SUE_PCAP_DLT`):`en10mb`(默认;纯 AFH 链路 Wireshark 原生看 eth/VLAN)/ `user0`(147;前导/CtlOS 等非以太帧唯一可解析方式,配合插件)。

**完整交互过程样例**(已挂接 `sue-cbfc-cc-update-onwire`,195 帧):

| 阶段 | 线上事件 |
|---|---|
| LLR 握手 | 双向 INIT + INIT_ECHO |
| 数据+损坏 | 数据(seq=0,带前导)→ NACK(2²⁰−1)→ 重传 ×2 |
| FLUSH 重同步 | INIT(init_seq=1)+INIT_ECHO(泄漏 1 信用) |
| 泄漏可见 | 周期 CC_Update cc:vc0=1(发送端消费计数) |
| 恢复 | 重同步后数据 seq=1/2 → ACK;CF_Update vc0_cf=2→3(信用返还) |
| 追回 | 尾部 CC_Update cc:vc0=3(与用例断言 S_CC=3/CU=0 吻合) |

## 2. Wireshark 解析插件(`tools/sue-wireshark-plugin.lua` + `tools/README.md`)

- **覆盖全部自定义格式**(§1 表),免编译 Lua,放入个人插件目录重启即用;
- **两种接入**同时生效:DLT_USER0(需一次性 首选项→Protocols→DLT_USER 把 User 0 payload 设为 `sue`;插件加载时也机会性自动注册)+ EtherType 0xFCF1/0xFCF2(普通 pcap 无需配置);
- **shim 有无按 EtherType 做成偏好项**(默认 0xFCF1 无/0xFCF2 有,与 `SueConfig` 默认一致);
- 仅用 Lua 5.2 兼容语法(无位运算符),20bit 连续位打包纯算术实现;
- **装机实测修复两轮**:① Proto 描述唯一性(Wireshark 拒绝同描述双 Proto);② DLT_USER 走 `dlt_user` 表而非普通 `wtap_encap` 表(错误信息即提示该路径)。现已实际可解析。

**验证方法论与结果**(两级):
- **位级交叉验证**(开发期):Python 按插件同一套位级规则跑实抓 pcap,与 C++ 工具输出(与接收侧 Deserialize 同源)逐字段比对——195 帧完整交互 + 4 帧 shim 变体零失败;
- **tshark 4.6.4 全量回归**(2026-08-19,装机后):`tools/sue-plugin-regression.py` 驱动 `tshark -T fields` 抽取全部 `sue.*` 字段,6 套 pcap(afh EN10MB / 完整交互 / 稳态 / 2KB ACK / DSCP-VC / PFC)× **9718 帧全量比对,失败 0**;PFC 分支(此前唯一未见真字节的路径)经 pfc-gating 抓包验证;5 类显示过滤器(`sue.llr.seq`/`sue.cc.vc`/`sue.ctlos.type`/`sue.afh.shim.ttl`/`sue.pfc.quanta`)计数全部符合预期;EN10MB 路径的 VLAN 顺带用原生 `vlan.id` 交叉验证。USER0 经 `-o uat:user_dlts:...` **运行时注入,无需任何持久化首选项**。
- 回归中发现并修复 2 处插件问题:① 位工具函数返回浮点子类型致 info 列显示 "0.0"(math.floor 收尾);② CC/PFC 控制帧被误加 `afh.dst_xpu/src_xpu` 字段(组播 MAC 无 XPU 语义,已改为仅显示 MAC)。装机后另修复 2 处加载/路由问题(Proto 描述唯一性、DLT_USER 走首选项路径)。

## 3. 交换机 VRF 演进(本周提交链,git:7a87ca3→c931034)

| 提交 | 内容 | 关键结论 |
|---|---|---|
| `7a87ca3`(08-17) | **交换机 VRF 转发域(§12.40)**:入口三元组 **DSCP>VLAN>port** 派生 VRF(帧不携带 VRF),每 VRF 一张 LPM 表(`SetRoutingTable(vrf, rt)`),默认 VRF0;TestSuite 专项 `SueVrfTestCase`(分段装配/优先级阶梯/miss 计数) | 三源映射与 §7 VC 映射同构,查表键 (XPU-id, VRF) |
| `82b5278`(08-17) | **VRF 32 平面拓扑**:8 GPU×32×200G 单 256 口交换机 + all-to-all 验证(scratch `sue-vrf-planes` = SC-34,判语 44→46);寻址模型:帧 dst 只带 XPU,端口经 `PORT_TO_VRF_MAP` 定平面,VRF0 空表(全部流量命中 VRF1..32,miss=0) | §12.40 语义的直接应用;暴露 §12.41 延迟退款缺陷(已修 + TS-61) |
| `2a681d8`(08-17) | **CBFC vs PFC 同量 A/B(§12.43)**:同一 all-to-all 数据量换流控 | **PFC 生效即更差**;仅 drain=0(PFC 实质未生效)时与 CBFC 持平 |
| `91d5c22`(08-18) | **VRF 拓扑 MTP 适配与四臂等价(§12.44)**:`run-vrf-mtp.sh`,LLR=1+CBFC / LLR=0 / PFC drain=0 / PFC drain=2000 四臂 | 1/2/4/8 线程 **BHASH 逐字节一致**,聚合速率同为 99.8327% |
| `c931034`(08-18) | **VRF 平面拆 LP:SPLIT 拓扑分解变体(§12.45)** | **MTP-8 首次正加速 1.89×**;边界:32 颗 8 口芯片 vs 1 颗 256 口芯片是两种硬件口径——验证 VRF 机制必须用默认臂,SPLIT 臂只用于并行收益论证;两者一致性本身即"VRF 隔离可分解"的实证 |

机制细节(三源映射、TS-61)与上周总结 §1.1/§1.5 交叉;本表为提交链视角的完整梳理。相关 MTP 背景:VRF 拓扑无倒退的原因是 conf 写了 `SUE_LINK_PROPAGATION_NS 10`(lookahead 有界),与 64 机 fat-spine 的 §12.46 倒退是同一机制的两面。

## 4. 协议行为实证(抓包直接回答的四个问题)

### 3.1 CC_Update 与 CF_Update 的稳态比例

- 压力用例(`cc-update-onwire`,CC 周期被手工压到 5µs)中 CC:CF = 179:2,系**用例构造**(目标是在 1ms 窗口内验证泄漏恢复),非常态;
- 稳态场景(真实预置节奏:CC 102µs / CF_min 10µs / CF_max 100µs @100G,300 帧连续流量):**CF 103 : CC 12 ≈ 8.6:1**——CF 是主信用返还路径(事件驱动),CC 是慢速周期对账,符合 UEC §5.2.6 设计;
- 抓包同时实证 **规则 9**(replay 窗口禁发 CC):5µs 定时器在 0–100µs 窗口内的 20 次触发全部被抑制,CC 从 105µs 起步,179=(995−105)/5+1 **精确吻合**。

### 3.2 VC 映射不依赖 VLAN(三源查表)

VC 不是报文字段,逐跳推导:`shim DSCP → VLAN PCP → DEFAULT_VC`(`SUESwitchNode::ComputeVc`/`SueNetDevice`/`SueLiteEndpoint` 三处同一张 `SueConfig` 表,端到端一致;默认直通 dscp&0x07 / pcp 恒等 / 兜底 0)。抓包实证:`STEADY_DSCP=5` 下 CF_Update 信用返还全部落 **vc5**(vc5_cf=1→6→11…),全程无 VLAN 参与;此前进度抓包全 vc0 是"无 shim 无 VLAN → DEFAULT_VC=0"的兜底路径。

### 3.3 ACK 节奏的两种模式

- **引擎默认(逐帧)**:`m_ackPeriod==0` 时每帧即时 ACK——手工 scratch 未配周期时的回退行为,满速率下会超 CtlOS 带宽预算,测试专用;
- **配置路径(UEC §5.1.6 SEND_ACKS)**:`LLR_CTLOS_TARGET_SPACING 2048B`(Table 5-9)驱动**累积 ACK 周期**(空闲也按节奏重发,发送端去重),叠加设备级 400B CtlOS pacing 门(§5.2.11.2;两个独立量,§12.26)。实测 2050B@100G=164ns 节拍:300 数据帧 → 7621 ACK,窗口内 ACK 严格 164ns 一拍、seq 停在累积值重复至下一帧到达。走 conf 的场景符合标准,逐帧 ACK 仅存在于手工单测。

### 3.4 CF_Update 双触发

- **增量**(UEC §5.2.6.1 触发 1):R_VC_CF 变化置脏,CF_min 到点只报脏 VC,**每条 8B 载荷最多打包 2 个 (VC,CF) 条目**(`SendCfUpdateBatch` 取掩码最低两 bit);
- **周期全量**(触发 2):CF_max 到点报全部 8 VC = **4 条背靠背**(丢 CF 自愈),并吸收 pending 的 CF_min 刷出(取消定时器+清脏掩码)。

## 5. 稳态抓包场景(`scratch/sue-cbfc-steady-capture.cc`,已入库)

单链路全栈(LLR+CBFC 无误码),节奏全部取真实预置;环境开关:`STEADY_DSCP`(shim DSCP→VC)、`STEADY_ACK_SPACING_BYTES`(2KB 累积 ACK 节拍);自带断言(全达/ADVANCE/PASS)。注:`endpoint-ring` 的 CC 定时器被 MTP 等价判据显式关闭,故稳态演示用独立场景。

## 6. 状态与开放项(诚实披露)

**本篇修订已提交归档**:
1. `scratch/sue-scratch-pcap.h` 扩展(控制帧字段级解析 + SUE_PCAP_DLT);
2. `scratch/sue-cbfc-cc-update-onwire.cc` / `scratch/sue-pfc-gating.cc` 挂接(各 2 行);
3. `tools/`(Wireshark 插件 + README + 回归脚本 sue-plugin-regression.py);
4. `scratch/sue-cbfc-steady-capture.cc`(稳态调试场景,未纳入 run-scratch-checks 回归——若需常驻判语再按惯例收编)。

**归档后新增(待下次提交)**:
- `src/sue` 控制帧 MACSA 单播修复(`GetMacSa` 对非单播默认地址合成 `02:00:00:<ifIndex>:<nodeId>`,IEEE 802.3 3.2.3(b) 合规,原生 Wireshark 组地址 SA 告警消除;9718 帧回归重跑零失败);
- `tools/cc-update-wireshark.lua`:CC_Update 独立解析脚本(postdissector,`suecc.*` 前缀,EN10MB pcap 零配置免全套插件);
- 实测结论:原生 Wireshark 4.6.4 即可完整解析 PFC(0x8808/0x0101,"Class Based Flow Control"),无需 SUE 插件(分工表见 tools/README.md)。

**已知限制**:
- 插件已在 tshark 4.6.4 全量回归(9718 帧/0 失败);其余 Wireshark 版本(尤其 Lua 5.2/5.3)未逐一验证,语法已按 5.2 兼容收敛;
- USER0 pcap 三种打开方式:装插件后 `-o` 运行时注入(零持久化,回归脚本内置)/ 持久化 `~/.config/wireshark/user_dlts`(6 字段格式 `"User 0 (DLT=147)","sue","0","","0",""`)/ GUI 一次性配置;插件的机会性自动注册在 4.6.4 上不生效(首选项路径优先);
- MTP 多线程下抓包文件写入有全局锁但跨 LP 写序不确定,排障建议单线程;
- pcap 时间戳为发射开始时刻(µs 精度);LLDP 仅类型名解析(评审决定不扩展)。

**后续候选**:稳态场景入库;若需离线重放口径可考虑记录到达时刻的副输出。(LLDP 字段级解析经评审决定**不做**,保持类型名解析。)

---

数据与文档索引:抓包工具 `ns-3-alibabacloud/simulation/scratch/sue-scratch-pcap.h`(头注释含用法);插件 `tools/sue-wireshark-plugin.lua`/`tools/README.md`;机制依据 [DESIGN.md](../DESIGN.md) §5.2.6/§5.2.9/§5.6/§7/§12.26/§12.37;上篇总结 [phase-summary-2026-08.md](phase-summary-2026-08.md)。
