# UEC spec 功能点矩阵(2026-08-13)

> 把 UEC 5.1(LLR)/ 5.2(CBFC)/ 5.3(链路协商)中**可仿真**的功能点逐一拆出、实现并验证。
> 状态:✅ 已实现且已测 / 🟡 已实现未专测(需补单测)/ ⏭ 可实现待实现 / ⚪ 不入仿真(物理层/管理面/有意建模简化——如 §12.11 CtlOS 子优先级、§12.32 非理想信用大小,仅记录)。
> 验证载体:scratch 单测(`sue-<模块>-<功能点>`,命名见 `test-organization.md`);TestSuite 负责 CI 防回归。
> 单测文件名与本表 ID 对应(如 LLR-SP-07 → 在 `sue-llr-nack-retransmit` 中断言)。

## 5.1 LLR

| ID | 条款 | 功能点 | 状态 | 验证 |
|---|---|---|---|---|
| LLR-SP-01 | 5.1.1.1 | 每帧 8B LLR preamble;eligible 帧 B1..B3 携带 seq,非 eligible 用标准 preamble | ✅ | sue-llr-preamble-fcs |
| LLR-SP-02 | 5.1.2 | 帧结构接口:SueHeader/XPU-id/shim | ✅ | sue-afh-header-parse |
| LLR-SP-03 | 5.1.3 | CtlOS 建模(LLR_INIT/INIT_ECHO/ACK/NACK;9B 近似格式,偏离已记录) | ✅ | sue-fc-ctl-onwire(CtlOS 节流段;PFC/CC_Update 64B 帧段见 sue-fc-frame-headers) |
| LLR-SP-04 | 5.1.4 | 两相使能 llr_mode_local/remote + 三行为 DISCARD/BLOCK/BEST_EFFORT + re_init_on_discard | ✅ | TestSuite LLR modes-behaviors |
| LLR-SP-05 | 5.1.4 | 定时器:replay_timer/data_age_timer/pcs_lost_status_timer/init_timer(0=自动折算 10×RTT) | ✅ | sue-llr-timer-wakeup / cfg 路径 |
| LLR-SP-06 | 5.1.5 | TX 状态机 OFF/INIT/ADVANCE/REPLAY/FLUSH;ADVANCE 才放行新帧(outstanding 三门限) | ✅ | TestSuite LLR can-tx-limits / flush-stays-flush |
| LLR-SP-07 | 5.1.5 | NACK → REPLAY 重放;重放完成 → ADVANCE;pause 不阻止重放(UEC 明文) | ✅ | TestSuite LLR nack-retransmit / replay-during-pfc-pause |
| LLR-SP-08 | 5.1.6 | ACK 周期:init_echo_sent 触发 SEND_ACKS,累积 ACK(next_rx_seq-1),空闲重复,NACK_SENT 抑制 | ✅ | TestSuite LLR ack-period |
| LLR-SP-09 | 5.1.7 | RX 过滤:next_rx_seq_vld、expected/missing/duplicate 判定(半空间判据)、NACK 单发 | ✅ | TestSuite LLR vld / ineligible-preamble |
| LLR-SP-10 | 5.1.8 | 收到 ACK/NACK 处理:ok 公式、释放重放缓冲、重复 ACK dup-skip、replay_timer 规则 4 重置、部分释放唤醒 TX | ✅ | TestSuite LLR acked-seq / partial-ack-wake |
| LLR-SP-11 | 5.1.9.1.1 | LLR_INIT 发送与 init_seq 选择;acked_seq=init_seq-1;重试上限后 GoAdvance 兜底 | ✅ | TestSuite LLR init-handshake |
| LLR-SP-12 | 5.1.9.1.2 | INIT_ECHO 与 init_seq 匹配校验 | ✅ | TestSuite LLR init-echo-seq |
| LLR-SP-13 | 5.1.10 | 错误传播:POISONED 路径、frame_bad 上报、BAD 帧 NACK | ✅ | sue-llr-poisoned |
| LLR-SP-14 | 5.1.10 | 闪断模型:最小 PCS 模型,pcs_lost_status_timer → FLUSH(短/长闪断区分) | ✅ | sue-llr-pcs-lost |
| LLR-SP-15 | 5.1.11 | 22 项计数器(UEC Table 5-13 语义;tx_replay=REPLAY 退出;tx_replay_frames debug) | ✅ | sue-fc-counters |
| LLR-SP-16 | 5.1.11 | rx_ack/rx_nack 对超范围/重复 CtlOS 也计数(收发配平) | ✅ | TestSuite LLR ACK-NACK counters pair across link |
| LLR-SP-17 | Table 5-10 | replay_ct 仅 ACK/INIT 清零;replay_ct_max=255 永不 FLUSH | ✅ | TestSuite LLR replay-ct-unlimited |
| LLR-SP-18 | 5.1.4 | 重放缓冲三门限 storage/BDP/window + RTT 换算 | ✅ | sue-llr-replay-buffer |
| LLR-SP-19 | Table 5-9 | 寄存器范围:ctlos_target_spacing [400,16384]、outstanding ≤ 524288 | ✅ | cfg 钳制 + TestSuite |
| LLR-SP-20 | 5.1.9.1.5 | CtlOS 发送优先级(Table 5-12 子优先级:INIT_ECHO=INIT>ACK=NACK>CF_Update) | ⚪ | 建模简化(DESIGN §12.11):单 FIFO + 400B 门,无子优先级;CF_Update 排在 ACK/NACK 后为预期规格行为(§12.26 解耦后无积压病理)。§12.32 正式标 ⚪ |
| LLR-SP-21 | 5.1.9.1.3/4 | ACK/NACK CtlOS 线格式(D1[7:4] seq 布局,偏离已记录) | ⚪ | 已记录为有意近似 |
| LLR-SP-22 | 5.1.5 | 错误注入对控制帧的覆盖(NACK/ACK/INIT 丢失容错条款) | ✅ | sue-llr-ctlos-loss(S1a 节奏恢复/S1b FLUSH 链/S2 NACK/S3 INIT_ECHO/S4 INIT) |
| LLR-SP-23 | 5.1.4 | data_age_timer 按最老帧年龄重排(与 UEC 规则 1/3 的建模近似) | ✅ | sue-llr-replay-buffer(C2) |
| LLR-SP-24 | 5.1.7 | BEST_EFFORT/LLR_OFF 帧:ineligible preamble、RX 不过滤 | ✅ | TestSuite LLR ineligible-preamble |

## 5.2 CBFC

| ID | 条款 | 功能点 | 状态 | 验证 |
|---|---|---|---|---|
| CBFC-SP-01 | 5.2.4.2 | 环回计数:CC 2^20 / CF 2^15;CU 按 CF_MOD 环差;LostCredits 半量程判定 | ✅ | TestSuite CBFC engine-credit / lost-credits-guard |
| CBFC-SP-02 | 5.2.5.2/5.2.8 | 配置:creditSize/pktOvhd/VC credit limit(≥1,CL==0 回填告警) | ✅ | sue-cfg-parse-apply |
| CBFC-SP-03 | 5.2.5.6.1 | 发送信用检查:CU+pkt ≤ CL 才发;credit_exhausted 计数 | ✅ | TestSuite CBFC engine-credit |
| CBFC-SP-04 | 5.2.5.6.2 | 发送计数:S_VC_CC += PktCredits(不含 preamble);重放帧不计(规则 1) | ✅ | sue-fc-llr-cbfc-coexist |
| CBFC-SP-05 | 5.2.5.6.3 | CF_Update 接收:S_VC_CF 累计直写 | ✅ | TestSuite CBFC engine-credit |
| CBFC-SP-06 | 5.2.5.6.4 | CC_Update 生成:周期 CC_msg_timer + 64B 真实帧 + 15b+5b VC 布局 | ✅ | sue-cbfc-cc-update-onwire |
| CBFC-SP-07 | 5.2.5.7.1 | 接收计数:R_VC_CC +=;类型 B(LLR-ineligible)窗口内仍计(Table 5-26) | ✅ | TestSuite FC llr-window-driven |
| CBFC-SP-08 | 5.2.5.7.2 | 排空计数:R_VC_CF +=;drainDelay 建模;defer 退还(§12.30) | ✅ | sue-cbfc-e2e-gating / sue-incast-2to1 |
| CBFC-SP-09 | 5.2.5.7.3/5.2.6.1 | CF_Update 生成:CF_min(脏 VC 触发 a)/CF_max(周期全量触发 b)/coalescing/400B 门 | ✅ | sue-cbfc-e2e-gating(hash 佐证) |
| CBFC-SP-10 | 5.2.5.7.4 | CC_Update 接收:泄漏回收 + LostCredits<0 重同步防护 | ✅ | TestSuite CBFC lost-credits-guard / cc-update-onwire |
| CBFC-SP-11 | 5.2.5.8 | 非理想信用大小:多信用/单元差异释放(仅本地优化,可选) | ⚪ | 不入仿真:缓冲粒度恒 1 信用单元(§12.32) |
| CBFC-SP-12 | 5.2.7 | MAC 接口:MAC 插入/接收 CBFC 消息(仿真中经控制路径) | ⚪ | 物理层接口不建模 |
| CBFC-SP-13 | 5.2.8.4 | VC 初始化过程 RTS/RTR 握手(经 LLDP,见 5.3) | ✅ | TestSuite RTS-RTR 握手四例(round-trip/门控 E2E/非对称 want/no-peer 兜底)+ active 门单测;最小语义模型,LLDP_NEGOTIATE=1 时启用(§12.32) |
| CBFC-SP-14 | 5.2.8.5 | VC 移除过程(动态去初始化) | ⚪ | 仿真无动态拓扑 |
| CBFC-SP-15 | 5.2.9 | nine rules 全文(1/2/4 结构保证;3/9 引擎驱动窗口;5/6/7 PFC;8 CC_Update) | ✅ | sue-fc-llr-cbfc-coexist / TestSuite FC rule3/rule9/llr-window-driven |
| CBFC-SP-16 | 5.2.11.2 | CtlOS 带宽 ≤2%:400B floor 门 | ✅ | sue-thpt-line-rate(带宽实测) |
| CBFC-SP-17 | 5.2.5.2 | CBFC_EGRESS_BUF_BYTES 出口→入口背压(仿真平台扩展,非 UEC 条款) | ✅ | sue-incast-2to1 |

## 5.2(PFC 对比,802.1Qbb)

| ID | 条款 | 功能点 | 状态 | 验证 |
|---|---|---|---|---|
| PFC-SP-01 | 802.1Qbb | PFC 帧格式(真实 64B、opcode 0x0101、class bitmap、quanta 0xFFFF) | ✅ | TestSuite PFC header |
| PFC-SP-02 | 802.1Qbb | pause/resume 阈值门控(简单模式 pause@≥th,resume@≤th) | ✅ | sue-pfc-gating |
| PFC-SP-03 | SwitchMmu | 共享池动态阈值 + headroom + resume_offset(迟滞) | ✅ | TestSuite PFC shared-pool hysteresis(迟滞带 + hdrm>0 守卫;算法为启发式近似,§6.4 已声明) |
| PFC-SP-04 | — | 配置路径 drainDelay(PFC_DRAIN_DELAY_NS,否则 pause 永不触发) | ✅ | sue-pfc-gating / cfg 路径 |
| PFC-SP-05 | UEC 5.1 | PFC pause 不阻止 LLR 重放 | ✅ | TestSuite LLR replay-during-pfc-pause |

## 5.3 链路协商(LLDP)

| ID | 条款 | 功能点 | 状态 | 验证 |
|---|---|---|---|---|
| NEG-SP-01 | 5.3.1/5.3.2 | LLDP Options TLV:LLR 使能协商(链路双方能力交换) | ✅ | sue-lldp-negotiate(B:无 LLR 对端 → BEST_EFFORT 降级)+ TestSuite LLDP RTS-RTR 握手用例(§12.32 in-suite) |
| NEG-SP-02 | 5.3.2.2 | CBFC TLV:S_CreditSize/R_TotalCredits/R_NumVCs/R_VC_Want 等协商 | ✅ | sue-lldp-negotiate(A:非对称 creditSize/VC limit 双向收敛)+ TestSuite LLDP RTS-RTR 握手用例(§12.32) |
| NEG-SP-03 | 5.3 | LLDP 帧格式/最近桥组播地址/数据库 | ⚪ | 管理面,不入仿真 |

## 平台自有(非 UEC 条款,单测仍按模块)

| ID | 功能点 | 状态 | 验证 |
|---|---|---|---|
| PLAT-SP-01 | AFH 转发:XPU-id LPM、ECMP(多路径 BFS)、TTL、VC 映射(DSCP/PCP/DEFAULT) | ✅ | sue-afh-forward / sue-afh-header-parse |
| PLAT-SP-02 | wire-time + IFG(含 preamble 计线时) | ✅ | sue-afh-wire-time |
| PLAT-SP-03 | 双 EtherType 逐 etype HAS_SHIM | ✅ | sue-afh-header-parse |
| PLAT-SP-04 | conf 预置 3 档(100G 默认/200G/400G;1G 档已随 §12.36 移除)解析+建网 | ✅ | sue-cfg-preset-smoke |
| PLAT-SP-05 | MTP:驻留不变量、等价性(NACK/FLUSH 双模式、线程扫描、ACKSUM 硬化) | ✅ | sue-mtp-equivalence |
| PLAT-SP-06 | 吞吐/incast 场景 | ✅ | sue-thpt-line-rate / sue-incast-2to1 |
| PLAT-SP-07 | scaleup 拓扑:8 卡机内置 switch、64/128/256 超节点(2-plane fat spine) | ✅ | sue-scale-topo-gen / sue-scale-smoke(64/128/256 全 PASS,见 docs/scaleup-topology.md) |
| PLAT-SP-08 | 随机错误注入(每帧概率,FCS 损坏;err_rate 键) | ✅ | sue-llr-random-errors(无损恢复 + 确定性双跑 + 1%/5% 两档) |
| PLAT-SP-09 | 非 SUE EtherType 帧丢弃防护 | ✅ | 并入 sue-afh-forward |

## 待办汇总(按优先级)

1. **已完成(2026-08-13)**:LLR-SP-16 / LLR-SP-22 / PFC-SP-03 五功能点轮落地;PLAT-SP-07 scaleup 拓扑 + 冒烟;sue 单测命名落位;TestSuite 用例重命名 + 禁字符修复。
2. **已完成(2026-08-13 规格收口,§12.32)**:CBFC-SP-13 RTS/RTR 握手(✅,TestSuite 5 新例)、LLR-SP-20 标 ⚪(§12.11 简化)、CBFC-SP-11 标 ⚪(缓冲粒度恒 1 单元)。矩阵从此无 🟡/⏭。
3. **后续(非矩阵项)**:E2 test-util 去重、E7 满负载压测(BFS 超线性 + ACK 定时器密度,`docs/scaleup-topology.md`)、ECMP hash 熵扩展(AGENTS 推迟项)。
