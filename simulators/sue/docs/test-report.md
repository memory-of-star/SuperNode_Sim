# SUE 仿真平台测试报告(2026-08-17)

## 1. 测试体系(本轮重构后)

| 层 | 载体 | 数量 | 入口 |
|---|---|---|---|
| TestSuite(CI) | `src/sue/test/sue-test.cc` | **61 例** | `./run-sue-tests.sh`(NS3_TESTS=ON 定向 `libsue-test`) |
| scratch 单测 | `sue-<模块>-<功能点>.cc` | **35 个** | `./run-scratch-checks.sh` 一键全跑(49 判语,含 env 配方) |
| MTP 等价 | sue-mtp-equivalence + sue-endpoint-ring + sue-vrf-planes | 3 套 | `./run-s8-mtp.sh 2,4,8` / `./run-s5b.sh` / `./run-vrf-mtp.sh 2,4,8`(五臂含 SPLIT,§12.44/§12.45) |
| 场景/规模 | thpt / incast / scale / vrf | 4 类 | run-s5c-thpt / run-s5e-incast / sue-scale-smoke / sue-vrf-planes |

命名 = 模块+功能点(afh/llr/cbfc/pfc/fc/cfg/entry/endpoint/mtp/thpt/incast/scale/lldp/vrf),规范见 `docs/test-organization.md`;**95 例全集(含逐例设计说明)见 `docs/test-cases.md`**;UEC 功能点矩阵(`docs/uec-spec-matrix.md`)**58 项中 52 项 ✅ 已实现已测**、6 项 ⚪ 不入仿真(物理层线格式/管理面/有意建模简化;2026-08-13 规格收口后 🟡/⏭ 清零,§12.32)。

## 2. 本轮新增功能点验证结果

| ID | 功能点 | 验证 | 结果 |
|---|---|---|---|
| LLR-SP-16 | ACK/NACK 计数配平(收到即计,UEC Table 5-13) | TestSuite "LLR ACK-NACK counters pair across link"(dup/超范围计数 + 空闲链路 tx==rx 配平) | ✅ |
| LLR-SP-22 | CtlOS 错误注入(丢 ACK/NACK/INIT/INIT_ECHO 容错) | sue-llr-ctlos-loss 五场景:①周期 ACK 节奏恢复(UEC 5.1.6)②无节奏下 replay 轮→FLUSH→INIT 链 ③丢 NACK→定时器兜底 ④丢 INIT_ECHO→init 重试 ⑤丢 INIT→重试 | ✅ 5/5 |
| PFC-SP-03 | resume_offset 迟滞(resume 线 = th−offset)+ hdrm>0 守卫 | TestSuite "PFC shared-pool hysteresis"(带宽边界逐字节断言;修复池耗尽 th=0 时空闲 VC 误 pause) | ✅ |
| PLAT-SP-08 | 随机错误注入(拓扑 err_rate 驱动,确定性 RNG) | sue-llr-random-errors:1%/5% 两档无损恢复(2000 帧 rx=2000、discard=0)+ 双跑逐字节一致 | ✅ |
| NEG-SP-01/02 | LLDP 协商(Options TLV + CBFC TLV 最小模型) | sue-lldp-negotiate:A 非对称 creditSize(256↔64)/VC limit(512↔8)双向收敛 + 引擎协商后启动;B 无 LLR 对端 → BEST_EFFORT 降级 + 投递 | ✅ |
| CBFC-SP-13 | RTS/RTR VC 初始化握手(UEC §5.2.8.4,LLDP 多轮最小模型) | TestSuite 4 例:头 round-trip(45B 三位图)/ 握手完成前门控 0 送达→完成后全送达 / 非对称 want(对端未 want 的 VC 永不发)/ no-peer 兜底自协商;+ 引擎 active 门单测(Init 默认开、creditLimit 免疫) | ✅ |

## 3. 全量回归结果

| 项目 | 结果 |
|---|---|
| TestSuite | **61/61 PASS**(0 FAIL,0 告警;含 2^20 seq 回绕(§12.38)、VRF 分段(§12.40)与延迟退款 preamble 不变性(§12.42,TS-61)用例) |
| scratch | **47/47 PASS**(含 11 条 CF 速率扫描配方 + VRF 8 平面(Die 编址)v2 LLR=1/0 双档 + FC=PFC 档) |
| MTP 等价 | NACK+FLUSH 双模式 × 线程 2/4/8 **6/6 PASS**(行为 hash + ACKSUM \|delta\|≤1 硬化判据) |
| endpoint MTP 等价 | **PASS**(6 线程) |
| 吞吐 s5c | 400G clean 391.75 Gbps;100G clean 97.93 Gbps / loss-1e-2 95.15 / loss-1e-3 97.65 / loss-1e-4 97.91 Gbps(NACK-replay 完整恢复,discard=0;2026-08-14 THU 对齐预设重测,§12.37) |
| incast s5e | 2:1 CBFC 背压 **PASS**(100G/200G/400G 三速率:97.94/195.87/391.73 Gbps 线顶,50/50 分拆,无损;天花板断言已参数化) |
| scaleup 冒烟 | 64 机(512 XPU)11.3s / MTP-8 30.5s;128 机(1024 XPU)27.8s;256 机(2048 XPU)111.5s — 全无损、双平面 ECMP 均衡 |
| scaleup 满负载压测 | 64 机:clean 31.4T 无损;sat **48.0T=93.7% 截面积**、credit_exhausted=221528、261760/261760 全投递;MTP-8 等价但 7.5× 慢;无负载 578 LP/43MB/1.65M 事件/ms-sim(§12.34,`sue-scale-stress`) |
| VRF 32 平面 all-to-all | §12.41:8 GPU × 32×200G 口、单台 256 口交换机、1792 流——3,417,344/3,417,344 零丢失;逐平面 56×1907 精确 ×32、VRF0=0、lookup miss=0、逐口 7×1907;LLR=1 全栈聚合 **48,982.8 Gbps = 载荷份额天花板 48,998.9 的 99.9673%**、per-GPU 6.99872 Gbit×8、replay/discard/nack=0、CU/pend=0;LLR=0 同判语(§12.42 退款尺寸修复后,`sue-vrf-planes`) |
| VRF 平面 CBFC vs PFC A/B | §12.43:同场景同数据量流控对比——CBFC 基线 48,982.8 Gbps(99.9673%);**PFC drain=0(配置默认,pause 实质未生效)48,983.0 Gbps(99.9675%)持平**;PFC 生效后单调塌落:drain=1µs 36,890.9(75.29%)、2µs 22,343.9(45.60%)、5µs 9,658.6 Gbps(19.71%),pause-samples 2,528/4,320/11,328;所有臂无损契约保持(delivered==sent、replay/discard/nack=0)——**结论:同等数据量 PFC 更差,代价纯是带宽**;机制 = 阈值-迟滞整链路 stop-and-go 震荡 vs 逐 VC 信用窗连续均衡(`VRF_PL_FC=PFC` 档注册为第 49 判语) |

## 4. credit_limit 对线速的影响(重点图表,creditSize=56B/512B 帧,2026-08-14 实测)

[📈 打开交互图表](test-report/credits-vs-rate.html)(含悬停取数、明暗主题、数据表)

**creditSize 无关性**:[📈 打开结论图表](test-report/credits-vs-rate-creditsize.html)——THU 对齐预设(CF 阈值 2075B 即时退款,CF_MIN=0,§12.37)下,五种 creditSize(32/56/64/128/256B)全矩阵按**信用窗口字节数**(CL×CS)归一后**逐点完全重合**:曲线只由窗口字节数决定,与 creditSize 无关。曲线三段(512B 帧,526B 含头):

- **死锁区**(窗口 ≤ 504B):窗口放不下单帧(526B=512 载荷+14 头,计 ceil(526/56)=10 credits),0 Gbps;
- **逐帧节流区**(窗口 ∈ [526, 2601)B,单帧但不足"阈值 2075 + 单帧 526"):退款须等第 2 帧跨阈值或 CF_max 周期兜底 → 吞吐跌至 ~0.05 Gbps;
- **线速区**(窗口 ≥ 2601B):阈值即时退款,93.5 Gbps(载荷效率天花板 512/546≈93.8%)。

扫描:sue-thpt-line-rate,2 节点直连,LLR+CBFC,creditSize=56B,512B 帧,clean:

| credit limit(CS=56B) | 窗口字节 | 400G | 100G | 占标称线速 |
|---|---|---|---|---|
| ≤9 | ≤504B | **0(死锁)** | **0(死锁)** | 0% |
| 10–46 | 560B–2.58KB | 0.219–0.684 Gbps | 0.055–0.171 Gbps | ~0.06–0.17% |
| 47 | 2.63KB | 369.97 | 92.49 | 92.5% |
| ≥48 | ≥2.69KB | 373.86 | 93.47 | 93.5% |

**结论**:
1. **线速窗口下界 = CF 阈值 2075 + 单帧 526 = 2601B**(CL=47 = ⌈2601/56⌉),模型与实测精确吻合;归一化后两速率逐点重合 → 字节语义、机制速率无关。
2. **窗口 < 单帧 → 死锁**是"阈值 ≤ 窗口"约束的极端形态(§9 已文档化:发送端在接收端攒够阈值前停摆),与芯片同构。
3. **creditSize 无关**:下界 2601B 折算成 ⌈2601/CS⌉ 个信用;旧数据(CF_MIN=125000 每 10µs 退款,§12.36)的过渡区(32K–128K)内大 CS 曲线下移(计费量化敏感),退款节奏 60× 变快后该区整体消失,creditSize 影响随之消失。
4. 工程含义:THU 对齐后小信用窗口(≥2.6KB)即可线速;阈值键取值直接决定窗口下界,调 `CBFC_CF_THRESHOLD_BYTES` 须与最小信用窗口联动评估。
5. **链路时延关系**(§12.39,THPT_PROP_NS/THPT_FWD_NS/THPT_CF_THRESHOLD 扫描实测):节流区速率 ≈ 2.2×kF/CF_max 周期、与 prop 无关;线速区速率 = 线速×W/(W+RTT×线速)(prop 0/100/500/2000ns → 92.49/42.95/13.76/3.87 Gbps;fwd=300ns 时 CL=47 掉到 37.50、CL=128 完全恢复),W=RTT×线速 时恰为半速——事件驱动退款下 BDP 是半速点而非线速点。跳变点 = ⌈Q/F⌉×⌈F/CS⌉(Q=2075→CL=40、Q=8192→CL=160、Q=16384→CL=320,三点实测)——释放量子 Q 是跳变窗口与控制报文频率的权衡键。

## 5. 遗留待办(下一轮)

- ~~LLR-SP-20 CtlOS 子优先级~~ → ✅ 标 ⚪(§12.11/§12.32);~~CBFC-SP-11 非理想信用大小~~ → ✅ 标 ⚪(缓冲粒度恒 1 单元);~~CBFC-SP-13 RTS/RTR~~ → ✅ 已实现(TestSuite 5 新例,§12.32)。矩阵 🟡/⏭ 清零。
- ~~E7 满负载压测~~ → ✅ 已执行(§12.34):sat 模式织物饱和 48.0T=93.7% 截面积、无损、MTP 等价(7.5× 慢);BFS setup 超线性实测 64/128/256 = 227ms/1.39s/7.87s,优化候选记录在 `docs/scaleup-topology.md`。
