# AI harness 导航

当前推荐入口是仓库根目录的 `bin/sue`，实现位于 `simulators/sue/harness/v2/`。外部 agent 负责理解需求、制定实验并解释证据；CLI 是确定性的 Python 工具，不内置 LLM，也不需要 API key。

## 从根目录运行

```bash
bin/sue help
bin/sue check --config configs/experiments/glm-single.json
bin/sue build
bin/sue run --config configs/experiments/glm-single.json --output results/sue/my-glm-run
```

`check` 展开所有条件并检查容量，不执行网络仿真。输出目录必须是新目录。统一 JSON 支持硬件、模型、并行、IO Die/HBM/PCIe profile、工作负载、种子与变体；见 [输入格式](../../../configs/experiments/README.md)。仅传裸硬件参数不会自动启用全部行为 profile。

## 入口分工

| 入口 | 用途与边界 |
|---|---|
| `bin/sue check/run --config JSON` | 当前默认：统一输入、资源模型、推理/卸载、多条件执行和失败台账 |
| `v2/hardware_run.py` | 逐参数兼容入口；统一入口的推理执行组件 |
| `v2/run.py` | 底层 DAG/拓扑生成、校验、执行；历史固定等待矩阵，不代表当前资源模型的全部能力 |
| `sue_harness.py` | 旧通用单口 CLI：single_switch/leaf_spine、PFC/OFF、MTP 对照；见 [旧入口说明](legacy-single-port.md) |

当前多 Die 入口使用单线程 SUE AFH＋LLR＋CBFC。并行 workers 启动独立实验进程。内部目标名 `sue-harness-v3` 和输入格式号 3 是兼容命名，产品名称仍为 v2。

## 证据和报告

统一入口输出 `experiment.json`、`expanded.json`、`sources.json`、`validation.json`、`implementation.json`、`results.json`、`summary.csv` 和 `verification.json`。每个 `conditions/<name>/` 保存完整输入、日志和 `result.json`，后者指向底层运行目录。先检查 planned/completed 与状态，再分析 task/port/resource 等原始统计。

统一入口不会自动生成全部交互图。图表按实验类型使用 `v2/` 中的分析工具；[手册第5章](../../../docs/manual/05-results-validation.md)说明指标与验收，[第7章](../../../docs/manual/07-usage.md)说明运行流程。端口采样可显式设置 `simulation.port_sample_ns: 500`，会增加输出和运行成本。不要把历史报告的存在当成本次运行成功。

## Agent 与开发导航

- [根目录 AGENTS.md](../../../AGENTS.md)：当前工作流及四类仓库操作指南。
- [v2 底层拓扑/DAG 说明](v2/README.md)：兼容接口及历史拓扑矩阵。
- [资源模型说明](v2/RESOURCE_MODEL.md)：当前机制与历史模型的区别。
- [完整使用与实现手册](../../../docs/manual/README.md)：当前行为模型、配置与实验解释。

历史结果和编译产物不随 Git 提供；首次克隆先构建再运行。仿真窗口不是完整模型 TPOT/TTFT。
