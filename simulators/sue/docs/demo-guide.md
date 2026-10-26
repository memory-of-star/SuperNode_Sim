# SUE Scale-Up 仿真平台 — 现场演示手册(2026-08)

> 目的:现场演示用例目录 + 运行方法 + 日志查看约定。所有命令、预期输出、耗时均为
> 2026-08-19 实测(prop=100 新口径),可直接照抄执行。详细设计依据见 `DESIGN.md`
> (§12.41–§12.46 为本期工作),测试全集见 `docs/test-cases.md`。

---

## 0. 一页速览(开场 1 分钟讲定位 + 成果)

**平台是什么**:基于 ns-3 的 Scale-Up Ethernet(SUE Gen1 Lite)仿真平台——AFH Gen1
转发 + LLR 链路级重传 + CBFC 信用流控(+ PFC 对比基线)+ 交换机 VRF 转发域,支持
100G/200G/400G、多平面拓扑、单进程多线程(MTP)并行。

**成果 → 证据 → 出处**(讲工作时按此表逐条指向用例):

| # | 工作 | 现场证据(用例) | 数字 | 出处 |
|---|---|---|---|---|
| 1 | SUE 数据面全栈(AFH/LLR/CBFC/PFC/VRF) | TestSuite + scratch 全家桶 | 61 例 + 47 判语全 PASS | §6–§9 |
| 2 | THU 交换机实测对齐(控制面三键) | 文档曲线 | CF/CC/ACK 偏差 ≤1%(多数 ≤0.3%) | `thu-cf-update-verify.md` |
| 3 | UEC 规范逐条合规 | 规范矩阵 CSV | 189 条:128 ✅ / 30 ⚠️ / 29 ❌ | `uec-spec-matrix.md` |
| 4 | VRF 32 平面拓扑(8 GPU×32×200G=256 口) | A3 | 全平面精确隔离、99.83% 载荷天花板 | §12.41/§12.40 |
| 5 | CBFC vs PFC 同量 A/B | A4 | PFC 生效即塌落:99.97% → 45.4% | §12.43 |
| 6 | MTP 多线程等价性体系 | B3/A5 | 行为 hash 逐字节一致 | §10.5/§12.44 |
| 7 | **MTP 并行加速(本期)** | A1/A2/A5 | fat-spine **3.33×**、smoke 256 机 **2.53×**、VRF SPLIT **1.89×** | §12.45/§12.46 |
| 8 | **7.5× 倒退根因定位(本期)** | A1b | lookahead=0 轮数暴雨,58 万轮→4k 轮 | §12.46 |
| 9 | 报文级观测(本期) | C1 | SUE_PCAP + Wireshark 插件,9718 帧回归 0 失败 | `tools/README.md` |
| 10 | 无损传输契约 | B1/B2 | 丢包 1/100 吞吐仅损 2.8% 零 discard;incast 2:1 无损钉线速 | §12.30/S5e |

---

## 1. 演示前准备(开始前 30 分钟)

```bash
cd /home/sujinchi/ns-3-scaleup
mkdir -p logs                                        # 演示日志统一归这里

# 1) 构建自检(约 1 分钟;增量构建,无输出即成功)
./build.sh --target libsue-test
./build.sh --target scratch_sue-scale-stress
./build.sh --target scratch_sue-scale-smoke
./build.sh --target scratch_sue-vrf-planes
./build.sh --target scratch_sue-afh-forward

# 2) 预跑长用例留底(现场只放快用例,长用例讲日志;见 §5 兜底建议)
./run-vrf-mtp.sh 8       2>&1 | tee logs/vrf-mtp.log    # ~15 分钟
./run-scratch-checks.sh  2>&1 | tee logs/scratch-all.log # ~15–25 分钟
VRF_PL_THREADS=1 ns-3-alibabacloud/simulation/build/scratch/ns3.36.1-sue-vrf-planes-default 2>&1 | tee logs/vrf-single.log  # ~90 秒

# 3) 二进制路径别名(演示终端里先敲一次)
BIN=ns-3-alibabacloud/simulation/build/scratch
```

---

## 2. 演示路线

**10 分钟精华版**(推荐):C2(0.6s)→ A1(16s)→ A2(4s)→ B3(10s)→ A3 讲预跑日志(2 分钟)→ C1(3 分钟)

**30 分钟完整版**:上述 + A1b(根因复现,70s)→ A4(讲 PFC 曲线)→ B1/B2(各 10s)→ A5 讲预跑日志 → scratch 全家桶结果尾扫(§C3)

---

## 3. 用例手册

> 通用约定:所有程序/脚本判语打印到 **stdout**,保存日志一律 `2>&1 | tee logs/<名字>.log`。

### A. 本期亮点(必演)

#### A1. fat-spine 满负载 MTP 转正 —— 3.33× 加速(§12.46)
- **功能点**:64 机/512 XPU/578 节点/2-plane fat spine,400G,1.68× 过载 all-to-all;CBFC
  信用窗全额吸收超额、零拒绝;多线程与单线程聚合逐位一致。
- **命令**(先单线程后多线程,对比墙钟):
```bash
SCALE_SAT=1 SCALE_THREADS=1 $BIN/ns3.36.1-sue-scale-stress-default 2>&1 | tee logs/stress-t1.log   # ~12s
SCALE_SAT=1 SCALE_THREADS=8 $BIN/ns3.36.1-sue-scale-stress-default 2>&1 | tee logs/stress-t8.log   # ~3.6s
```
- **预期关键行**:
  - 两跑均有 `SCALE STRESS PASS`;`sustained aggregate=46517.9 Gbps (fabric cross-section 51200)`
    (=90.85% 截面积,两跑同值);`delivered=261760/261760` 无损。
- **讲解要点**:86T 需求被信用窗压到 46.5T 且注入侧零丢帧;MTP-8 从**慢 7.5×** 翻到
  **快 3.33×**(下一张讲为什么)。

#### A1b. 7.5× 倒退根因复现(§12.46,可选但有说服力)
- **功能点**:MTP 每轮推进窗口 = 最小事件时间 + lookahead;链路时延 0 → lookahead=0 →
  轮数=全局不同时间戳数(58 万轮),每轮固定成本 ∝ LP 数。
- **命令**:`SCALE_SAT=1 SCALE_PROP_NS=0 SCALE_THREADS=8 $BIN/ns3.36.1-sue-scale-stress-default  # ~69s, PASS`
- **讲解要点**:同一个二进制,只把传播时延预设从 100ns 改回 0,MTP-8 就从 3.6s 退回
  69s——根因是配置(lookahead)而非模型;`SCALE_PROP_NS` 旋钮即"根因开关"。

#### A2. 超节点规模冒烟 + MTP 扫描(§12.46)
- **功能点**:8 卡机 + 2-plane fat spine 自动建网;64/128/256 机(512/1024/2048 XPU)无损
  冒烟;MTP 加速比随规模上升。
- **命令**:
```bash
SCALE_N=64  SCALE_FRAMES=20 SCALE_THREADS=8 $BIN/ns3.36.1-sue-scale-smoke-default   # 4.3s(1.74×)
SCALE_N=128 SCALE_FRAMES=5  SCALE_THREADS=8 $BIN/ns3.36.1-sue-scale-smoke-default   # 9.9s(2.14×)
SCALE_N=256 SCALE_FRAMES=2  SCALE_THREADS=8 $BIN/ns3.36.1-sue-scale-smoke-default   # 39s(2.53×)
```
- **预期关键行**:各跑 `SCALE SMOKE PASS`(无损 + 双平面转发断言)。

#### A3. VRF 32 平面拓扑 all-to-all(§12.41/§12.40)—— 预跑讲日志
- **功能点**(v2,IO Die 编址):单台 256×200G 口交换机,`PORT_TO_VRF_MAP` 划 8 平面
  (每 GPU 4 IO Die,XPU-ID 含 Die 位);帧不携带 VRF;56 对 all-to-all 条纹化铺满 8 平面。
- **命令**(单跑 ~90s,建议现场讲 `logs/vrf-single.log`):
```bash
VRF_PL_THREADS=1 $BIN/ns3.36.1-sue-vrf-planes-default 2>&1 | tee logs/vrf-single.log
```
- **预期关键行**:`VRF-PLANES PASS`;六组断言全 ok(每 GPU 每源 61024 帧/逐平面 56×1907
  精确/端口对称/LLR 零 replay/discard/nack);`aggregate=48916.9 Gbps (ceiling 48998.9, 99.8327%)`。
- **讲解要点**:平面隔离是**精确计数断言**(不是"差不多"),VRF0 空表 miss=0 证明没有
  流量逃出所属平面。

#### A4. CBFC vs PFC 同量 A/B(§12.43)—— 数据讲解
- **功能点**:同一 all-to-all、同一注入纪律、同一标尺,流控二选一;PFC 接收驻留 drain
  从 0(未生效)扫到 2000ns(生效)。
- **命令**(每档 ~90s,建议讲文档曲线或预跑):
```bash
VRF_PL_FC=PFC VRF_PL_PFC_DRAIN_NS=0    $BIN/ns3.36.1-sue-vrf-planes-default  # 99.97%,与 CBFC 持平
VRF_PL_FC=PFC VRF_PL_PFC_DRAIN_NS=2000 $BIN/ns3.36.1-sue-vrf-planes-default  # 45.4%,断言 FAIL 是预期
```
- **预期关键行**:drain=2000 时 `aggregate=22248.6 Gbps ... 45.4063%`、`pause-samples=…>0`;
  两档 `delivered == sent` 无损契约均保持。
- **讲解要点**:PFC 生效即塌落且**代价纯是带宽**(无损契约不破)——机制:阈值-迟滞
  整链路 stop-and-go 震荡 vs 逐 VC 信用窗连续均衡;这正是业界推 CBFC 的仿真证据。

#### A5. VRF MTP 五臂等价 + SPLIT 拆 LP 1.89×(§12.44/§12.45)—— 预跑讲日志
- **功能点**:MTP 行为 hash(BHASH)双跑 diff 判等价;SPLIT 变体把单交换机按 VRF 平面
  拆 32 台(=40 LP)拿并行收益——"VRF 隔离=可分解"的实证。
- **命令**(全五臂 ~15 分钟,预跑 `logs/vrf-mtp.log`):
```bash
./run-vrf-mtp.sh 8 2>&1 | tee logs/vrf-mtp.log
```
- **预期关键行**:5 个 `VRF-MTP EQUIVALENCE PASS`;日志中 SPLIT 臂墙钟对照单线程
  46.8s vs 88.4s(1.89×)。
- **讲解要点**:PFC drain=2000 臂双跑同为 FAIL(45.4% 天花板)也算等价——判定一致 +
  hash 一致;掩掉的 ACK/NACK 计数有无问题由 LLR-clean 断言兜底。

### B. 数据面核心机制

#### B1. LLR 无损重传 + 吞吐曲线(run-s5c-thpt,§12.34 前序)
- **功能点**:100G 干净链路线速 97.93%;丢包 1/100 → NACK-replay 全量恢复,吞吐仅损
  2.8%,discard 恒 0。
- **命令**:`./run-s5c-thpt.sh 2>&1 | tee logs/s5c.log`(数秒,多档自动跑)
- **预期关键行**:各档 `THPT PASS`;`replay == nack`、`discard=0`。

#### B2. incast 2:1 无损背压(run-s5e-incast,S5e/§12.30)
- **功能点**:两源打满单出口(2:1 超额);出口→入口信用背压,发送端被真实节流,
  200G 出口钉线速,全程零丢帧。
- **命令**:`./run-s5e-incast.sh 2>&1 | tee logs/s5e.log`(数秒)
- **预期关键行**:`INCAST PASS`;`credit_exhausted>0`(发送端节流证据)、`pend/pendMax`
  (交换机挂起证据)、`delivered==sent` 无损;窗口速率 195.87 Gbps。

#### B3. LLR 全栈 MTP 等价(run-s8-mtp,§12.20/§12.28)
- **功能点**:NACK(单帧损坏重传)与 FLUSH(replay 耗尽→跨 LP 重同步)双模式;单线程
  vs 多线程行为 hash 一致 + 被掩 ACK 计数 |delta|≤1 硬化判据。
- **命令**:`./run-s8-mtp.sh 8 2>&1 | tee logs/s8mtp.log`(**9.7s,适合现场跑**)
- **预期关键行**:`NACK MTP EQUIVALENCE PASS (threads=8)`、`FLUSH MTP EQUIVALENCE PASS`。

### C. 观测与工具链

#### C1. 报文级观测:SUE_PCAP 抓包 + Wireshark 插件(本期,tools/)
- **功能点**:旁路抓包(SueChannel trace,标准 EnablePcap 抓不到 SUE 流量);.pcap +
  人类可读 .txt 双输出;Wireshark Lua 插件全格式解析(前导/CtlOS/AFH/CF/CC/PFC),
  tshark 回归 9718 帧 0 失败。
- **命令**:
```bash
SUE_PCAP=1 SUE_PCAP_DLT=en10mb $BIN/ns3.36.1-sue-afh-forward-default   # AFH 转发,秒级
SUE_PCAP=1 SUE_PCAP_DLT=user0  $BIN/ns3.36.1-sue-cbfc-steady-capture-default  # 稳态:INIT+ACK+CF+CC 全控制面
ls sue-*.pcap sue-*.txt    # 当前目录(或 SUE_PCAP_DIR=logs 指定)
```
- **日志查看**:`.txt` 直接看字段级解析;`.pcap` 装 `tools/sue-wireshark-plugin.lua`
  后用 Wireshark 打开(user0 DLT 全格式解析;安装方法见 `tools/README.md`)。
- **讲解要点**:稳态场景一屏看清四类控制面节奏——数据/ACK 各 300、CF_Update 数十条、
  CC_Update 周期 102µs(THU 实测 9,790/s 对齐)。

#### C2. TestSuite 61 例(`./run-sue-tests.sh`)
- **功能点**:LLR 状态机/计数器、CBFC 三触发、PFC 共享池、VRF 三源映射、LLDP
  RTS/RTR、seq 回绕、延迟退款不变性……CI 级回归。
- **命令**:`./run-sue-tests.sh 2>&1 | tee logs/tests.log`(**0.6s,全场最快**)
- **预期关键行**:`PASS <用例名>` 逐条打印;`grep -cE '^[[:space:]]*PASS' logs/tests.log`
  = **62**(61 用例 + 1 条 suite 汇总行)。(加 `--xml=result.xml` 可出 XML 报告。)

#### C3. scratch 全家桶 47 判语(`./run-scratch-checks.sh`)—— 预跑讲结果
- **功能点**:34 个自检程序 + env 配方(含 VRF 三臂、CF 速率扫描 11 配方、规模冒烟与
  满负载压测)。
- **命令**(约 15–25 分钟,预跑):`./run-scratch-checks.sh 2>&1 | tee logs/scratch-all.log`
- **预期关键行**:末尾逐条 `PASS`(47 判语);`grep -c PASS logs/scratch-all.log`。

---

## 4. 日志查看与归档速查

| 判语(grep 键) | 含义 | 出处用例 |
|---|---|---|
| `SCALE STRESS PASS` | 满负载压测全断言过 | A1 |
| `SCALE SMOKE PASS` | 规模冒烟全断言过 | A2 |
| `VRF-PLANES PASS` | VRF 拓扑六组断言过 | A3/A4 |
| `VRF-MTP EQUIVALENCE PASS` | BHASH 双跑一致 | A5 |
| `MTP EQUIVALENCE PASS` | s8 行为 hash 一致 | B3 |
| `THPT PASS` / `INCAST PASS` | 吞吐/incast 断言过 | B1/B2 |
| `^PASS <name>` ×61 | TestSuite 用例 | C2 |
| `aggregate=… Gbps (ceiling …, x%)` | 载荷天花板占比 | A3/A4 |
| `delivered == sent` / `zero loss` | 无损契约 | 全数据面用例 |

归档建议:`logs/YYYYMMDD-<用例>.log`;pcap/txt 加 `SUE_PCAP_DIR=logs`;TestSuite XML
`--xml=logs/tests.xml`。

---

## 5. 其他建议(现场风险与讲法)

1. **控时原则:现场只跑 ≤40s 的用例**(C2/A1/A2/B1/B2/B3/C1);A3/A4/A5/C3 讲预跑
   日志——"日志就在这里,可复查",比现场干等 90s 观感好。
2. **兜底**:预跑日志落 `logs/` 后 `git status` 确认无未预期改动;万一现场构建/环境出
   问题,直接放日志 + `docs/test-report.md` 数字。
3. **讲数字的话术**:先讲契约(无损/等价/精确计数),再讲性能(加速比/天花板占比)——
   契约是"对不对",性能是"好不好",顺序颠倒会被质疑先射箭后画靶。
4. **预期问答**:
   - *为什么 PFC 更差?* 阈值-迟滞整链路 stop-and-go vs 逐 VC 信用均衡;且 PFC 无损
     契约未破,代价纯是带宽(§12.43)。
   - *MTP 为什么现在快了?* 根因是 lookahead=0 轮数暴雨(§12.46),预设 100ns 后轮数
     58 万→4k;A1b 一条命令现场复现根因。
   - *90.85% 缺口是什么?* 控制帧/前导/IFG 线时 + RTT 信用窗代价(prop=0 时 93.7%,
     §12.39 模型预测内)。
   - *和 THU 实测什么关系?* 控制面三键(ACK/CF/CC 节奏)反推对齐,偏差多数 ≤0.3%
     (`docs/thu-cf-update-verify.md`)。
   - *256 口是真实芯片吗?* 单机臂钉 VRF 机制;SPLIT 臂是等价分解变体,两种口径分开
     讲(§12.45)。
5. **演示后收尾**:把 `logs/` 打包带走;新口径基线数字以 `docs/scaleup-topology.md`
   为准(历史 prop=0 数字括注保留)。
