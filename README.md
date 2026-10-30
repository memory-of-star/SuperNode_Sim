# Scale-Up 仿真工作区

当前使用 **SUE Scale-Up Ethernet 仿真平台 v2**，代码位于 `simulators/sue/`。新实验请通过 `bin/sue` 启动，硬件规格从 `configs/hardware/profiles/` 选择，结果写入 `results/`。

## Agent 使用入口

Agent 请先读 [AGENTS.md](AGENTS.md)，再读 [harness 导航](simulators/sue/harness/README.md)。`bin/sue help` 可查看统一命令和配置示例。指南位于根目录，内部 harness 无需移动或安装；不自动识别 AGENTS.md 的工具也可从本 README 或 CLAUDE.md 找到入口。

## 目录分工

| 目录 | 内容 |
|---|---|
| `simulators/sue/` | 当前SUE仿真器源码、构建产物、AI harness、项目自身设计文档 |
| `configs/` | 统一输入配置：硬件规格与行为模型、模型参数、并行策略、实验定义；见[配置导航](configs/README.md) |
| `results/sue/` | 已有SUE报告、原始输入输出、校验结果；原内部目录名保持不变 |
| `docs/manual/` | 完整使用与实现手册、实验图表和离线下载包 |
| `docs/topology/` | 用户拓扑原文、支持情况分析、拓扑审计脚本和结果 |
| `archive/simulators/` | 历史ns-3-ub、原生ns-3.47项目；完整保存源码与原构建目录 |
| `archive/downloads/` | 原始ZIP/TAR下载包 |
| `archive/docs/` | 早期Kimi/GLM方案、运行说明及初始结果 |
| `archive/results/` | 早期版本报告及历史记录，旧结果路径保留兼容链接 |
| `archive/logs/` | SUE早期构建/运行日志 |
| `ops/report-hosting/` | 公网报告服务脚本、部署记录和隧道日志 |
| `bin/sue` | 当前仿真器的统一命令入口 |

## 完整手册与并行配置

- [完整使用与实现手册](docs/manual/README.md)：10章说明，覆盖运行、行为模型、SUE协议、DeepSeek部署、MPU/PCIe实验及历史证据口径。
- [单页离线HTML](docs/manual/manual.html)：全文搜索、打印及配置下载。
- `configs/parallel/`：TP/DP/EP、MoE收发、专家放置与token分配JSON。

```bash
bin/sue check --parallel configs/parallel/tp8-global.json
bin/sue run --parallel configs/parallel/tp8-global.json --output results/sue/glm-tp8
```

## 统一JSON输入（推荐）

选择一份JSON即可同时指定硬件、模型、并行策略、工作负载和对照条件：

```bash
bin/sue check --config configs/experiments/glm-single.json
bin/sue run --config configs/experiments/glm-single.json --output results/sue/glm-unified
```

完整格式和多条件／PCIe卸载示例见[统一实验输入说明](configs/experiments/README.md)。组件支持内联或文件引用，引用路径相对于实验JSON；每个条件保存可直接重用的完整输入快照。下方旧式逐参数命令仍可使用，不能与`--config`混用。

下列逐参数入口属于兼容用法；没有指定 IO Die、HBM、PCIe profile 时，不等同于当前手册实验。新实验优先使用上面的统一 JSON。

## 日常使用

在本工作区根目录执行：

```bash
# 查看硬件配置及每Die/每端口的换算值
bin/sue check

# 构建当前SUE适配器
bin/sue build

# 跑一个GLM案例；输出目录必须是新目录
bin/sue run --output results/sue/my-glm-run \
  --model glm-5.3 --chips 8 --tp 4 --batch 8 --context 8192

# 换一种芯片规格
bin/sue run --hardware configs/hardware/profiles/example-half-scaleup.json \
  --output results/sue/my-glm-half-scaleup \
  --model glm-5.3 --chips 8 --tp 4 --batch 8 --context 8192
```

默认配置是 [configs/hardware/profiles/current.json](configs/hardware/profiles/current.json)。字段、单位及批量模式见 [硬件配置说明](configs/hardware/README.md)。当前默认MoE是多Die分片收发；结果是代表块/阶段仿真，不是完整模型TPOT/TTFT。

## 报告与历史资料

- [拓扑原文](docs/topology/topology.md)
- [SUE项目说明](simulators/sue/README.md)
- [迁移记录](archive/reorganization-20261022.json)

历史仿真器仅保留在 `archive/simulators/ns-3-ub/` 和 `archive/simulators/ns-3.47/`；根目录的两个同名软链接已移除。历史资料中的旧路径需对应到上述归档目录。

当前源码唯一入口为 `simulators/sue/`，手册统一放在 `docs/manual/`。旧根目录链接 `simai-scaleup-main` 已移除，维护中的脚本、构建配置与文档命令均已改用规范路径。历史实验的原始日志、绝对路径及SHA记录保持原样；其中旧路径的 `simai-scaleup-main/` 前缀对应现在的 `simulators/sue/`。

根目录旧兼容链接 `hardware`、`.report-hosting`、`topology.md` 和 `topology_support.md` 已移除。硬件输入使用 `configs/hardware/`，报告运维使用 `ops/report-hosting/`，拓扑文档使用 `docs/topology/`。项目内 `reports` 和早期日志目录仍指向结果与归档目录。

本次整理保留历史实验、源码和下载包；原始证据中的路径与SHA记录未改写。

模型配置已集中到 [configs/models](configs/models/README.md)，支持原始 Hugging Face `config.json`＋独立仿真假设 `simulation.json`：`bin/sue check --model-config configs/models/glm-5.3/config.json`。

## Git 版本管理范围

仓库保存当前 SUE 源码（含 ns-3 基座、scratch 适配器和回归驱动）、实验入口、JSON 配置、文档和图表。历史仿真器副本、下载包、原始实验结果、编译产物、缓存、日志及可重新打包的压缩文件仅保留在本地，由 `.gitignore` 排除。忽略不会删除本地文件。

首次克隆后通过 `bin/sue build` 构建，再按上面的命令生成实验结果。`results/` 中的历史结果及指向归档的兼容链接目标不随仓库提供；报告中的历史证据下载包需要原工作区另行提供，或重新运行对应实验生成。已经生成的 HTML 和图表可直接阅读。
