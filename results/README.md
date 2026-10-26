# 实验结果导航

所有SUE结果位于 `sue/`，本次只整体移动，不改变实验内部文件。新运行建议使用 `bin/sue run --output results/sue/新的实验名`。

| 位置 | 含义 |
|---|---|
| [sue/v2-topology-kimi-glm53/report.html](sue/v2-topology-kimi-glm53/report.html) | 早期拓扑与合成通信窗口报告 |
| [sue/v2-topology-kimi-glm53/exploration-20261008/report.html](sue/v2-topology-kimi-glm53/exploration-20261008/report.html) | 拓扑/通信机制探索 |
| [sue/v2-topology-kimi-glm53/hardware-inference-20261022/report.html](sue/v2-topology-kimi-glm53/hardware-inference-20261022/report.html) | 327条件硬件/容量探索；历史集中MoE收发，未全量更新为新布局 |
| [sue/v2-topology-kimi-glm53/moe-striped-validation/report.html](sue/v2-topology-kimi-glm53/moe-striped-validation/report.html) | 多Die MoE分片收发的25项验收与流量图 |
| `sue/hardware-json-validation/` | JSON硬件参数与物理速率校验 |
| `sue/hardware-json-glm-current/`、`sue/hardware-json-glm-half/` | 原规格与半scale-up带宽的GLM案例 |
| `sue/workspace-migration-smoke/` | 整理目录后的运行验证 |

公网服务仍只发布原先的 `sue/v2-topology-kimi-glm53/` 报告子树，不会因目录整理扩大到整个工作区。当前公网地址见 `../ops/report-hosting/deployment.json`。旧报告URL路径保持不变。
