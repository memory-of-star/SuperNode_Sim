# Scale-Up 仿真工作区

当前使用 **SUE Scale-Up Ethernet 仿真平台 v2**，代码位于 `simulators/sue/`。新实验请通过 `bin/sue` 启动，硬件规格从 `hardware/profiles/` 选择，结果写入 `results/`。

## 目录分工

| 目录 | 内容 |
|---|---|
| `simulators/sue/` | 当前SUE仿真器源码、构建产物、AI harness、项目自身设计文档 |
| `hardware/` | 当前硬件JSON、不同芯片规格的配置副本及说明 |
| `results/sue/` | 已有SUE报告、原始输入输出、校验结果；原内部目录名保持不变 |
| `docs/topology/` | 用户拓扑原文、支持情况分析、拓扑审计脚本和结果 |
| `archive/simulators/` | 历史ns-3-ub、原生ns-3.47项目；完整保存源码与原构建目录 |
| `archive/downloads/` | 原始ZIP/TAR下载包 |
| `archive/docs/` | 早期Kimi/GLM方案、运行说明及初始结果 |
| `archive/results/` | 早期版本报告及历史记录，旧结果路径保留兼容链接 |
| `archive/logs/` | SUE早期构建/运行日志 |
| `ops/report-hosting/` | 公网报告服务脚本、部署记录和隧道日志 |
| `bin/sue` | 当前仿真器的统一命令入口 |

## 完整手册与并行配置

- [完整使用与实现手册](docs/simulator/README.md)：7章详细说明，从运行到DAG、资源队列、SUE协议与证据口径。
- [单页离线HTML](docs/simulator/manual.html)：全文搜索、打印及配置下载。
- `configs/parallel/`：TP/DP/EP、MoE收发、专家放置与token分配JSON。

```bash
bin/sue check --parallel configs/parallel/tp8-global.json
bin/sue run --parallel configs/parallel/tp8-global.json --output results/sue/glm-tp8
```

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
bin/sue run --hardware hardware/profiles/example-half-scaleup.json \
  --output results/sue/my-glm-half-scaleup \
  --model glm-5.3 --chips 8 --tp 4 --batch 8 --context 8192
```

默认配置是 [hardware/profiles/current.json](hardware/profiles/current.json)。字段、单位及批量模式见 [硬件配置说明](hardware/README.md)。当前默认MoE是多Die分片收发；结果是代表块/阶段仿真，不是完整模型TPOT/TTFT。

## 报告与历史资料

- [报告导航](results/README.md)
- [拓扑原文](docs/topology/topology.md)
- [SUE项目说明](simulators/sue/README.md)
- [迁移记录](archive/reorganization-20261022.json)

根目录的 `simai-scaleup-main`、`ns-3-ub`、`ns-3.47`、`topology.md`、`topology_support.md` 和 `.report-hosting` 是兼容软链接，不是重复副本。项目内 `reports`、早期日志目录也保留软链接。旧实验命令、动态库路径和报告证据可能引用这些旧路径，**不要直接删除兼容链接**；新操作使用上面的规范目录。

本次整理只移动归类，没有删除历史实验、源码或下载包，也没有改写历史实验中的绝对路径和SHA记录。

模型配置已集中到 [configs/models](configs/models/README.md)，支持原始 Hugging Face `config.json`＋独立仿真假设 `simulation.json`：`bin/sue check --model-config configs/models/glm-5.3/config.json`。

## Git 版本管理范围

仓库保存当前 SUE 源码（含 ns-3 基座、scratch 适配器和回归驱动）、实验入口、JSON 配置、文档和图表。历史仿真器副本、下载包、原始实验结果、编译产物、缓存、日志及可重新打包的压缩文件仅保留在本地，由 `.gitignore` 排除。忽略不会删除本地文件。

首次克隆后通过 `bin/sue build` 构建，再按上面的命令生成实验结果。`results/` 中的历史结果及指向归档的兼容链接目标不随仓库提供；报告中的历史证据下载包需要原工作区另行提供，或重新运行对应实验生成。已经生成的 HTML 和图表可直接阅读。
