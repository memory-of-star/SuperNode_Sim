# SUE 平台评审修复计划(2026-08-13)

> 来源:六维度评审(设计文档 / AFH 数据面 / LLR / CBFC·PFC / 前端·MTP / 测试)。
> 基准:DESIGN.md;规范:UEC 5.1(LLR)/ 5.2(CBFC)。
> 修复按 AGENTS.md 约定:小步验证,每阶段构建 + 回归测试通过后再提交(受控资产变更须评审后提交,本计划即评审输出)。

## 阶段划分

- **Stage A**:P0 几行级修复(8 项,先合入)
- **Stage B**:P0 设计级修复(2 项:部分 ACK 唤醒、rules 3/9 真实窗口)
- **Stage C**:文档修正(tx_replay 论断、§9 默认值表、计数口径统一)
- **Stage D**:P1 验证地基(s5c 吞吐断言、BFS 真 ECMP、MTP 判据强化、run 脚本可诊断化、拓扑校验)
- **Stage E**:P2 收尾(conf 冒烟测试、双轨收敛、§8 同步等,按序推进)

每阶段验证:`. /build.sh`(或定向目标)→ `run-sue-tests.sh`(TestSuite)→ 受影响 scratch 程序 → S8 MTP 双跑等价。

---

## Stage A — P0 几行级修复

### A1. TTL==0 丢弃路径信用泄漏 🔴
- 文件:`src/sue/model/sue-switch-node.cc:123-129`
- 问题:先 `RemoveHeader(h)` 再 `RefundCredit(..., p->GetSize())`,退还字节数少于计费侧(`OnRx` 按含头完整帧),帧长跨信用边界时每丢一帧泄漏 1 credit。
- 修法:`RemoveHeader` 前先取 `uint32_t sz = p->GetSize()`;退还用 `sz`。
- 回归测试:sue-test.cc 新增用例——defer 模式 + TTL 耗尽丢弃 + 帧长构造为跨信用边界,断言 `R_VC_CC`/`CU` 归零。

### A2. OnCcUpdate 缺环回比较防护 🔴
- 文件:`src/sue/model/sue-cbfc-engine.cc:117-122`
- 问题:`LostCredits<0` 被当作 `2^20-δ` 巨值计入 `R_VC_CF`(UEC §5.2.5.7.4 要求"上报严重错误")。
- 修法:`lost > 0 && lost < CC_MOD/2` 才应用;否则 `NS_LOG_WARN` 并仅重同步 `R_VC_CC := S_VC_CC`。
- 回归测试:R_VC_CC 领先 S_VC_CC 时注入 CC_Update,断言计数不被污染、有重同步。

### A3. FLUSH→ADVANCE 非法状态转移 🔴
- 文件:`src/sue/model/sue-llr-engine.cc:429`
- 问题:缓冲为空即无条件 `GoAdvance()`,不检查当前状态;`LLR_RE_INIT_ON_DISCARD=0` 时 FLUSH 态被迟到 ACK/NACK 拉进 ADVANCE,绕过 INIT 重同步。
- 修法:仅当 `m_tx ∈ {ADVANCE, REPLAY}` 时 GoAdvance;INIT/FLUSH 态只更新 acked_seq/缓冲。
- 回归测试:re_init_on_discard=0 → FLUSH → 注入迟到 ACK → 断言仍停留 FLUSH。

### A4. MTP 数据竞争 ①:g_hostRx 🔴
- 文件:`src/sue/network_frontend/sue-entry.cc:46-50`
- 修法:`std::atomic<uint32_t>`(头文件声明同步改)。

### A5. MTP 数据竞争 ②:EtherType 缓存未预热 🔴
- 文件:`src/sue/network_frontend/sue-config.cc`(InvalidateCache 后仅预热 VC map)
- 修法:SueReadConf 预热 VC map 处追加 `EthType0()/EthType1()/HasShim(eth0)/HasShim(eth1)`。

### A6. PFC pause 阻止 LLR 重放 🔴
- 文件:`src/sue/model/sue-net-device.cc:950-977`(Retransmit/DrainRetx 的 `FcPaused(vc)` 门)
- 问题:与 UEC §5.1 明文冲突("pause 不阻止 replay,仅作用于新数据帧")。
- 修法:重传路径绕过 pause 门;注释改为 UEC 依据。DESIGN §6.10/九规则表若有冲突表述一并修正(Stage C)。
- 回归测试:PFC pause 期间 NACK → 重传帧仍发送、不累积 replay_ct。

### A7. tx_replay 改为 UEC 语义(REPLAY 退出次数)🔴
- 文件:`src/sue/model/sue-llr-engine.cc:491`、`sue-llr-counters.h:22` 注释
- 问题:逐帧计数,与 UEC Table 5-13("exited the REPLAY state")相反;DESIGN §6.11/§12.9 的错误"已对齐"论断同步修正(Stage C)。
- 修法:REPLAY→ADVANCE 转移处计数一次;多帧重放场景核对既有断言(现有单测单帧缓冲,应为 1 不变)。

### A8. §9 配置默认值表与代码不一致(文档)🔴
- 文件:`DESIGN.md §9`(SUE_LINK_PROPAGATION_NS 10 vs 0、SUE_SWITCH_FWD_BUDGET_NS 300 vs 0 无背书;CF_MIN/MAX、replay/init 定时器预置值占默认栏)
- 修法:§9 表增"注册默认 / conf 预置"双栏口径;补 §12 决策记录(prop/budget 注册默认 0=基线、conf 预置 10/300)。

---

## Stage B — P0 设计级修复

### B1. ACK 部分释放窗口无唤醒 → 停-走吞吐 🔴
- 文件:`src/sue/model/sue-llr-engine.{h,cc}`、`sue-net-device.cc`(SetLlrEngine 接线)
- 问题:缓冲非空时 OnAckNack 只重排 replay 定时器;设备被 `!CanTx()` 拦下后无唤醒源(仅缓冲全空 GoAdvance / CBFC CF_Update 到达)。FC_MODE=OFF/PFC 下吞吐退化为每窗口一批;末帧 ACK 丢失且 replay 定时器非绑定时无限期停滞。
- 修法:引擎新增 `SetTxOpportunityCb`(ACK/NACK 处理后、窗口可能释放时回调);设备映射到 `DequeueAndTransmit()`(不清 m_retxQueue,与 stateChangeCb 区分)。CanTx 放宽点:回调在 acked_seq 更新之后触发。
- 回归测试:FC_MODE=OFF + 窗口=2 + 突发 N 帧,断言第 3 帧在首个 ACK 后即发送(不等窗口清空);并断言全部送达。

### B2. nine rules 3/9 窗口改由 LLR 引擎状态驱动 🔴
- 文件:`sue-net-device.cc`(Retransmit/DrainRetx 的同步 `FcSuspendForLlrWindow` 包裹、SendCtlCcUpdate 守卫、HandleCcUpdate)、`sue-llr-engine.{h,cc}`(新增窗口回调)
- 问题:旗标仅在重传 TransmitStart 同步区间为真;真实 replay/discard 窗口(跨多事件)期间为 false,周期 CC_Update 穿窗 → CC/CF 双偏 → CU 误算 ~2^15 → VC 误闸停摆。
- 修法:
  - 引擎侧:窗口 = `(m_tx==REPLAY) || m_nackSent`;在进入 REPLAY、GoAdvance、NACK 置位/清除(接收侧 discard 窗口起止)时经回调通知设备。
  - 设备侧:删除 Retransmit/DrainRetx 的同步包裹;`SendCtlCcUpdate` 守卫改查引擎驱动旗标;接收侧 `HandleCcUpdate`(规则 3)在窗口内跳过 `R_VC_CC := S_VC_CC` 更新,**类型 B(LLR-ineligible + CBFC lossless)帧仍须照常计数**(guard 按帧分类,UEC Table 5-26)。
- 回归测试:①真实窗口用例——1G、NACK 触发 ~83 帧重传、CC_Update 周期 100µs 落在重传突发中,断言窗口内无 CC_Update 发送、接收端无窗口内处理;②类型 B 帧在窗口内仍计入 R_VC_CC。规则 3/9 现有单测改为真实窗口驱动。

---

## Stage C — 文档修正

- C1:`tx_replay` 相关 DESIGN §6.11/§12.9 论断改为与实现一致的表述(实现已按 UEC 改,A7)。
- C2:DESIGN §9 默认值双栏(A8)+ §12 补决策记录。
- C3:计数口径统一:DESIGN §4.1/§5.7/§6.2/§10.5 与 AGENTS.md 的 TestCase 计数改为最终值(单一一处事实源:§11)。
- C4:§8 关键流程时序同步到当前 API(TxStore、RxFrame(seq,fcsStatus)、单参 GetNextSendableQindex、延迟退还 §12.30)。
- C5:§9 实现状态清单补漏列 4 键(LLR_PCS_LOST_STATUS_TIMER_MAX_NS、SUE_LINK_PROPAGATION_NS、SUE_SWITCH_FWD_BUDGET_NS、CBFC_EGRESS_BUF_BYTES)。
- C6:九规则表/§6.3 修正 rules 3/9 描述为引擎驱动窗口(B2 后);PFC replay 豁免(A6)若 DESIGN 无记录则补。
- C7:定时器自动折算公式 prop 硬编码 1µs 与 SUE_LINK_PROPAGATION_NS 脱钩(🟡,修代码:sue-config.cc:642 改读 LinkPropagation;1G 预置超 UEC 寄存器上限注明为仿真放宽(2026-08-13 已随 1G 档移除解决,默认档 21830ns 合规,§12.36))。
- C8:§10.4 过期建议(SUE_TIME_RESOLUTION 未注册)删除;§12.13/12.17 加"已闭环/已取代"回注;§12.5 随机注入"留 S5"改挂新 owner。

---

## Stage D — P1 验证地基

### D1. s5c 吞吐阈值断言
- 文件:`scratch/sue-s5c-thpt.cc`(clean 模式)+ `run-s5c-thpt.sh`
- 修法:clean 模式断言 `gbps ≥ 0.95×线速上限`;clean 模式下 `sumReplay==0`。

### D2. BFS 真 ECMP + 双平面分布断言
- 文件:`src/sue/network_frontend/sue-entry.cc`(InstallRouting)、`scratch/sue-s8-mtp.cc` / `sue-s5b-endpoint.cc`
- 修法:BFS 按等价路径收集多端口装 ECMP 组;端到端场景增"两 spine 的 tx/rx 计数均 >0"断言。注意 MTP 等价性纪律(§12.20:起始时刻全局唯一)。

### D3. MTP 等价判据强化 + 线程数扫描
- 文件:`run-s8-mtp.sh`、`scratch/sue-s8-mtp.cc`
- 修法:脚本参数化线程数(默认 8,CI 可 2/4/8);被掩 ACK/NACK 项改"双跑差异 ≤1"断言,超差即 FAIL;保留全 hash 诊断输出。

### D4. run 脚本可诊断化
- 文件:`run-s8-mtp.sh`、`run-s5b.sh`、`run-s5c-thpt.sh`、`run-s5e-incast.sh`、`run-sue-tests.sh`
- 修法:关键行提取改 `grep ... || { echo 诊断; exit 2; }`;BUILD_TYPE 回退改为可执行结构(消除 pipefail 死代码);加 `timeout` 包裹;`S8_THREADS` 非数字校验。

### D5. 拓扑解析边界校验
- 文件:`src/sue/network_frontend/sue-entry.cc`、`sue-lite-endpoint.cc`
- 修法:解析后校验 `l.src/l.dst < nNodes`、`switch id < nNodes`,违者报错拒绝建网;`SueLiteEndpoint::SetDevice` 空指针防护。

---

## Stage E — P2 收尾(按序)

- E1:conf 冒烟测试(scratch 直接 `SueReadConf("conf/sue-400g.conf")` 建网跑通),防预置漂移。
- E2:共享 test-util 头(ResultHash / SueMacFromXpuId 去重 18 处)。
- E3:CL==0 VC 告警;CF_MIN 单键生效;PFC 配置路径补 drainDelay 键(`PFC_DRAIN_DELAY_NS`);混合 FC_MODE 双 OnDrain 门控(`ingress->GetDeferCreditRefund()` 判据)。
- E4:Deserialize 校验 SUE EtherType(误注入帧丢弃+计数);`SueSetGlobal` 返回值告警;SueMtpActive 违约断言。
- E5:replay_ct 仅 ACK/INIT 清零(UEC);引擎默认 outstandingSeqMax 钳到 524288。
- E6:scratch 注册 ctest;TestSuite 按阶段拆分多文件(可选,重)。
- E7:512 卡压测冒烟(1s 无负载:分区表、事件率、内存)+ S5b 验收标准写入 DESIGN §11。

## 状态追踪

| 项 | 状态 | 提交 |
|---|---|---|
| A1–A8 | ✅ 已落地 | `ca7f525`(TestSuite 44/44,scratch 全 PASS,S8 等价 PASS) |
| B1–B2 | ✅ 已落地 | `c3676a6`(TestSuite 46/46,scratch 全 PASS,s5c/s5e/S8 等价 PASS) |
| C1–C8 | ✅ 已落地 | `82cd832`(文档同步 + C7 prop 脱钩) |
| D1–D5 | ✅ 已落地 | `8700081`(s5c 吞吐护栏/BFS 真 ECMP/MTP 判据硬化/脚本可诊断/拓扑校验) |
| E1/E3/E4/E5/E6-lite | ✅ 已落地 | 待提交 |
| E2 test-util 去重 | ⏭ 剩余(独立重构) | |
| E6b TestSuite 拆分 | ⏭ 剩余(重) | |
| E7 512 卡压测冒烟 + S5b 验收标准 | ⏭ 剩余(需拓扑生成器) | |

> 每阶段完成后更新此表并在 git 提交信息中引用编号。
