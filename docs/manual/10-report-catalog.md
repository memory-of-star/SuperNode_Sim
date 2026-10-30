# 10. 历史报告与迭代记录

本章将工作区已生成的实验报告、验证记录及底座参考资料归入同一份手册，保留每项结果的模型、布局和测量范围。历史报告盘点日期为2026-10-23，当前实现与实验状态补充核对至2026-10-30；来源路径均相对工作区根目录。数字取自已保存的报告与验证记录，本次盘点没有重跑这些历史实验。

## 10.1 找到了哪些独立报告

按内容去重后，找到**7份主要专题报告**，另有卸载扫描冒烟报告、DeepSeek案例章节和若干JSON/CSV验收材料。同一报告的HTML/Markdown、发布副本、兼容软链接及源代码快照按一份计算。

| 时间 | 报告 | 内容与已记录规模 | 整合时保留的范围 |
|---|---|---|---|
| 2026-09-10 | Kimi K3 / GLM-5.2：64-rank首轮通信仿真 | 6个窗口、137,134个任务完成；固定seed1、EP64、合成MoE通信 | 首轮UB实验；GLM-5.2与后续GLM-5.3需区分 |
| 2026-09-14 | SUE新增功能与真实场景对齐 | 四类skills、任务DAG、分片、队列、运行证据，以及8组历史案例 | 功能接入说明；当前harness能力以第1章核对结果为准 |
| 2026-09-23（按运行记录） | SUE新增特性与16种拓扑模型通信 | 16种拓扑×2模型×3窗口=96项；另有25项功能检查 | 历史固定等待工作负载；保留拓扑/路径/隔离结论的实验条件 |
| 2026-10-08 | SUE通信机制受控探索 | 366条件，363通过、3个墙钟超时；比较拓扑、放置、UCIe、信用、分片和调度 | 三种子合成实验；失败保留，拓扑排名依赖负载 |
| 2026-10-22 | 算力、内存与通信联合探索 | 327条件：297逐包完成、30容量拒绝、0执行失败 | 包含计算/HBM/MPU资源队列；该批结果采用当时集中MoE布局 |
| 2026-10-22 | 多Die MoE分片收发验收 | 25项验收；核对全活跃TP rank发送、流量守恒、专家计算与依赖 | 属于收发布局功能/因果验收，不能代替整模型性能校准 |
| 2026-10-23 | MPU与PCIe条件对比 | 报告记录145例、193,001任务的证据核验；另增加54个MPU效率1.0案例 | 修正版采用每芯片独立PCIe基线；该报告PCIe列为载荷下界，不是TLP/PHY仿真 |

各报告的实际位置：

- **Kimi K3 / GLM-5.2：64-rank首轮通信仿真**：`archive/simulators/ns-3-ub/scratch/20260910-kimi-k3-glm52-ep64/report.html`；同目录有 `report.md`。
- **SUE新增功能与真实场景对齐**：`archive/results/20260914-feature-alignment/report.html`；同目录有 `report.md`。
- **SUE新增特性与16种拓扑模型通信**：`results/sue/v2-topology-kimi-glm53/report.html`；同目录有 `report.md`。
- **SUE通信机制受控探索**：`results/sue/v2-topology-kimi-glm53/exploration-20261008/report.html`；同目录有 `report.md`。
- **算力、内存与通信联合探索**：`results/sue/v2-topology-kimi-glm53/hardware-inference-20261022/report.html`；同目录有 `report.md`。
- **多Die MoE分片收发验收**：`results/sue/v2-topology-kimi-glm53/moe-striped-validation/report.html`。
- **MPU与PCIe条件对比**：`results/sue/mpu-pcie-analysis-20261023/report.html`。

## 10.2 将各报告的发现放回研究问题

### 从UB首轮实验到SUE任务接入

2026-09-10的首轮实验回答“给定EP通信与依赖能否完成”，覆盖prefill、连续32步decode和训练前反向通信。它使用UB后端及GLM-5.2配置。2026-09-14的报告回答“如何把工作负载与实验管理接到SUE”，包含skills、DAG、分片、有限注入、依赖反馈和证据管理。二者是不同后端与阶段的成果，窗口时长不能直接拼成同一模型的性能演进。当前AI harness的文件对应和运行核对见第1章。

### 拓扑、放置与通信调度

16种拓扑报告覆盖纯XPU、混合MPU、大端口交换机和576卡寻址；新增的核心是统一接线生成、组选路、目的Die环转交及原口确认。96个模型窗口与25项检查支撑当时的交付和路径验证，其中不可达负例的仿真失败属于预期结果。

受控探索进一步改变流量分布、专家摆位、环资源、信用和调度。该报告显示拓扑排名会随热点与负载翻转；亲和摆位可能减少环绕行；降低消息P99也可能拉长总完成窗口。366条件中的3个512B/parity墙钟超时仍保留，不能从成功案例中抹去。资源扫描的平台区间只对应当时负载，不是通用硬件规格建议。

### 算力、内存与MoE布局

327条件报告将计算、HBM、MPU服务和网络耦合起来，讨论容量能否容纳模型、远端专家读取、Engram、冷KV恢复和prefill参数块。297个逐包完成与30个容量拒绝应分别阅读；容量拒绝不是执行失败。它采用生成时的MoE布局，不能因为当前代码改进而把历史结果视为已更新。

25项多Die MoE验收随后专门检查tp-striped布局：所有活跃TP rank参与实际通信、分片守恒、专家等待输入、返回和任务依赖。它可以补充前一批报告的布局解释，但没有替代那327条件的重新执行。

### DeepSeek具体部署与MPU/PCIe比较

DeepSeek 16卡案例已经并入第8章：TP4、DP16、EP64、MPU访问和资源瓶颈均有具体映射；`results/sue/full-rerun-20261030-unified/deepseek/verification.json`记录18例通过。原始指标保存在该目录的JSON/CSV中，没有必要再将它包装成另一份主文档。

MPU/PCIe修正版明确每颗XPU有独立PCIe链路。MPU服务率按每节点共享190GB/s核对；PCIe纯载荷下界按各芯片实际字节量计算。当前修正版撤回了共享单PCIe链路基线产生的“1.70倍优势”，`superseded-shared-link-baseline/`保留原版本。整合时采用修正版的条件和结论，保留撤回记录。

最新结果采用事件唤醒、1ps时间精度、1ns请求发起与依赖通知、IO Die尽力拼包、XPU/MPU两端各400ns接入延迟，以及按256B AXI请求并发的MPU流水线（128个在途槽位、200ns可重叠延迟、内部带宽FIFO）；同时使用事务级PCIe、双路Host共享DDR、有限DMA/信用与HBM带宽公平共享，登记180个主对比、100个比例扫描及50个独立种子确认条件。每个工作负载的1/2/4/8/16 MPU均纳入候选扫描及独立种子确认；实际完成数和曲线状态以第9章及其校验记录为准。结果整合为[第9章：MPU与PCIe卸载对比](09-mpu-pcie-offload.md)，原始报告位于 `results/sue/full-rerun-20261030-unified/offload/analysis/report.html`。曲线、基线、表格均来自本轮新模型。所有结果仍是代表块仿真，不能解释为完整模型TPOT；与早期报告的“PCIe纯载荷下界”也是不同证据。历史盘点的7份报告数量保留为当时快照，新增专题见第9章。

## 10.3 其他有价值的文档和验收材料

| 材料 | 位置 | 在这份doc中的位置或用途 |
|---|---|---|
| 早期模型方案、首轮结果与运行说明 | `archive/docs/`：Kimi/GLM通信方案、仿真结果、运行说明、NS3_QUICKSTART | 说明原始需求、首轮输入与背景；当前命令见第7章 |
| 用户拓扑原文与支持分析 | `docs/topology/topology.md`、`topology_support.md` | 原始设计要求；当前实现和路径机制见第4章 |
| 硬件JSON验收 | `results/sue/hardware-json-validation/verification.json` | 6项硬件测试、非法结构字段拒绝和默认时间戳回归 |
| 并行策略验收 | `results/sue/parallel-config-validation/verification.json` | 5项逐包案例、9种非法配置拒绝、默认时间戳回归 |
| 模型JSON验收 | `results/sue/model-json-validation-v2/verification.json` | 4项逐包案例、12种非法配置拒绝；更早目录保留测试设置被容量拒绝的记录 |
| 通信观测验收 | `results/sue/full-rerun-20261030-unified/communication/verification.json` | 4个逐包案例、3种子和观测不改变任务时间戳的检查 |
| AI harness能力核对 | `docs/manual/harness-capability-check.json` | 第1章图中能力、16个源码/skill文件哈希和118任务冒烟证据 |
| 小案例与协议敏感性报告 | `simulators/sue/harness/index.html`、`harness/validation/{threads,errors,credit-window}/report.html` | 辅助解释线程、注错和信用窗口；逐运行HTML属于原始案例证据 |

### 底座自带的20份文档

`simulators/sue/docs/`下的20份Markdown文档与最初下载ZIP逐文件比较，内容全部一致。因此它们属于底座参考资料，不能算作后来生成的20份新报告。

| 主题 | 文件 |
|---|---|
| 能力与接口 | `platform-capabilities.md`、`api-reference.md`、`app-notes.md`、`config-guide.md`、`demo-guide.md` |
| 协议与硬件对照 | `hsdk-llr-cbfc-api.md`、`uec-spec-matrix.md`、`thu-spec-compliance.md`、`thu-cf-update-verify.md` |
| 测试与验证 | `test-report.md`、`validation-report.md`、`test-cases.md`、`test-organization.md`、`cbfc-dualcell-verify-report.md` |
| 设计与排查档案 | `phase-summary-2026-08.md`、`phase-summary-2026-08-19.md`、`review-fix-plan.md`、`rtt-investigation-2026-08.md`、`scaleup-topology.md`、`cbfc-credit-size-design.md` |

此外，`docs/test-report/`中的两张信用/速率HTML曲线也属于底座验证材料。

## 10.4 统一文档入口与去重规则

主文档继续使用本手册 `docs/manual/manual.html`：第1—7章解释当前实现、运行方法与测量边界，第8章给出具体案例，第9章给出事务级PCIe与MPU的完整卸载对比，本章汇总历史报告及来源。HTML内嵌图片，完整手册可通过搜索和打印阅读；原始实验CSV、日志和报告仍保留在各自目录，方便追溯。

- 公网`/manual/`由发布服务直接映射到`docs/manual/`并禁用缓存；`results/sue/v2-topology-kimi-glm53/manual/`中的旧展示副本不作为权威内容来源。
- 当前源码仅位于 `simulators/sue/`；根目录的旧 `simai-scaleup-main`、`ns-3-ub` 链接已移除。历史记录中的旧路径保留为证据，重放命令需使用现有源码或归档路径。`results/sue/v3-topology-kimi-glm53` 等结果别名不重复计数。
- 同目录的 `report.md`、`report.html` 是同一主题的不同格式；模板、打包源码和单次运行报告不增加专题报告数量。
- `archive/results/sue-history/previous-naming-experiments.zip`内的原命名拓扑报告属于历史快照；修正前MPU/PCIe报告同样只作为版本记录。
- 本次在上述项目文档与报告目录未发现独立Word `.docx` 成品；这里的统一doc采用现有离线HTML手册。

[完整来源清单与文件哈希](report-catalog.json)记录本次盘点快照，便于后续增加材料时检查来源和版本。
