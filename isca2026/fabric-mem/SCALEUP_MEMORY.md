# Scale-Up 域内存池：直接相关证据与新候选机制

核查日期：2026-11-03。**已有学术研究和产业方案，不能把“Scale-Up 域内接入独立内存池”当作首次提出的概念。** 上一轮主要从 LLM offloading 检索，漏掉了 ScalePool、UnifabriX、Photonic Fabric 等直接相关工作；本次已补入 [资料库](ref/README.md)。

本目录由仓库根 `isca2026_mpu/` 整体迁入 `isca2026/fabric-mem/`。名称只描述 fabric-attached memory 研究对象，不预设最终机制。模板、工具链、缓存、PDF 和文献原文均随目录保留。

## 1. “有人做过吗”的精确回答

需要分开三件事：用其他 GPU 的 HBM；通过 CPU/CXL 路径访问共享内存；让独立内存设备成为加速器 Scale-Up fabric 的端点。前两者不能自动证明第三者，但本次也找到了第三类的直接证据。

| 工作 | 实际关系与证据 | 与我们的区别及证据边界 |
| --- | --- | --- |
| **ScalePool，2025** | 题目明确 Unified Scale-up Domains；XLink 负责域内加速器连接，CXL 负责跨集群及独立 tier-2 内存池。正文 §5–6 描述纯内存节点及 preliminary evaluation | XLink/CXL 混合、分层 fabric；主要应用评估是 LLM 训练。采用低层建模、原型测得的交换延迟和高层仿真，不是完整推理整机实测 |
| **UnifabriX MAX / MPU** | 官方产品页直接称 Scale-up Memory Tier，并列 CXL、UALink、ESUN/SUE；UALink 联盟 2025 年 12 月演示第 23 页画出独立机架内存节点和 Scale-up fabric ports | 与我们 MPU 形态非常接近。网页/演示能证明公开架构与产品主张，不能证明所有协议组合都已量产或达到宣称延迟 |
| **Astera Labs Leo X-Series，2026-09-15** | 发布稿宣布 GPU fabric-attached memory；补充技术文章报告 4×H200、Scorpio P-Series、vLLM＋修改版 LMCache 的实机 KV offload 测试 | 已超出产品概念，属于厂商报告的实机实验。具体测试走 PCIe P2P，不能推广为 UALink/SUE 验证；Leo X 与 CPU/CXL 的 Leo 2 E/P 也须区分 |
| **Photonic Fabric Platform，2025；Photonic-CXL Memory Appliance，2026** | 光互连共享内存模块/appliance，独立于计算扩展；后者专门研究 LLM KV 缓存 | 要区分平台的 XPU 接入选项与论文具体 CXL-host/PF-NIC 路径。后者结合 GPU 系统测量、硬件 emulation 和服务仿真；不能把三者合成整机实测 |
| **CMM-MP，2026** | 主机 CXL 接入、Ethernet 后端独立内存池，已有 FPGA 实现和应用验证 | 拓扑相近，但主机 CXL→设备→Ethernet 不能直接等同 GPU 原生加入同一 SUE Scale-Up 交换域 |
| **CXL-CCL，ICS 2026；Octopus，NSDI 2026** | 前者已通过 CXL 共享池实现跨节点 GPU 集合通信；后者研究 CXL scale-up pods 与多端口内存拓扑 | 不能宣称“内存池参与通信”或“池化拓扑”本身首次；具体路径、同步语义与 GPU/CPU 角色不同 |

对应一手来源：[ScalePool](https://arxiv.org/abs/2510.14580)、[UnifabriX 产品](https://www.unifabrix.com/product)、[UALink 联盟演示](https://ualinkconsortium.org/wp-content/uploads/2025/12/UnifabriX_UALink-Webminar_20251210_PDF.pdf)、[Leo X 发布](https://www.asteralabs.com/news/astera-labs-bolsters-leo-smart-memory-controller-family-for-agentic-ai-and-general-purpose-cloud-workloads/)、[Photonic 平台](https://arxiv.org/abs/2507.14000)、[Photonic KV 论文](https://arxiv.org/abs/2607.27187)、[CMM-MP](https://arxiv.org/abs/2610.22716)、[CXL-CCL](https://doi.org/10.1145/3797905.3807846)、[Octopus](https://www.usenix.org/conference/nsdi26/presentation/zhong)。本地文件见对应 [资料卡](ref/README.md)。

准确结论是：**概念已存在；具体部署的成熟度和验证深度因工作而异。当前资料不足以证明“某个公开系统已完整实现并验证我们设想的 SUE MPU 推理栈”，也不足以支持“没人做过”。** 缺少完全相同实现不自动构成学术新颖性。

## 2. 为什么仅改信用策略还不够强

上一版的读准入可以作为实现基础和强基线，但与通用传输调度的距离太近。新发现又进一步收窄了可声称的贡献：

- **直接读取不经 HBM 中转已有 DAK。** 它用 TMA 从远端 host memory 取权重/KV 到 GPU SMEM，并研究局部拥塞与 CTA multicast。我们不能单独拿“少一次 HBM 落地”作新点。[论文](https://arxiv.org/abs/2604.26074)
- **MLA KV 单份存储、主 rank 取回再广播已有 LMCache。** 官方实现有 `save_only_first_rank`、广播路径和 leader GPU buffer，因此“把多份合成一份”也不是空白。具体调用链与版本需要在实验时固定。[实现](https://github.com/LMCache/LMCache/blob/dev/lmcache/v1/cache_engine.py)
- **按计算角色拆分池已有 CrossPool。** 它把 FFN 权重与 KV/attention 分到不同 GPU 池；这是有计算能力的 GPU 池，不能直接当纯 MPU，但“计算与存储位置解耦”的动机已经相当拥挤。[论文](https://arxiv.org/abs/2606.24506)
- **把地址追踪/gather 下放远端已有 Tiara。** 它的可编程远程访问包含 PagedAttention 和 MoE gather 示例；普通 gather offload 需要更明确差异。[论文](https://arxiv.org/abs/2606.13708)

此外，Scale-Up 多播本身有公开的 [IETF 个人草案](https://datatracker.ietf.org/doc/html/draft-wang-mcast4ai-p2mp-scaleup-00)。它不是已批准标准，也不是我们已有实现，但足以说明不能把“网络支持多播”独占为新点。

## 3. 更值得检验的候选：把远程读取与多节点分发合成一个事务

暂定题目：**Read Once, Deliver Many: Group-Aware Memory Transactions for Scale-Up Inference**。

中文：**一次读取，多节点交付：面向 Scale-Up 推理的组感知内存事务**。

核心问题不是“有没有内存池”，而是：**独立内存池已经持有多节点共同需要的数据时，为什么还要重复远程读取，或先送到一个 GPU 再启动一轮分发？能否让内存读取直接生成最终消费者需要的交付，同时保留每个消费者独立的完成和背压？**

### 一个足够具体的场景

某些 MLA、MQA/GQA 与 TP 布局中，多 rank 会需要相同 KV 内容。适用范围取决于模型和 TP/DCP/PCP 布局，不能把所有 KV 访问都视作相同。[TensorRT-LLM 并行说明](https://nvidia.github.io/TensorRT-LLM/1.3.0rc18/features/parallel-strategy.html)、[vLLM 模型相关并行设计](https://vllm.ai/blog/2026-09-08-vllm-agentx)。

以 r 个消费者需要同一不可变 B-byte 数据块为例，有三条候选路径：

| 路径 | 内存池/共享路径可能付出的代价 | 不能省略的成本 |
| --- | --- | --- |
| 各 rank 独立读 | 无请求合并或缓存时，可能有 r 次底层读与 rB 返回；实际以计数为准 | 每个 rank 的请求、响应、接收与完成 |
| 主 rank 读后广播 | 池先返回 B，GPU 再分发；这是必须认真优化的基线 | 中转端资源、广播启动、流水与最终交付；中转不必总是完整 HBM 落地 |
| MPU 组事务 | 一次底层读直接服务一组消费者，分支复制并管理各自完成 | 分支末端仍至少交付 rB；多播表、缓存、分支信用、地址映射、重试和慢接收端状态 |

若只有 MPU 发出 r 份单播，就只可能减少底层 DRAM 读，不会自动把 MPU 上行流量由 rB 降为 B。只有在支持分支复制的路径上，才可能减少共享链路字节。实验必须按每条链路及每个资源核算，不能用“读一次”偷换“网络只传一次”。

### 候选机制的边界

第一版只处理相同保护域、已封口不可变 KV block 或只读权重 tile。运行时提供 block/版本、消费者组和目标位置；不是要求 MPU 执行 attention，也不做任意共享写一致性。

每个组事务包含一个共享读取描述符与多个独立消费者完成记录。消费者有自己的地址映射、事务 ID 和接收预算；可准备的分支先完成，慢分支不能永久占住整组资源。用有界缓存保存待交付内容；超过保留预算时允许退化为同版本重读/单播，并如实计入代价。尾部 KV 写入、块回收和取消需要版本/生命周期规则。

真正值得研究的困难在于：**复用需要保留共享状态，尾延迟隔离又要求尽快释放资源。** 等齐所有人会形成屏障，完全独立发又丢掉复用；如何根据实际消费者就绪度与有限预算决定分组、拆组和回退，才可能形成一个可验证的事务机制。避免仅把传统 read coalescing 或 multicast 更名。

这需要扩展端点/运行时接口及可能的交换路径，不是声称现有 AXI、SUE 或 UALink 自动支持任意多目的响应。若只能在 MPU 做合法单播扇出，先评估该受限版本，再决定是否值得增加交换机功能。

### 与最近工作的关系

- 对 DAK：候选对象是跨设备消费者组与独立内存端点的事务完成；DAK 的直接访问和 GPU 内 TMA multicast 必须作为实现/对照基础，不能计为新增贡献。
- 对 LMCache：必须超过“单份存储＋leader retrieval＋优化广播”，不能只击败重复 PUT/GET 的弱实现。
- 对 CXL-CCL：其已证明共享池可承载集合通信；要明确比较“既有池中数据的读与最终交付融合”相对其同步/中转路径究竟减少了什么。
- 对通用多播：重点是内存对象版本、返回承诺、每个请求的退休与有限缓存下的慢分支隔离。若普通多播加简单 per-destination 队列已实现同样效果，就没有足够新贡献。

**目前只能称候选机制，不能称首创。** 本次检索并未建立全面的新颖性证明。

## 4. 先做三个筛选实验，而不是直接写控制器

1. **重复性是否足够？** 从实际内存访问轨迹提取 `(对象, 版本, offset, length, 消费者, ready time)`，画重复读比例、组大小、到达时间差与生命周期。现有合成地址和总字节预算不足以证明存在跨 rank 复用。前缀恢复与活跃 decode 分开。
2. **强基线能否已经解决？** 比较独立读、主 rank 读后流水广播、MPU 单播扇出、普通多播、候选组事务；另比较适用的 DCP/DP 布局，保持模型质量和总体资源预算。DCP 减少复制但增加其他通信；若全面更优，应停止硬件方案。[vLLM 说明](https://vllm.ai/blog/2026-09-08-vllm-agentx)
3. **慢分支会不会吞掉收益？** 在固定总缓存、信用、端口及 DRAM 服务能力下，扫描真实消费者时差与接收拥塞；报告公共路径字节、内存读放大、分支状态峰值、退化重读和最终阶段时间。需要记录完整依赖及对其他流量的影响，不只看多播吞吐。

决定性图应同时展示：随重复度与消费者时差变化，独立读、leader 广播与候选机制的胜负区域。若只有同步、全相同、无缓存限制的理想点胜出，价值有限。若实际重复度低，或普通广播/分片已经解决，停止这条路线。

## 5. 第二候选：固定池中状态，弹性改变计算布局

另一个较有架构含义的问题是：**KV 物理布局和所有权能否独立于当前 TP/DP/DCP 组，使并行度改变时主要更新视图，而不是迁移整份 KV？**

潜在机制为稳定的块命名/物理分片、带版本的执行视图，以及按新计算组选择直接读取、临时复制或局部恢复。真正目标是节省重配置成本，同时控制之后每步付出的远端访问代价；“没有迁移”不等于“没有数据传输”。

这比通用信用调度更接近系统架构，但与 MemServe、CrossPool、弹性 TP、布局无关 KV 缓存等已有系统重叠，必须继续专项查重；需要真实服务执行器和算子布局支持，当前事务仿真器单独不能证明收益。现阶段优先验证上一节的读与分发融合，因为其关键变量更接近我们已有 IO/事务/队列模型。

## 6. 现在最稳妥的研究定位

独立内存池是研究前提；SUE 是实验平台与协议约束；校准仿真器是证据工具。可能的贡献来自：**发现“单份存储仍产生重复服务/中转”的明确瓶颈，设计受限且可实现的组事务，并证明在强软件/网络基线之上仍有收益。** 上一版信用准入保留为基础机制，暂不独立承担论文主张。

本次只更新文献与研究方案，未实现组事务或改动仿真行为。英文稿仍为原提案，后续有证据后再改写。

## 7. 正文阅读后的证据深度修订

以下页码均为本地 PDF 页码。这里只评价已下载版本所公开的证据，不推断作者没有其他内部实现。

- **ScalePool，第 4–7 页：**讨论混合互连、路由、两级内存及一致性设计；§6 明确为 Preliminary Evaluation，低层链路/事务排队建模使用原型交换延迟，再接高层 LLM 模型。应用为训练，不能把其训练加速当作活跃 KV decode 的验证。已核实正式出处为 DIMES 2025，DOI `10.1145/3764862.3768173`。
- **UnifabriX，演示第 19–28 页：**独立内存节点、交换机/内存池双角色、CXL/UALink/Ethernet 接口、Fabric Manager 与 Memory OS 已有架构展示。第 28 页明确把 UALink Fabric NIC 标为 prototypes、CXL 路径标为 stable，并给出相对延迟/带宽图；不能说完全没有原型证据，但图中没有足以复现的完整测试方法，也未提供 LLM 工作负载与消融。
- **Astera，技术文章：**4×H200、TP=4、Qwen2.5-32B、vLLM、LMCache v0.4.2＋patch；10 个 32k-token 多轮会话，并以 CPU 内存压力线程制造共享 DDR 争用。32 个压力线程下 warm TTFT 从 413 ms 到 158 ms；这是指定负载下的厂商实测。没有压力时为 246 ms 到 127 ms，不能将 62% 归到所有场景。主要验证 KV 恢复路径与 CPU DDR 隔离，不是每步远程 attention、跨 rack SUE 或严格尾延迟保证。硬件清单与后文对 Leo 容量的表述需在复现前澄清。
- **Photonic 2025，第 4–10 页：**讨论 HBM 缓存＋DDR、共享内存、TP/集合通信、训练数据搬运能耗、LLM 推理和 DLRM。CelestiSim 在 H100/H200 的 TensorRT-LLM 测量上验证，文中报告 MAPE 7.57%；PFA 本身仍是预测。表 5 同时改变内存资源与并行配置，不是简单替换同等资源链路。尤其第 9 页已把容量缓解 TP、减少重复访问及通信开销作为核心解释，所以“内存池改变并行度”也不能直接当新贡献。
- **Photonic-CXL 2026，第 6–10 页：**已有 Veloce 上 PF-NIC/PF Memory Module emulation、QEMU/CXL 事务、地址交错、32B–1KB 访问，以及 idle/loaded 延迟。第 7 页明确分析 outstanding 超过信用限制后排队、背压及读返回长尾；其延迟分解不覆盖 QEMU 请求发出到 RC 的不可观测部分。第 8 页的软件集成含 offset 寻址、DMA 完成与 rendezvous 顺序设计，但 connector 存在将来时表述；第 10 页明确物理硬件端到端推理验证和多主机服务评估尚待完成。第 9 页的 6.6× 来自服务仿真中 2 TB host DRAM 与额外 32 TB 池的容量/缓存命中差异，不能归因于纯光链路时延。

因此，信用/背压、减少 TP 通信、KV offload 与独立内存设备都已有较具体论证。后续比较应对准可复现的完整机制及其边界，不能把“对方论文未展示某图”当作“对方没有考虑该问题”。
