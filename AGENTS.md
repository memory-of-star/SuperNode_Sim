# Agent 入口：SUE Scale-Up Ethernet 仿真平台 v2

本文件适用于整个工作区。所有命令默认从仓库根目录执行。

## 首次进入

1. 阅读 [harness 导航](simulators/sue/harness/README.md) 与 [统一实验 JSON](configs/experiments/README.md)。当前入口是 `bin/sue`，实现位于 `simulators/sue/harness/v2/`。
2. 用 `bin/sue check --config configs/experiments/glm-single.json` 检查配置与容量；这不执行逐包仿真。
3. 需要运行时先 `bin/sue build`，再 `bin/sue run --config configs/experiments/glm-single.json --output results/sue/my-run`；输出目录必须尚不存在。
4. 查看输出的 `verification.json`、`results.json`、`summary.csv`，再沿各条件的 `result.json` 中的运行路径读取原始统计。容量拒绝、超时及失败不能作为有效性能点。

## 输入与实现

- `configs/` 是可配置输入的唯一维护位置：实验定义引用硬件、模型、并行与行为模型。文件引用相对于实验 JSON 所在目录，不是当前 shell 目录。
- 当前示例使用事件唤醒、1 ps 精度（`time_resolution_ns: 0`）、1 ns 请求/通知间隔；MPU 配置为 128 条 AXI 在途请求、190 GB/s 单向带宽。检查展开的配置，不能假设旧入口或历史输入也自动采用这些设置。
- IO Die、HBM 公平共享、事务级 PCIe 由相应 profile 启用；未提供 profile 的兼容行为与当前实验不同。不要只复制裸 CLI 参数来声称复现手册实验。
- 当前多 Die 入口使用 SUE AFH＋LLR＋CBFC，单次仿真单线程。`execution.workers` 是独立案例并行数；底座/旧单口入口的 PFC、MTP 不代表当前入口已接通。
- 修改或评审仿真器前，阅读 [内部 AGENTS](simulators/sue/AGENTS.md) 及其指定的 `DESIGN.md`。协议设计与新增行为模型分层：新增资源、IO、PCIe 机制见 [完整手册](docs/manual/README.md)。

## Agent 工作流

以下文件是仓库内操作指南；未自动加载它们的 agent 应按任务直接读取，不需要安装插件或 API key：

- [初始化与检查](simulators/sue/.agents/skills/sue-welcome/SKILL.md)
- [实验规划](simulators/sue/.agents/skills/sue-plan-experiment/SKILL.md)
- [执行与台账](simulators/sue/.agents/skills/sue-run-experiment/SKILL.md)
- [结果分析](simulators/sue/.agents/skills/sue-analyze-results/SKILL.md)

现有用户授权在阶段间有效。不要为了流程反复请求确认。先校验再运行，保留失败证据；使用新输出目录，不覆盖历史输入、哈希和运行记录。未经任务要求，不启动全量矩阵或重跑 `archive/`。

结果是代表块/阶段窗口，不能直接称完整模型 TPOT/TTFT。解释吞吐或拥塞时区分有效载荷、线上字节、信用检查次数和实际等待时间。新克隆没有被忽略的 `results/` 原始证据；阅读已生成图表不需要重跑，重建历史报告则需要对应数据。
