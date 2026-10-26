# CF_Update 三件套配合模型与验证方案(THU 交换机)

> 背景:THU-SPEC 的 CF_Update 速率(default 1,518,935 / min 1,514,674 / max 13,020,601 per s)**实际没有打流**,因此只可能是定时器触发的。本文回答:三件套如何配合、已测三组数对应哪个旋钮、以及如何在 THU 交换机上验证。
> SDK 依据:`bcm_cosq_port_cbfc_rx_control_t`(cosq.h)。

## 1. 三件套的分工模型

```c
uint32 credit_timer;      // 定时触发:每 N×512B 线时到期,触发一次 credit-free 更新
uint32 credit_threshold;  // 数据量触发:累计收到 N×512B 数据,触发一次 credit-free 更新
uint32 cf_min_spacing;    // 发送地板:两条 CF_Update 之间最小间隔(M×1KB 线时)
uint32 flags;             // 未置 CREDIT_RX_CONTROL 时,三者按端口速率内部推导
```

流水线语义:

```
收帧 → R_VC_CF 累计
        │
        ├─[credit_threshold] 计数 ≥ N×512B ──→ 置"该发"(脏触发,事件驱动)
        │
[credit_timer] 周期到期(无论有无新计数)─────→ 置"该发"(周期全量,丢帧恢复)
        │
        └─→ OR 合并
              │
        [cf_min_spacing] 地板:距上一条 CF ≥ M×1KB 才放行;不满足则等待/合并
              │
        CF_Update 上 MAC
```

- **credit_timer = 周期全量触发** ≈ UEC `CF_max_timer` 语义(丢一条 CF 也能被下一条周期消息补上)。**无打流时它是唯一驱动源** → 观测速率 = `1 / max(credit_timer, cf_min_spacing)`。
- **credit_threshold = 数据量脏触发** ≈ UEC 双触发里"R_VC_CF 更新 + CF_min 到期"的流量侧;打流时它主导,计数 ∝ 收到的数据字节数 → **与端口速率无关**(THU 结论的正确解释)。
- **cf_min_spacing = 发送地板** ≈ UEC `CF_min_timer`(≥8B wire 的带宽保护);**对两个触发源都生效**——任何触发比地板更频繁都会被钳住,实测速率天花板 = `1/cf_min_spacing`。
- 三者的"或"关系:发送时机 = max(触发需求, 地板)。触发决定"该不该发",地板决定"能不能发"。

与本工程仿真的同构:我们 CBFC 引擎的 `CBFC_CF_MIN_TIMER_BYTES`(脏 VC one-shot 地板)+ `CBFC_CF_MAX_TIMER_BYTES`(周期全量)+ 设备 400B CtlOS 门 = 同一模型;TestSuite TS-54 三场景(S1 纯 CF_min 网格 / S2 空载纯 CF_max 8µs 网格 / S3 丢 CF 补偿)正是这个机制的三面验证。

## 2. THU 三组已测数据的映射

无打流 → threshold 永不置位 → 只有 timer/地板两条路径:

| THU 数据 | 折算周期 | 最可能的旋钮档位 | 说明 |
|---|---|---|---|
| max 13,020,601/s | 76.80ns | **cf_min_spacing=1(1KB)地板档**:timer 设得比地板更小,被地板钳住 | 76.8ns ≠ 1KB@100G(81.92ns)精确值,≈ 960B@100G 或 1KB@106.7G——**换算基底待实验定标**(候选:100G MAC / 103.125G / 106.25G PAM4,或地板计数含消息本身开销) |
| default 1,518,935/s | 658.4ns | **credit_timer ≈ 16×512B = 8KB@100G**(655.4ns,+0.46%;若把 CF 消息自身线时计入,误差可收敛) | 周期全量档 |
| min 1,514,674/s | 660.2ns | ≈ default(差 0.28%):大概率 min 改的是 **credit_threshold**(无打流不生效),或 timer 最小值恰与 default 相邻 | 数据本身无矛盾,但说明"min"不是 timer 档 |

结论:**已测三组只覆盖了"定时档 + 地板档"两条路径;credit_threshold(打流主导)和地板钳制曲线完全没有测到**——这正是需要补的实验。

## 3. 验证方案(4 组实验 + 1 项读回)

所有实验固定 100G 单口,读 `bcm_cosq_port_cbfc_rx_control_get` 确认实际生效值(写显式值须置 `BCM_COSQ_CBFC_CREDIT_RX_CONTROL` flag)。

### 实验 A:无打流 · 扫 credit_timer(定标 timer 与换算基底)

- 配置:threshold = 任意大(如 65535,无流量不触发)、spacing = 1(1KB,地板最小)。
- 扫描:timer ∈ {1, 2, 4, 8, 16, 32, 64} × 512B,测 CF 速率。
- 期望曲线:
  - timer ≥ spacing 区:速率 = 1/timer,线性下降,斜率 = 端口速率/(512B×8)——**用斜率反推 SDK 的字节→时间换算基底**(100G/103.125G/106.25G 一眼可判);
  - timer < spacing 区:速率拐平 = 1/spacing = 13.02M/s(与 THU max 对照)。
- **关键副产品**:这条曲线同时给出 credit_timer 与 cf_min_spacing 两个键的精确换算口径,并解释 2KB 列 103.2G 的历史疑点。
- 三速率交叉:同一配置在 100G/200G/400G 各跑一遍 → 速率若 ×2/×4,确认"字节单位×端口速率"折算成立。

### 实验 B:恒定打流 · 扫 credit_threshold(验证数据量触发 + 速率无关)

- 配置:timer = 巨大(如 64×512B,周期全量基本不发)、spacing = 1。
- 打流:固定字节率(如 50Gbps、2048B 帧),扫 threshold ∈ {1, 2, 4, 8, 16} × 512B,测 CF 速率。
- 期望:CF 速率 = 流量字节率 / (threshold×512B)(线性);把端口换 100G/200G/400G、**流量字节率不变** → CF 计数不变 = "与端口速率无关"的正解(由数据量触发,不是时间键)。
- 对照:THU 的 default 1.52M/s 若在 ~50Gbps 打流下重现,则 threshold ≈ 50e9/8/(1.52M×512) ≈ 8×512B。

### 实验 C:突发打流 · 验证 spacing 地板钳制

- 配置:threshold = 1(每 512B 就触发)、timer = 巨大,瞬间注入大突发。
- 期望:CF 速率被钳在 1/spacing(= 实验 A 的拐点值 13.02M/s),而不是跟随数据量无限增长——验证地板对两个触发源都生效。

### 实验 D:丢帧恢复 · 验证 timer 的 CF_max 全量语义

- 配置:打流 + 大 threshold(脏触发少见),确认 credit_timer 触发的 CF_Update 携带**全 VC 计数**(而非仅脏 VC);
- 若 SDK 有统计口,故意丢一条 CF(如拔/差错注入)后观察下一条周期消息是否补偿——验证 UEC CF_max"周期全量防丢失"。

### 附加:读回默认推导值

- 不置 flag(写 0)时 SDK "按端口速率内部推导"——`rx_control_get` 读回推导值,与 THU 的 default 档对照(658ns ≈ 16×512B@100G 的反推是否成立)。

## 3.5 新数据点与寄存器真义(2026-08-13 更新)

THU 新实测:{spacing=1, timer=511, threshold=511} → **993,479/s**(周期 1006.56ns)。

寄存器(devdb/bcm78920_a0.yml 硬件字段):

| LT 字段 | 硬件寄存器 | 位宽 | 复位值 |
|---|---|---|---|
| TIMER_INTERVAL | `LMMU_CBFC_RX_CF_CONFIG.TIMER` [17:9] | 9 bit(max 511) | 0 |
| CREDIT_THRESHOLD | 同上 .CREDIT_THRESHOLD [8:0] | 9 bit(max 511) | **507** |
| MIN_SPACING | `LMMU_CBFC_RX_CF_SPACING.MIN_SPACING` [6:0] | **7 bit(max 127)** | 0 |

要点:
1. **值直通硬件,换算在芯片内**(ll_cosq_port_cbfc_rx_control_set 原样写 LT)。
2. **单位与注释不符的疑点**:511×512B@100G 应为 20.93µs(47.8k/s),实测 1.0066µs——"512B worth"注释与实际周期对不上(≈2ns×511,或内部有派生/封顶)。
3. **SDK 覆盖陷阱**:芯片层代码 `if (!(rx_control->flags & BCM_COSQ_CBFC_CREDIT_RX_CONTROL)) { ctrl.credit_timer = credit_info[index].credit_timer; ... }` ——**不置 flag 时用户写的值被内部按速率推导值覆盖**!THU 必须先 `rx_control_get` 读回确认落盘值,再谈寄存器语义。
4. MIN_SPACING 硬件只有 7 bit(max 127),API 注释的 511 与寄存器不符——写 511 的实际行为待确认。

**"993,479/s 是否最小值"的判定**:无打流下速率 = 1/max(timer 周期, spacing 地板)。当前 {timer=511, spacing=1} 只把 timer 档拉满、spacing 放开——**若 spacing 档的最大周期大于 timer 档,还有更低的点**。补两组配置即可判定(见实验 A' 的 4 点解模型)。

### 3.6 追加实测:{511,511}=1517.6k、{1,511}=1519.0k——floor 模型被推翻(2026-08-13)

THU 追加两组实测(已确认 flag 置位、数据落盘):

| 配置 {timer, spacing} | 速率 | 周期 |
|---|---|---|
| {511, 1} | 993,479/s | 1006.6ns |
| {511, 511} | 1,517,620/s | 658.9ns |
| {1, 511} | 1,519,013/s | 658.3ns |
| THU default(参照) | 1,518,935/s | 658.4ns |

结论:
1. **spacing 拉大后 timer 失效**:{511,511} ≈ {1,511} ≈ default(差 <0.1%)——周期钉死在 ~658ns。
2. **spacing 与周期的关系为负斜率**(与 "min gap 地板" 直觉相反):数据拟合 周期 ≈ 1008.7 − 2.76×spacing(ns)(假定 511 经 7-bit 字段截断落盘 127);timer 系数 ≈ 0。§1 的 floor 模型**不成立**,真实语义待定标矩阵钉死。
3. 993k 是已测配置空间的最慢点;是否全局最小需补 {511, 0} 与 {511, 2} 两点。
4. MIN_SPACING 硬件仅 7 bit(max 127)——写 511 大概率落盘 127,须 rx_control_get 读回确认。

### 实验 A':16 点定标矩阵(钉死真实模型)

无打流,threshold=511 固定:

| 维度 | 取值 |
|---|---|
| spacing | {0, 1, 2, 4, 8, 16, 32, 64, 127} |
| timer | {1, 511} |

共 18 点;拟合周期模型后用 THU 三组旧数据(default 658.4ns / min 660.2ns / max 76.8ns)反推当时的寄存器配置,并定标 "512B/1KB worth" 注释的真实折算速率。

## 3.7 按注释语义的正确组合与验算(2026-08-13)

注释语义链:**发送 = (timer 到期 OR 收到 threshold 数据) AND (距上一条 ≥ spacing)**。

推荐取值(@100G,UEC 约束):
- cf_min_spacing M=1(1KB 地板:9B/1KB=0.88% < 2% 帽,且 > 8B floor)
- credit_threshold N=1~2(512B~1KB 数据一报;9B/512B=1.76% 逼近 2% 帽)
- credit_timer N=32~2048(16KB–1MB,UEC CF_max_timer 范围;100G 下 1.31µs–83.9µs)

注释模型验算实测(矛盾):
- {t=511,s=1}:预期 max(20.9µs, 81.9ns)=20.9µs→47.8k/s;实测 993.5k(21×)
- {t=511,s=511}:预期 41.9µs→23.9k/s;实测 1.52M(64×)
→ 落盘值(511→127 截断/0x1FF 全 1 特殊语义)或字段单位与注释不符,须 rx_control_get 读回。

语义验证三点(对应三个键各一次):
1. {t=32, s=1}:timer 线性区验证(注释模型预期 32×512B=16KB@100G=1.31µs→763k/s)
2. {t=511, s=2}:负斜率外推(此前拟合 ≈1003ns;注释模型则仍 20.9µs)
3. 打流 + {t=511, s=1, N=1}:threshold 数据量触发(rate=数据字节率/512B,1KB 地板封顶)

另注:THU default 658ns ≈ 16×512B=8KB < UEC CF_max 16KB 下限——若 default 档确为 timer=16,又是一处符合性缺口候选。

## 3.8 调用链追踪(bcm_cosq_port_cbfc_rx_control_set/get,2026-08-13)

完整链路(8 层,全程无单位换算,值原样进寄存器):

```
bcm_cosq_port_cbfc_rx_control_set            (public API, bcm/cosq.h)
 └─ bcm_ltsw_cosq_port_cbfc_rx_control_set   (ltsw/cosq.c,multi-unit 分发)
     └─ mbcm_ltsw_cosq_port_cbfc_rx_control_set (ltsw/mbcm/cosq.c,驱动表)
         └─ bcm78920_a0_ltsw_ll_cosq_port_cbfc_rx_control_set (chip/cosq.c)
             ├─ 未置 flag → ctrl.{timer,threshold,spacing} = credit_info[index] 派生默认
             └─ tm_ltsw_cosq_port_cbfc_rx_control_set (tm/cosq.c)
                 └─ ll_cosq_port_cbfc_rx_control_set:bcmi_lt_entry_set 写 LT(原样)
                     └─ IMM stage 回调 bcmtm_cbfc_rx_contorl_port_stage
                         └─ bcm78920_a0_tm_cbfc_rx_control_pt_set (chip lmmu 驱动)
                             └─ bcmtm_field_set 直写 HW 寄存器
                                LMMU_CBFC_RX_CF_CONFIG.{TIMER,CREDIT_THRESHOLD}
                                LMMU_CBFC_RX_CF_SPACING.MIN_SPACING
```

关键发现:
1. **SDK 派生默认表**(credit_info[],按速率档):
   {100G: t=32, th=4, sp=32}/{200G: 16,4,16}/{400G: 8,4,8}/{800G: 4,4,4},
   且默认 credit_size=56B(idx1)、packet_overhead=16B(idx1)——**56B cell 是
   BCM78920 的芯片默认**;数值随速率翻倍减半 = 字节单位×端口速率的 SDK 意图。
2. **511→127 截断实锤**:`bcmtm_field_set` 按字段位宽掩码写入——MIN_SPACING
   7 bit,写 511 落盘 127。THU 的 {t=511,s=511}/{t=1,s=511} 实测实际是
   HW {T=511,S=127}/{T=1,S=127}。此前对 s=511 的模型拟合全部作废,须按
   s=127 重新解读。
3. **软件层零换算**:字节→时间在芯片硬件内部完成——"512B/1KB worth" 的
   真实折算速率无法从 SDK 源码得到,只能实测定标。
4. **GET 读回真实值**:ll get → bcmi_lt_entry_get(LT→IMM→HW),返回实际落盘
   值(含截断后的 127)——rx_control_get 读回计划有效。
5. **其他写入者**:IMM `bcmtm_cbfc_rx_control_port_internal_update`
   (ACTION_PORT_UPDATE_INT/DELETE_INT)——端口速率变更/删除时会重新 stage
   该表(存量 LT 条目重放,用户已提交值保留)。

以 s=127 重新解读三组实测(100G):
- {T=511,S=1} → 1006.56ns;{T=511,S=127} → 658.93ns;{T=1,S=127} → 658.32ns
- S=127 时周期钉死 ~658.5ns(timer 无效);S=1 时 timer 才主导
- 剩余之谜(timer=511 为何 1006.56ns 而非 511×512B@100G=20.9µs)在硬件
  内部,18 点定标矩阵是唯一途径

## 3.9 仿真改进落地:CBFC_CF_THRESHOLD_BYTES + 芯片↔仿真对照(2026-08-13)

仿真新增数据量触发键(与芯片 credit_threshold 对等,§12.36):

- `CBFC_CF_THRESHOLD_BYTES`(GlobalValue,默认 0=关闭零回归):OnDrain 累计
  释放信用字节,≥ 阈值即请求上报(余量结转,floor(总字节/阈值) 次触发);
  CF_min>0 时由地板网格排队(与芯片 spacing 地板同构),CF_min=0 时立即
  批量发送——CF 计数 ∝ 数据字节、**与端口速率无关**(芯片同款属性)。
- 工程约束同芯片:阈值必须 ≤ 信用窗口(CL×creditSize),否则发送端停摆。
- 验证:TS-54 场景 4(2KB 阈值 × 41×100B → 恰 2 次触发;100G vs 400G
  同字节同计数=速率无关);TestSuite 58/58、scratch 33/33、s8 2/2。

芯片↔仿真配置对照(拿到 THU 寄存器可直接翻译):

| 芯片(SDK) | 单位 | 仿真 conf 键 | 单位 | 语义 |
|---|---|---|---|---|
| credit_timer | 512B/档 | CBFC_CF_MAX_TIMER_BYTES | 字节连续 | 周期全量兜底 |
| credit_threshold | 512B/档 | **CBFC_CF_THRESHOLD_BYTES** | 字节连续 | 数据量脏触发 |
| cf_min_spacing | 1KB/档(max 127) | CBFC_CF_MIN_TIMER_BYTES | 字节连续 | 发送地板 |
| cc_time_period | ns,128ns 粒度 | CBFC_CC_MSG_TIMER_US | µs 连续 | CC 周期 |
| credit_size 枚举 | 8 档(含 56) | CBFC_CREDIT_SIZE | 连续 | 信用粒度 |
| packet_overhead 枚举 | 6 档 | CBFC_PKT_OVHD | 带符号连续 | 每包开销 |

例:芯片默认档(100G){timer=32, threshold=4, spacing=32} →
`CBFC_CF_MAX_TIMER_BYTES=16384, CBFC_CF_THRESHOLD_BYTES=2048,
CBFC_CF_MIN_TIMER_BYTES=32768`。

## 3.10 默认配置三速率实测:字节键语义实锤 + threshold 主导(2026-08-13)

THU 纯默认配置(去掉自定义参数,SDK 派生默认 {t=32, th=4, sp=32} 及对应速率档)实测:

| 速率 | CF 速率 | 周期 | 周期×线速 |
|---|---|---|---|
| 100G | 6,024,685/s | 165.98ns | 2074.7B |
| 200G | 12,050,807/s | 82.98ns | 2074.5B |
| 400G | 24,101,669/s | 41.49ns | 2074.5B |

结论:
1. **周期字节数恒定(≈2074.7B)→ 字节键语义实锤**(SDK "512B/1KB worth"
   注释正确,换算在芯片内按端口速率)。
2. **2074.7B ≈ 2048B(=credit_threshold=4×512B)+ ~27B CF 消息自身开销**
   (9B CtlOS+8B 前导+12B IFG=29B):线速下 **threshold 触发主导**,
   timer(16KB)/spacing(32KB) 更粗不绑定。
3. 与早期"速率无关"(default 1.52M)矛盾的解释:同一公式
   `CF 计数 = 流量字节/(threshold×512B+开销)` 的两个侧面——固定字节量
   打流 → 计数与速率无关;线速打流 → 每单位时间字节随速率 ×2 → CF 速率
   随速率 ×2。
4. 仿真复现:`CBFC_CF_THRESHOLD_BYTES=2048` 线速打流 → CF 速率 =
   线速/2048B ≈ 6.10M/s@100G(与 THU 6.02M 差 1.3%=消息开销,数值
   不必一致);TS-54 S4 已验证字节比例语义与固定字节量速率无关性。

## 3.11 SDK 为什么这样设计?(三难权衡与字节键动机)

三旋钮 = 控制路径"及时性 × 带宽 × 可靠性"三难:
- credit_threshold = 及时性×带宽的平衡(数据量分批,主力路径)
- credit_timer = 可靠性兜底(UEC CF_max 原文 "in case a previous
  CF_Update was lost"——丢帧自愈;无打流时仍有 CF 的原因)
- cf_min_spacing = 2% 带宽帽的执行机构(突发洪泛限速)

字节键动机:①控制开销比跨速率恒定(2KB 数据一条 9B 消息=0.44%,时间键
则高速端口开销比稀释、低速逼近 2% 帽);②硬件账——信用记账本来就按
字节数,threshold 计数器是"顺路白送",时间键需额外时钟资源。
512B/1KB=2 的幂(移位计数器免费,且任何配置天然落在 2% 帽内);9-bit
字段 = 寄存器自然宽度;默认表随速率减半 = 字节域恒定(同一套配置所有
速率行为一致,只需速率索引)。

仿真印证:CF_THRESHOLD/CF_MAX/CF_MIN 三键对齐;差异——SDK timer 默认取
UEC 下限 16KB(自愈窗口最紧),我们 conf 预置 10×CF_min=1.25MB(更省
带宽);对齐芯片语义可设 CBFC_CF_MAX_TIMER_BYTES=16384。

## 3.12 最大/最小 CF_Update 速率:配置配方与扫描实测(2026-08-14)

新增自检 scratch `scratch/sue-cf-rate-scan.cc`:env 三键 + 打流开关,实测
CF_Update 消息速率并与模型公式比对(自检式 PASS/FAIL),已纳入
run-scratch-checks.sh 的 4 个配方。

### 口径:消息 vs 触发

CF_Update 每条 8B CtlOS 消息打包 2 个 (VC,CF) 条目(§5.6):
- **threshold/CF_min 路径**:只脏收流 VC → 每触发 **1 条**消息;
- **CF_max 周期全量**:报全部 8 个 VC → 每触发 **4 条**消息。

THU 计数器按消息条数;对照时注意 ×4 批量差异。

### 配方表(100G,scratch 实测,全部 PASS)

| 目标 | 配置 | 实测消息速率 | 模型与对照 |
|---|---|---|---|
| **最大(门顶)** | CBFC_CF_THRESHOLD_BYTES=1、CF_MIN=0、线速打流 | **31,250,000/s** | line/(8×400B) 门封顶:阈值再小也超不过 400B 固定门(CTLOS_MIN_SPACING_BYTES)——UEC 2% 帽的执行机构 |
| **THU max 档等价** | CBFC_CF_MIN_TIMER_BYTES=1024 + CBFC_CF_THRESHOLD_BYTES=1、打流 | 12,345,700/s | 1/⌊81.92ns⌋=1/81ns。THU max 13,020,601/s(等效地板 960B),差 5.4% = 消息开销 |
| **芯片默认档** | CBFC_CF_THRESHOLD_BYTES=2048、打流 | 6,103,520/s | line/2048。THU 同档 6,024,685/s,差 1.3% = 消息开销(§3.10 已述) |
| **THU default(无打流,旧表 1,518,935/s)等价** | CBFC_CF_MAX_TIMER_BYTES=8192(=16×512B)、不打流 | 6,109,290/s | 4×(1/⌊655.36ns⌋)。**触发口径 1,527,323/s** vs THU 1,518,935/s(差 0.55%)。THU 每触发 1 条、仿真按 UEC 全量 4 条——批量差异,触发速率一致。新 THU-SPEC(2026-08-14)已删无打流 default 列,此行为历史参照(§3.13) |
| **真正最小(仿真合规界)** | CBFC_CF_MAX_TIMER_BYTES=1048576(UEC 上限)、不打流 | 51,254/s(11 触发/ms × 4) | 触发口径 11,921/s = line/(8×1MB)。新表 THU min = 993,479/s({t=511,s=1} 芯片怪值,见 §3.13 映射);合规配置下仿真真正最小在 CF_max 拉满处,比 THU 报的 min 低 83×(触发口径) |

### 扫档曲线(全 PASS,与模型公式精确一致)

threshold 扫档(打流,200B..256KB):200/400B→31.25M(门顶);512B→
24,414,060/s(=line/512);2KB→6.10M;16KB→762,939;64KB→190,735;
256KB→47,684——精确等于 line/(8×T),T<400B 时门顶封顶。

CF_MIN 扫档(threshold=1 逐帧,256B..64KB):256B→31.25M(门顶);512B→
25.0M(=1/⌊40.96ns⌋=1/40ns);1KB→12.35M(=1/81ns);2KB→6,134,970(=
1/⌊163.84ns⌋=1/163ns);16KB→763,359(=1/1310ns);64KB→190,767(=
1/5242ns)——精确等于 1/⌊CalculateBytesTxTime(CF_MIN)⌋(NS 1ns 截断)。

### scratch 用法

```bash
BIN=ns-3-alibabacloud/simulation/build/scratch/ns3.36.1-sue-cf-rate-scan-default
$BIN                                                       # 裸跑 = 芯片默认档复现
env CF_THRESHOLD_BYTES=1 CF_GBITS=1 $BIN                   # 最大(400B 门顶)
env CF_THRESHOLD_BYTES=1 CF_MIN_BYTES=1024 CF_GBITS=1 $BIN # THU max 档等价
env CF_MAX_BYTES=1048576 CF_GBITS=0 $BIN                   # 真正最小(无打流)
```

## 3.13 THU-SPEC 三速率新表映射与预置对齐(2026-08-14)

THU 更新 THU-SPEC.md:CF_Update 表改为**三速率表**(标题"与端口速率无关"→
"与端口速率的关系"——字节键语义由 THU 实测确认,thu-spec-compliance §3
判定随之闭合),且 CF_Update 计数口径为**消息条数**。

| Speed | default | min | max |
|---|---|---|---|
| 100G | 6,024,685/s | 993,479/s | 13,020,601/s |
| 200G | 12,050,807/s | — | 26,046,236/s |
| 400G | 24,101,669/s | — | 52,091,984/s |

### 3.13.1 三列映射(仿真可复现性)

| 列 | THU 等效 | 仿真配置 | 实测 vs THU | 说明 |
|---|---|---|---|---|
| default | 周期 2074.7/2074.5/2074.5B(字节恒定,速率相关) | CBFC_CF_THRESHOLD_BYTES=2075 + CF_MAX=1.25MB | 6,076,220/12,189,700/24,528,300 vs 6,024,685/12,050,807/24,101,669(**+0.9%/+1.2%/+1.8%**) | 2075 = 2048(SDK th=4×512B)+ ~27B CF 消息自身线时计入计数器;残差 = 周期全量叠加 +0.66% + 组帧效率 + 400G NS 网格 41.5→41ns |
| max | 960.02/959.83/959.84B 字节地板(×2/×4 精确) | CF_MIN=960 + threshold=1,100B 密节奏打流 | 13,157,800/26,315,600/52,631,200 vs 13,020,601/26,046,236/52,091,984(**+1.05%/+1.03%/+1.04%**) | 960B = 芯片 spacing=1 的有效值(1KB 注释值 - 64B,换算基底待 THU A' 矩阵定标,§2);NS 网格 76.8→76ns 等 +1% |
| min(仅 100G) | 12,582B 周期(= {timer=511, spacing=1} 芯片怪值,§3.5/§3.6) | CF_MAX=12582 不打流 | **触发口径 993,479/s = THU 精确命中**;消息 3,977,020/s(×4 批量) | THU 每触发 1 条、仿真按 UEC 全量 4 条——批量差异;寄存器真义待 A' 矩阵 |

CC_Update(表未变):default 9,790/s = 102.1µs → CBFC_CC_MSG_TIMER_US=102
(9,804/s,+0.14%);min 1,222/s = 818.3µs 可表达;max 7,814,426/s = 128ns 低于
µs 键分辨率,不表达(SDK 粒度,thu-spec-compliance §2)。

LLR ACK(表未变,2KB 列):THU 配置值 = 2KB = 2048B,100G/200G 档保持
2048(实测速率差 = 64b/66b 口径,疑点 5);400G 行是 MAC 干净口径,2048B=
40.96ns 被 NS 截断 40ns → +2.4% 偏快,预置改 2050B=41.0ns 精确,与 THU
24,414,902/s 差 **-0.1%**。

### 3.13.2 预置改动(conf/sue-default|200g|400g.conf,§12.37)

| 键 | 旧 | 新 | 理由 |
|---|---|---|---|
| CBFC_CF_THRESHOLD_BYTES | 0 | **2075** | THU default 列对齐(见上) |
| CBFC_CF_MIN_TIMER_BYTES | 125000 | **0** | CF_min>0 时 CF 节奏 = 1/CF_min(阈值流被钳),与 THU 默认档 spacing=32KB 不绑定的实测矛盾;0 = threshold 主导,设备固定 400B 门仍封顶 2% 帽 |
| CBFC_CC_MSG_TIMER_US | 100 | **102** | THU 9,790/s |
| LLR_CTLOS_TARGET_SPACING | 2048 | **2050(仅 400G 档)** | NS 网格对齐(见上);100G/200G 保持 2048 = THU 配置值 |
| CBFC_CF_MAX_TIMER_BYTES | 1250000 | 不动 | 周期全量叠加仅 +0.66%;THU 默认配置 timer 观测量近似看门狗不叠加(差异记录) |

### 3.13.3 顺带修复:多量子阈值触发(sue-net-device.cc)

4096B 大帧验证暴露:OnDrain 阈值块原单 `if` + 循环内清 dirty → 帧长 ≥ 阈值时
每次 drain 只发 1 条、速率塌到帧率(与"floor(总字节/阈值) 次触发"注释语义
不符;TS-54 S4 小帧路径从未暴露)。改 `while` 逐量子发送、dirty 循环外清零;
4096B 帧 60,239 条 = 125M 字节/2075 精确。TS-54 全场景回归不变。

scratch sue-cf-rate-scan 配套:增 `CF_FRAME_BYTES`(默认 4096,大帧≈100% 数据
效率匹配 THU 打流;地板档须 100B 小帧密节奏)+ 打流速率窗口锚定数据接收
区间(排除打流后空闲期 CF_max 尾);run-scratch-checks 注册 11 配方全 PASS。

## 4. 与仿真的同构验证(可选,我们侧)

我们 TestSuite TS-54 已覆盖同一机制的三面(CF_min 网格 / 空载 CF_max 网格 / 丢 CF 补偿),可直接对照:

| THU 实验 | 仿真已有验证 |
|---|---|
| 实验 A(无打流 timer 扫档) | TS-54 S2:空载 CF_max 8µs 网格,间隔 ∈ (7µs, 9µs) ✓ |
| 实验 B/C(打流 threshold + 地板) | TS-54 S1:纯 CF_min 2µs 网格 ✓;TS-47 等打流场景 |
| 实验 D(丢帧补偿) | TS-54 S3:丢一条 CF → cf_update_rx == tx−1、CU 归零 ✓ |
| 实验 A 扫档曲线 | **已补**:scratch sue-cf-rate-scan(§3.12),threshold/CF_MIN/CF_max 三键扫档 + 最大/最小配方,全部与模型公式一致 ✓ |
