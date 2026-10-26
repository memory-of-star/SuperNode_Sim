# HSDK 6.5.35 LLR/CBFC API 清单与 UEC 对照

> SDK 头文件:`hsdk-all-6.5.35/include/bcm/{port.h, cosq.h, async.h}`。
> 对照基准:UEC(reference/UEC)+ 本工程仿真实现(DESIGN.md)。2026-08-13。

## 1. LLR API(port.h)

### 1.1 模式与行为

| SDK | 说明 | UEC | 仿真 |
|---|---|---|---|
| `bcm_port_llr_mode_enable_{set,get}` + `bcm_port_llr_mode_type_t {Local, Remote}` | Local=可收 INIT;Remote=可发 INIT | ✓ llr_mode_local/remote 完全对齐 | ✓ `LLR_MODE_LOCAL/REMOTE` |
| `bcm_port_llr_tx_behavior_t {Block, BestEffort}` | **只有两档,无 DISCARD** | ⚠ UEC Table 5-9 三行为 DISCARD/BLOCK/BEST_EFFORT | ⚠ 我们实现了三行为;SDK 缺 DISCARD |
| 仅 `llr_init_behavior` 字段 | **无独立 llr_flush_behavior** | ⚠ UEC 有 INIT/FLUSH 两个独立行为键 | ⚠ 我们两键独立;SDK FLUSH 行为未暴露 |

### 1.2 ACK/NACK spacing(THU 数据的第一来源)

```c
typedef enum bcm_port_llr_ctrl_os_spacing_e {
    bcmPortLlrCtrlOsSpacing512b=0, 1k, 1p5K, 2k, 2p5k, 3k, 3p5k, 4k, 4p5k, 5k,
    5p5k, 6k, 6p5k, 7k, 7p5k, 8k, 8p5k, 9k, 9p3k, 9p5k, 10k, 10p5k, 11k, 11P5k,
    12k, 16k, 20k
} bcm_port_llr_ctrl_os_spacing_t;   /* 26 个离散档位 */
```

- **512B 是枚举最小值、20K 是枚举最大值**——THU-SPEC.md 表头的 "512B (min)"/"20KB(max)" 正是这两个枚举端点。
- UEC Table 5-9 要求 400–16384B 连续范围 → **SDK 覆盖不住两端**(512 > 400;20480 > 16384),thu-spec-compliance.md 的判定 2/3 确认为芯片/SDK 能力限制,非测试选择。
- 默认 2k ✓ 与 UEC 建议值 2048 一致。
- 16k/20k 两档的速率差解释:枚举在 12k 后直接跳 16k/20k(无 13–15k 档),20k 档违反 §5.1.3.1.1 的 17296B 速率下界是枚举设计使然。

### 1.3 配置结构(与仿真引擎字段一一对应)

```c
typedef struct bcm_port_llr_config_s {
    bcm_port_t port;  int flags;
    bcm_port_llr_ctrl_os_spacing_t ctrl_os_spacing;  // ACK/NACK 周期(按收到数据字节计)
    uint32 init_count_max;          // INIT 发送次数上限 = 我们 initRetryMax
    uint32 init_retry_interval;     // ns = 我们 initTimerMax
    uint32 outstanding_data_max;    // 重放缓冲字节上限 = 我们 outstandingDataMax(字节)
    uint32 outstanding_seq_max;     // 重放缓冲包数上限 = 我们 outstandingSeqMax(帧)
    uint32 data_age_timer_max;      // ns = 我们 dataAgeTimerMax
    uint32 replay_timer_max;        // ns = 我们 replayTimerMax
    uint32 replay_count_max;        // = 我们 replayCtMax
    uint32 link_down_timer_max;     // ns:link down 后 flush 定时(我们无此键)
    bcm_port_llr_tx_behavior_t llr_init_behavior;
    int replay_buffer_size;         // 字节(需 BUFFER_ALLOC_CUSTOM flag)= 我们 replayBufMax 的字节版
} bcm_port_llr_config_t;
```

- **`bcm_port_llr_config_t_init(unit, port, speed, num_lanes, RTT, config)`**:按 {speed/lanes/RTT} 返回推荐默认——与我们 `0=自动折算 10×LLR RTT`(§12.27)同思路的 SDK 原生实现。
- 计时键(init/data_age/replay/link_down)**全部 ns**;ACK spacing 是**离散字节档**。
- flags:`REINIT_ON_DATA_AGE_EXPIRED / REINIT_ON_REPLAY_MAX_COUNT / REINIT_ON_LINK_FLAP`(UEC 的 re_init_on_discard 拆成三个条件,SDK 超集)+ `CONFIG_BUFFER_ALLOC_CUSTOM`、`CONFIG_REPEAT_ACK`(空闲期重复 ACK ≈ UEC §5.1.6 累积 ACK 重复)、`FLUSH_INTR_ENABLE_{LINK_DOWN, DATA_AGE, REPLAY_COUNT}`。
- Flush 回调:`bcm_port_llr_flush_callback_register` + 事件 {LinkDown, MaxAge, RetryLimit}——与我们 FLUSH 触发源(数据老化/重传计数/pcs_lost)对齐,SDK 多一个 link-down。
- LLR error XOFF:`bcmPortControlLlrErrorXoff{Detection,Monitor,State}`——按 LLR 错误包计数的 XOFF 背压(SDK 扩展,UEC 无明文)。

### 1.4 状态

`bcm_port_llr_status_get` → {llr_active_tx, llr_active_rx, replay_buffer_allocated} ≈ 我们 `GetLlrStatus()`/`GetReplaySize()`。

## 2. CBFC API

### 2.1 port 层(发送侧 CC_Update)

```c
bcm_port_cbfc_enable_{set,get}
typedef struct bcm_port_cbfc_config_s {
    int cc_enable;              // CC 报文发送开关
    uint32 cc_time_period;      // CC 周期,ns,**128ns 粒度**(非 128 倍数向下取整);
                                // 0 = 默认,按端口速率内部推导
    bcm_port_cbfc_cc_counter_mode_t cc_counter_mode;  // Byte / Credit 两种计费模型
    int cc_overhead;            // 每包开销,带符号(负值=从计数中减)
    uint32 cc_cell_size;        // 信用粒度字节(cell)
} bcm_port_cbfc_config_t;
```

- **cc_time_period 128ns 粒度** → THU-SPEC 的 CC max 7,814,426/s = 128ns 周期 = **SDK 最小粒度值** → 之前的"超 UEC 1µs 上限 7.8×"确认是 SDK 能力,非 THU 配置错误(UEC 要求 1µs ≤ CC_msg_timer ≤ 25ms,SDK 下限比 UEC 低 7.8×)。
- **0 = 按速率内部推导**——与 THU "default 9790/s"、与我们 `CBFC_CC_MSG_TIMER_US` 显式配置两路并存。
- `cc_counter_mode`:Byte 模式 `pkt_credit = round_up(pkt_size + byte_overhead)/credit_worth` = **UEC §5.2.5.7.1 公式逐字一致**;Credit 模式 `round_up(pkt_size/credit_worth) + credit_overhead` = SDK 扩展模型(UEC 未定义)。`cc_overhead` 带符号 = UEC PktOvhd(-16..+127)对齐。
- `cc_cell_size` = UEC CreditSize——**56B cell 的落地处**。

### 2.2 cosq 层(接收侧 CF_Update 三件套 —— THU CF 数据的来源)

```c
typedef struct bcm_cosq_port_cbfc_rx_control_s {
    uint8 enable;
    uint32 credit_timer;      // 定时触发 credit-free 更新;单位 = 512B 线时(值 1 = 1×512B)
    uint32 credit_threshold;  // 收到 N×512B 数据触发 credit-free 更新
    uint32 cf_min_spacing;    // 相邻 CF_Update 最小间隔;单位 = 1KB 线时(值 1 = 1×1KB)
    uint32 flags;             // BCM_COSQ_CBFC_CREDIT_RX_CONTROL 置位才用显式值,
                              // 否则"derived internally as per port speed"
} bcm_cosq_port_cbfc_rx_control_t;
```

- **SDK 的 CF_Update 是字节语义**:credit_timer/credit_threshold/cf_min_spacing 都以"512B/1KB 线时"为单位——UEC `CF_min_timer ≥ 8B wire`、双触发(更新+CF_min 到期 / CF_max 到期)在 SDK 里体现为 定时触发 + 数据量阈值触发 + 最小间隔地板 三件套。
- **THU "CF 速率与端口速率无关"的合理解释**:credit_threshold 按**收到的数据字节数**触发——测试流量字节量恒定时,CF_Update 计数与端口速率无关;定时器只做地板。与 UEC 字节语义不矛盾(字节语义说的是**地板**随速率缩放,实测计数由数据量触发主导)。
- SDK 未暴露显式 CF_max 周期全量键——credit_timer 触发可能兼任周期全量(或芯片内部处理),待 THU 确认。

### 2.3 信用/overhead 枚举(离散档)

```c
BCM_COSQ_CBFC_CREDIT_SIZE_{32, 56, 64, 128, 256, 324, 420, 512}      // 8 档
BCM_COSQ_CBFC_PACKET_OVERHEAD_{0, 16, 32, 48, 64, 80}                // 6 档(全非负)
```

- **56B 在官方枚举中**(THU cell 的来源);另有 324/420 两个非 2 幂档位。
- UEC `R_CellSize` 是 1–4095 连续值 → SDK 离散 8 档是能力子集(合规性:UEC MUST 支持协商任意值?——若对端通告非枚举值,SDK 无法精确匹配,需确认 SDK 的兜底行为)。
- PACKET_OVERHEAD 枚举全非负;port.h 的 `cc_overhead`(int)支持负值——两处口径不一,注意区分使用场景。

### 2.4 端口/VC 信用配置(TX/RX 两侧)

| SDK | 字段 | UEC |
|---|---|---|
| `bcm_cosq_port_cbfc_rx_info_t` | buffer_credits、lossless_vc_bitmap、credit_size、packet_overhead | R_P_CL / 无损 VC 集合 / R_CellSize / R_PktOvhd |
| `bcm_cosq_port_cbfc_tx_info_t` | port_credits、port_overshoot_credits、credit_size、packet_overhead(0=按 5m 线缆内部推导) | S_P_CL /(overshoot=SDK 扩展)/ S_CellSize / S_PktOvhd |
| `bcm_cosq_port_cbfc_tx_vc_info_t` | lossless、vc_min_credits | S_VC_CL / 逐 VC lossless |
| `bcm_cosq_port_cbfc_rx_vc_info_t` | lossless(逐 VC lossless/lossy 转换) | R_VC_Want 语义近似 |
| `bcm_cosq_port_cbfc_vc_config_{set,get}` | 逐 VC 配置(含 `BCM_COSQ_CBFC_VC_LIMIT` 等 flags) | S_VC_CL/R_VC_CreditLimit |

### 2.5 CBFC deadlock 检测(async.h/cosq.h)

`bcm_cosq_cbfc_deadlock_control_{set,get}`(DetectionEnable,per-port per-VC)、`deadlock_event_register`(Idle/Deadlock 事件)、`deadlock_vc_state_get`——SDK 工程特性,UEC 无明文对应(相关语义散见 §5.2.9 规则表);我们仿真无此模型(记录为差异)。

## 3. SDK ↔ UEC ↔ 仿真 差异速查

| 项 | UEC | HSDK | 本工程仿真 |
|---|---|---|---|
| ACK spacing | 400–16384B 连续,建议 2048 | **26 档离散 512B–20K** | 钳制 [400,16384] 连续 |
| TX 行为 | DISCARD/BLOCK/BEST_EFFORT × INIT/FLUSH 两键 | Block/BestEffort,仅 INIT 键 | 三行为 × 两键 ✓ 全实现 |
| replay buffer 单位 | 实现自定 | **字节** | **帧数**(差异记录) |
| replay/data_age 定时器 | ns | ns ✓ | ns ✓ |
| 定时器默认 | 无强制默认 | **config_t_init 按 speed/RTT 推导** | 0=自动折算 10×RTT ✓ 同思路 |
| CC 周期 | 1µs–25ms | **128ns 粒度,0=按速率推导**(下限低于 UEC 7.8×) | 100µs 默认,范围未钳制 |
| CC 计费模型 | roundup((size+PktOvhd)/CS) | Byte 模式 = UEC 公式;Credit 模式 = SDK 扩展 | UEC 公式 |
| CF 触发 | 更新+CF_min 到期 / CF_max 到期;CF_min≥8B;CF_max 16KB–1MB | **credit_timer(512B 粒度)+ credit_threshold(512B)+ cf_min_spacing(1KB)**;无显式 CF_max | CF_min/CF_max 字节键双触发 ✓ |
| CreditSize | 1–4095 连续 | **8 档枚举(32/56/64/128/256/324/420/512)** | 连续可配 |
| PktOvhd | -16..+127 | cosq 枚举 0/16/32/48/64/80(非负);port.h cc_overhead 带符号 | -127..127 连续 |
| re_init_on_discard | 单开关 | 三条件 flags(age/replay/link-flap) | 单开关(对齐 UEC) |
| Deadlock 检测 / LLR error XOFF | 无明文 | 有 | 无 |

## 4. THU-SPEC 疑问闭环(更新 thu-spec-compliance.md 的待确认项)

1. **ACK spacing 512B/20K**:SDK 枚举端点,非测试选择 → 判定 2/3 落为 SDK 能力差异(500 字面合规缺口仍在)。
2. **CC max 7.81M/s**:= cc_time_period 取 128ns(SDK 最小粒度)→ SDK 能力,非配置错误。
3. **CF "与端口速率无关"**:credit_threshold 数据量触发主导的解释成立,与 UEC 字节语义不矛盾(字节语义约束的是地板/阈值单位,不是触发计数)。
4. **CF_min/CF_max 单位**:SDK 为字节语义(512B/1KB 线时单位)→ 与 UEC、与我们仿真一致;之前"时间键"的猜测排除。
5. **2KB 列 100G/200G 行 +3.2% 口径**:仍待 THU 确认(非 SDK 头文件可答)。
6. **512B 档 ACK 速率只有 line/512 的一半**:仍待 THU 确认(可能 ACK 计入自身 CtlOS 线时/测量口径)。

## 5. 对仿真的建议(后续对齐候选)

- SDK 无 DISCARD 行为、无 llr_flush_behavior 独立键——若以 THU 芯片为对标,我们的三行为/两键是 UEC 超集,保留。
- SDK replay buffer 按字节、我们按帧——对帧长不等的混合流,字节语义更贴近芯片;可评估 `LLR_REPLAY_BUF_MAX` 增加字节口径(现有字节键 `LLR_OUTSTANDING_DATA_MAX` 已在)。
- SDK ACK spacing 离散档位与 UEC 连续范围的关系:仿真的 [400,16384] 钳制区间涵盖 SDK 全部 26 档(512..20K 中 16K/20K 两档超 UEC 上限,属 SDK 特有)。
- 新增可选模型:LLR error XOFF、CBFC deadlock 检测——均超出 UEC 明文,若要仿真需在 DESIGN 决策记录中立项。
