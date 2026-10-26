# CBFC CreditSize 双粒度修订 —— 验证报告

> **日期**:2026-08-31(实现与验证)/ 2026-09-07(报告归档)
> **范围**:UEC §5.2.5.8 method-2 异粒度信用记账(双端 MMU cell 不一致)的引擎/LLDP/配置修订 + 单测/场景/全量回归。
> **设计文档**:`docs/cbfc-credit-size-design.md`(原始问题/方案/反事实对比);设计记录 DESIGN.md §12.56。
> **环境**:dell-Precision-7960-Tower,GCC 11.4 / CMake 3.22 / Python 3.72 核,-j36。

---

## 1. 修订内容

| 文件 | 修订 |
|---|---|
| `src/sue/model/sue-cbfc-engine.h/.cc` | 单一 `m_creditSize` 拆 `m_txCellSize`/`m_rxCellSize`;`PktCredits`(发送,CC/CanSend/OnTx)与 `RxPktCredits`(接收,OnRx/OnDrain,保底 1 信用)分轨;`SetTxCellChoices` 支持集 + `AdoptTxCellFromPeerRx` snap-down;`GetCreditSize()` 语义改为 rx 原生 cell |
| `src/sue/model/sue-net-device.cc` | LLDP 广播值 = 本端 rx 原生 cell;采纳改 `AdoptTxCellFromPeerRx`——**只写 tx,不碰 rx**(修正旧代码整引擎覆写) |
| `src/sue/network_frontend/sue-config.cc` | 新 GlobalValue `CBFC_TX_CELL_CHOICES`(CSV 支持集;空 = 任意,兼容旧行为);`CBFC_CREDIT_SIZE` 语义不变(同时设双 cell) |
| `src/sue/test/sue-test.cc` | 新增 TS-62~65;更新 LLDP 采纳断言(tx 采纳 128 / rx 保持 256) |
| `scratch/sue-cbfc-cellsize.cc` | 新增:四臂对照场景(SC-36) |
| `scratch/sue-lldp-negotiate.cc` | 采纳断言按新语义更新(A.tx=64=B.rx、A.rx=256 不动;B 对称) |
| `run-scratch-checks.sh` | 注册 `sue-cbfc-cellsize:CBFC CELLSIZE PASS` |
| `DESIGN.md` / `docs/test-cases.md` | 设计记录条目 56(§12.56);用例全集 101 例(TS-62~65 + SC-36;axi-translator 条目不随本 patch,待其自身合入时补录) |

**不改动项**(护栏):CU=CC−CF 差分代数、mod 2^15/2^20 回卷、`OnCcUpdate` 对账公式、auto egress buffer 公式(经 `GetCreditSize()==rx` 自然继承新语义)。

## 2. 验证矩阵

| 层 | 入口 | 结果 |
|---|---|---|
| TestSuite(CI) | `./run-sue-tests.sh` | **67/67 PASS,0 FAIL**,套件 0.78s(rc=0) |
| 全量 scratch 回归 | `./run-scratch-checks.sh` | **51 programs, fail=0**(含重量级 mtp-equivalence / scale-stress / vrf-planes×3 / cf-rate-scan×11) |
| 四臂场景 | `sue-cbfc-cellsize`(SC-36) | **9 判语全 ok,`CBFC CELLSIZE PASS`** |

新增用例明细:

- **TS-62** 双粒度走账:1518B 帧 48 计/28 退/20 泄漏 → CC 对账 → CU=0(评审口径逐位固化);
- **TS-63** snap 语义五分支(56→32、精确、缺 TLV、无可行值→最小支持值、空集=旧行为)+ rx 不被采纳改写;
- **TS-64** 回归护栏:rx==tx 双引擎对照,四种帧长 PktCredits 全等、五组计数器全等;
- **TS-65** e2e:32/56 + CC 50µs,100×1518B 满交付,S_CC=4800,末态 CU=0;
- **SC-36** 四臂(下表)。

## 3. 四臂关键数字(100Gbps / 1518B / 窗口 4096 credits / drain 1µs)

| 臂 | 配置 | 吞吐 | CU 峰值 | SW cell 峰值 | CC 帧 |
|---|---|---|---|---|---|
| a min() | A.tx=32 / SW.rx=32 | 100.36 Gbps(线速) | 1248 | **432**(记账放大 71%) | 0 |
| b 方案 | A.tx=32 / SW.rx=56 + CC 10µs | **100.31 Gbps(线速)** | 2388(锯齿) | 252 | 299 |
| c 反例 | A.tx=32 / SW.rx=56,CC 关 | **0.00(硬停)** | 4084→卡死 4060 | 224 | 0 |
| d 理想 | A.tx=56 / SW.rx=56 | 100.36 Gbps(线速) | 728 | 252 | 0 |

**核心结论**:① 方案(b)恢复 min() 基线(a)同等线速,泄漏被对账钉在一个 CC 周期内(CU 锯齿而非单调漂移);② 无对账(c)不是降速而是**硬停**——203 帧后泄漏吃满窗口(交付 ≈ W/(1−rx/tx)),SW 排空后再无返还来源,定量钉死"CC 周期 dT ≤ W−BDP"约束;③ SW 原生 cell 记账比 min() 口径省 71% cell 数(缓存浪费的定量);④ 32B 发送粒度本身不损失吞吐(a==d)。

## 4. 过程中修复的连带断言(新语义必然后果,非缺陷)

| 位置 | 旧断言 | 新断言 |
|---|---|---|
| sue-test.cc(LLDP 采纳用例) | `GetCreditSize()==128`(整引擎采纳) | `GetTxCellSize()==128` + `GetRxCellSize()==256`(rx 不动) |
| scratch/sue-lldp-negotiate.cc | A/B 整引擎互换 creditSize | A.tx=B 原生、A.rx 不动;B 对称(两方向各自同构) |

另:首次全量回归曾被管道 `tail` 掩盖 lldp-negotiate 的真实失败(exit 码失真),已用 pipefail 重跑确认——引用回归命令时注意。

## 5. 复现

```bash
./run-sue-tests.sh                    # 67/67
./run-scratch-checks.sh sue-cbfc-cellsize   # 单场景
./run-scratch-checks.sh               # 51 项全量(~20 分钟)
# 四臂旋钮:RATE/FRAME_BYTES/CL/CC_US/DRAIN_US/WARM_US/STOP_US/N/SAMPLE_US
# 例:CC 周期扫描
CC_US=1  ns-3-alibabacloud/simulation/build/scratch/ns3.36.1-sue-cbfc-cellsize-default
CC_US=100 ns-3-alibabacloud/simulation/build/scratch/ns3.36.1-sue-cbfc-cellsize-default
```

## 6. 证据日志

| 文件 | 内容 |
|---|---|
| `logs/sue-tests-dualcell.log` | TestSuite 67 PASS 全文 |
| `logs/cbfc-cellsize-first.log` | 四臂数字 + 9 判语 |
| `logs/scratch-all-dualcell.log` | 全量回归 51 项 PASS/fail=0 |

## 7. 边界与后续

1. **情形 b 未建模**(设计文档 §5.2):"记账 32 / 物理 cell 128"的小帧 2× 超订需增加物理 cell 占用追踪才能仿真复现——实机属禁止配置,优先级低;
2. CC 周期扫描(1/10/100µs)与 200G/400G 复测未入本轮基线,旋钮已备;
3. 15 位半模校核(<16384 在途 credits)目前是文档约束,未做 conf 启动时自动检查。
