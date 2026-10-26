# AGENTS.md — ns-3 SUE Scale-Up 仿真平台

## 项目目标
基于 ns-3 实现 Scale-Up Ethernet (SUE) 仿真平台:SUE Gen1 Lite 转发 + LLR + CBFC + PFC(对比用)。目标规模:512 卡 / 32 交换机 / 多平面。

## 设计基准(必读)
**`DESIGN.md`** 是后续**代码实现 / 代码评审 / 规格验证**的唯一基准。实现与文档冲突时以文档为准。开始任何编码/评审前先读 `DESIGN.md`。

## 参考材料
`reference/`(已 gitignore,第三方,可从上游重新获取):
- `SimAI/` — ns-3-alibabacloud scale-out 后端 + astra-sim(基座/集成参考)
- `SUE/` — `Scale-Ethernet-RM104.pdf` + `Switch-ASIC-AFH.md`(AFH Gen1 规范)
- `UEC/` — LLR / CBFC / 链路协商规范
- `ns-3.48.tar.bz2` — 干净上游 ns-3.48 源码(候选构建基座)

## 版本管理规则(防误删/误改)
- 本仓库用 git 管理;`DESIGN.md` 与 `src/sue/` 为**受控资产**。
- 变更须**评审后提交**,提交信息说明变更点;严禁直接覆盖或删除受控文件。
- 误删/误改可用 git 历史回溯:`git log -- <file>`、`git checkout <commit> -- <file>`。
- 阶段性提交对齐 `DESIGN.md §11` 验证里程碑(S0–S6)。
- `reference/` 不入库(第三方);`ns-3-alibabacloud/` 树已入库作受控基线(见“构建基座”)。

## 工作约定
- 新增独立 `sue` 模块实现,**不修改现有定制模块**(仅入口 1 处配置分发)。
- LLR/CBFC/PFC 封装在 `SueNetDevice`,与 AFH 转发解耦;上层只见无丢包链路。
- 小步验证:S0 框架 → S1 AFH 转发 → S2 CBFC → S3 LLR → S4 共存 → S4b PFC 对比 → S5 端到端(多线程)。每阶段:单测 + UEC 计数器输出。

## 构建基座(已定)
- **ns-3-alibabacloud fork**(已拷入工程 `ns-3-alibabacloud/` 作为受控基线)+ **mtp** 多线程(已就绪)。
- `ns-3.48.tar.bz2`(在 `reference/`)仅作上游 API/构建参考,非构建基座。
- 详见 `DESIGN.md` §1/§6.1/§6.13/§10。


## 回归基线(2026-08-13 评审后)
- 归档文档:**RTT 排查档案** `docs/rtt-investigation-2026-08.md`(§12.48-53 全链);**平台能力/使用条件/IO Die 接口** `docs/platform-capabilities.md`;**应用实例集** `docs/app-notes.md`;**API 契约** `docs/api-reference.md`(SwitchSend/Receive/背压三件套)。
- 测试组织:**scratch=单测、TestSuite=CI**,命名=模块+功能点(规范 `docs/test-organization.md`;**用例全集与逐例设计说明 `docs/test-cases.md`**;UEC 功能点矩阵 `docs/uec-spec-matrix.md`,§12.32 规格收口后 🟡/⏭ 清零)。
- TestSuite:**53 例**(`./run-sue-tests.sh`,NS3_TESTS=ON 定向 `libsue-test`);scratch:**33 个**(`./run-scratch-checks.sh` 一键全跑,含 conf 预置冒烟、scaleup 冒烟与满负载压测)。
- MTP 等价:`./run-s8-mtp.sh [2,4,8]`(NACK+FLUSH 双模式,行为 hash + 被掩 ACK/NACK 计数 |delta|≤1 硬化判据);VRF 32 平面:`./run-vrf-mtp.sh [2,4,8]`(五臂含 SPLIT 拓扑分解变体,§12.44/§12.45——**MTP-8 首次正加速 1.89×**)。
- scaleup:8 卡机内置 switch + 2-plane fat spine,64/128/256 超节点冒烟全 PASS(`docs/scaleup-topology.md`)。
- 评审修复记录:DESIGN §12.31 / `docs/review-fix-plan.md`。

## 构建(已验证,S0)
本地工具链 Ubuntu / GCC 15 / Python 3.14 与基线有两个不兼容,已用 workaround 绕过:
1. `./ns3` 脚本与 Python 3.14 的 argparse 不兼容 -> 直接用 CMake。
2. GCC 15 收紧传递包含,基线头文件用 `uint8_t` 未显式 include -> 全局 `-include stdint.h`(不改源码)。

一键脚本(已固化上述命令):
```
./build.sh                       # 配置(首次)+ 全量构建,-j8
./build.sh --target libsue       # 只编 sue
rm -rf ns-3-alibabacloud/simulation/build && ./build.sh   # 重新配置
```
S0 状态:`libsue` 生成,`SueChannel/SueNetDevice/SUESwitchNode` 的 TypeId 已注册(可实例化),全量基线构建 exit 0。
注:单进程多线程(**`NS3_MTP=ON`**)已启用并落地(S5a,§10.5);`mpi` 未启用(MPI 后续)。

**已知基线问题(fork,非本次引入,已记录)**:
- `all` 目标在 `NS3_TESTS=ON` 下构建失败:`src/mtp/test/mtp-test-suite.cc:4` include 不存在的 `ns3/mtp.h`(mtp 公开头是 `mtp-module.h`/`multithreaded-simulator-impl.h` 等)。`build.sh` 默认 `NS3_TESTS=OFF`,不受影响;全量回归/测试构建请建**定向目标**(`libsue-test`、`scratch_*`),勿建 `all`。候选:向 fork 上游报。


## S0 验证(已通过)
- 受控 S0 单测:`src/sue/test/sue-test.cc`(TestSuite,已入 `TEST_SOURCES`),全量构建 + `NS3_TESTS=ON` 时编译并经 test runner 运行(此时 csma 等依赖齐全)。
- 本地快速验证:`simulation/scratch/sue-afh-device-dispatch.cc`(scratch/ 被 fork 的 .gitignore 忽略、**不入库**,仅本地)。已验证 `S0 CHECK PASS`:
  构建 `cmake --build build -j8 --target scratch_sue-afh-device-dispatch`,运行 `./build/scratch/ns3.36.1-sue-afh-device-dispatch-default`。
  三项:三类可 `CreateObject`;Attach 后 `GetNDevices==2`;TransmitStart 静态分派到 dst `SueNetDevice::Receive`。
- 基线 build-fix:`point-to-point/CMakeLists` 补 `${libinternet} ${libapplications}` 链接依赖(原 CMakeLists 漏声明,致 `libpoint-to-point.so` 含未解析符号、可执行文件链接失败;不改逻辑,对 astra-sim 全量构建无影响)。


## S1 测试状态(评审闭环)
- 入库单测:`src/sue/test/sue-test.cc`(TestSuite,S0+S1 用例 + AFH 计数器断言),`NS3_TESTS=ON` 下编译通过(`libsue-test` 生成)。
- 运行时验证:`scratch/sue-afh-device-dispatch.cc`、`scratch/sue-afh-header-parse.cc`、`scratch/sue-afh-forward.cc` 已 force-track 入库(scratch/ 默认被 fork 的 .gitignore 忽略,故强制入库);`cmake --build build --target test`(ctest)三者均 Passed。这三者与 TestSuite 用例同构,因 ns-3 test-runner 在本机不可用而作为直接运行时验证。
- 已知基线工具缺口:`./ns3` wrapper 与 `test.py` 在 Python 3.14 下不兼容(`store_true` 作用于位置参数等多项 3.14 问题),`test.py -s sue` 暂不可用。**未修改基线 ns3 脚本**,构建仍走直接 CMake(`./build.sh`)。运行 TestSuite 经 test-runner 属基线工具修复项,非 S1 逻辑问题。

## 推迟项(记录)
- ~~时序/带宽建模:wire-time+IFG~~ ✅ 已落地(S2):sue-net-device.cc 按 p->GetSize() 计线时(含 8B preamble)、IFG 按 SUE_IFG_BYTES 折算;SueTimingTestCase 断言 A@wireTime、B@wireTime+IFG+wireTime。
- ~~可配置项:EtherType/VC 映射~~ ✅ 已落地(S5/S7):双 EtherType 0xFCF1/0xFCF2 + 逐 etype HAS_SHIM;DSCP_TO_VC_MAP/PCP_TO_VC_MAP/DEFAULT_VC(§7)。
- ~~ECMP hash 熵:现仅用 XPU-id~~ → ✅ 已落地(§12.33):`GetOutPort(xpuId, flowEntropy)` 熵 = FNV-1a(SA|DA|FlowLabel),流内亲和/流间分散;scale 冒烟双平面自然分拆,不再依赖奇偶凑组。
- `SueRoutingTable::AddPrefix` 同前缀重复添加不去重;次要,后续清理。
- AFH/LLR/CBFC/PFC 计数器均已实现(UEC Table 5-13 22 项 + CBFC/AFH;tx_replay 为 REPLAY 退出语义,§12.31 A7)。
- ~~(2026-08-13 评审新增)ECMP 数据面:BFS 装路由只装单端口~~ → ✅ D2 已闭环(15163b3):BFS 收集等代价多路径装 ECMP 组(按 first-hop 口分别扩展共享中间节点),s8 双平面分布断言 spine10/11 tx 800/800。仅剩 ECMP **hash 熵**扩展(上一行)。

## 推迟项(S5 收尾清单,评审提醒)
- **MTP 驻留不变量(§10.5,✅ 已落地,§12.19)**:`ScheduleCtx`/`ScheduleSelf` 依赖"调用方必然在节点 LP"(数据平面经 channel 的 `ScheduleWithContext` 接缝驻留)。已加 debug 断言 `SueMtpResidencyCheck`(sue-mtp.h):MTP 下校验当前 LP == 拥有节点 LP,违规即 `NS_ASSERT_MSG` abort;负测试(公共 LP 发包)已证触发。S5 驱动 `SueLiteEndpoint` **已满足**(§6.12/§12.21):首发 `ScheduleWithContext(nodeId, ...)` + 自链式普通 `Schedule` 继承节点 LP + `SendFrame` 每 arm 前 residency check;`sue-endpoint-ring`/`run-s5b.sh` MTP 双跑等价佐证。
- **S8 等价性补测(FLUSH+INIT 重同步)** → **✅ 已闭环(§12.20)**:S8 增 `SetCorruptFirstN(10)` FLUSH 模式,`run-s8-mtp.sh` NACK+FLUSH 双模式双跑 diff hash 全 PASS。
- **S8 等价性场景约束(§12.20)**:任何依赖同时间戳跨 LP 事件顺序的等价性场景,须保证出发时刻全局唯一(每 host 互异、非 20us 倍数的 dither 起始偏移)——否则 Default(FIFO)vs MTP(LP 序)会把"内容→槽位"分配互换,刀锋翻转(rx_ack 197 vs 196 根因)。
- **基线构建记录**:`all`@`NS3_TESTS=ON` 失败(fork `mtp-test-suite.cc` include 不存在 `ns3/mtp.h`)见上"已知基线问题"。

## SUE AI harness（2026-09-14 新增）

自然语言实验优先使用本仓库的 SUE harness，入口 `python3 harness/sue_harness.py`，文档 [harness/README.md](harness/README.md)。流程借鉴 ns-3-UB 的规划、运行、分析与证据契约；协议仍由原 `src/sue` 实现。

仓库内 skills（从当前仓库根解析路径）：
- 初始化／冒烟：[sue-welcome](.agents/skills/sue-welcome/SKILL.md)
- 实验规划／对比：[sue-plan-experiment](.agents/skills/sue-plan-experiment/SKILL.md)
- 执行／台账：[sue-run-experiment](.agents/skills/sue-run-experiment/SKILL.md)
- 分析／报告：[sue-analyze-results](.agents/skills/sue-analyze-results/SKILL.md)

已有用户授权在阶段间持续有效。根据已知需求推进，不为流程本身反复确认。新增功能只使用外部适配器 `simulation/scratch/sue-harness.cc` 和独立 `harness/` 工具；未替换 SUE 核心协议。优先执行明确授权的可复现案例。

## 当前版本命名

沿用 DESIGN.md 的项目名称“SUE Scale-Up Ethernet 仿真平台”，本地新增版本对外称“SUE Scale-Up Ethernet 仿真平台 v2”。AI harness 是组件名。文档不再使用旧的临时版本称呼。底座保留 RoCE/RDMA 等能力，但当前 harness 只接通 SUE；参见 README.md。


## v2 拓扑适配与命名（用户已确认）

当前所有新增能力统一命名为“SUE Scale-Up Ethernet 仿真平台 v2”，不要再把当前拓扑适配称为v3。多Die、MPU及16种拓扑入口为 `harness/v2/run.py`，读 `harness/v2/README.md`；通用单口入口仍为 `harness/sue_harness.py`，两者都属于v2。`harness/v3`和旧报告路径仅保留兼容别名；后端内部目标`sue-harness-v3`、历史日志及输入格式号3保持不变，防止破坏原始哈希与复现记录。

原DESIGN.md仍是协议基准，新增端点、UCIe、确认模型见v2说明。唯一受控SUE改动为 `src/sue/network_frontend/sue-config.cc` 的端口VRF表扩至1024项；LLR/CBFC/AFH算法未改。功能验收为 `python3 harness/v2/verify.py`。实验报告位于 `reports/v2-topology-kimi-glm53/`，开头详述相对最初下载版本的全部新增特性。特性文本单源为 `harness/v2/features.md`，通过render.py同时生成HTML/Markdown。不得将原版已有的VRF、4Die测试、LLR/CBFC等写成新增；也不得把合成窗口称完整模型测速，或把通用入口的MTP/PFC能力声称为已接入多Die入口。
