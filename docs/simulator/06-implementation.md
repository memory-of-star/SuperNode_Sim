# 6. 源码导航、扩展方式、故障排查与局限

## 6.1 从命令到包事件的调用链

```text
bin/sue run
  hardware_run.main()
    设置SUE_HARDWARE_JSON，导入并冻结硬件
    resolve_parallel()：解析并验证TP/DP/EP
    layout_map()：导出组与专家映射
    inference_experiments.execute()
      profile()：模型结构和近似参数量
      budget()：容量和解析预算
      model_block()：生成任务、资源、token路由
      create()：验证并编译输入、生成sue.conf
      run_case()：复制输入、记录SHA、启动C++进程
        C++ main → 读TSV → Build → 注册根任务 → Simulator::Run
        Begin → FIFO或LocalPump或网络Pump
        Receive → 代理Die转交 → Delivered → SendReply → Complete
      analyze()：检查完成、字节、依赖、路径、资源互斥
      die_traffic()：统计每Die的MoE载荷
```

资源模型在Python中推算服务时长，C++负责服务队列时间线与网络事件耦合。这让容量、行为假设可以快速迭代，同时保留SUE真实逐包拥塞过程；代价是Python预算若不准确，C++不会自动补出真实GPU行为。

## 6.2 文件与职责索引

以下路径以工作区根目录为基准。

| 文件/目录 | 应该在什么场景阅读或修改 |
|---|---|
| bin/sue | 统一入口和默认硬件选择 |
| hardware/profiles/current.json | 用户确认的物理规格与显式效率假设 |
| configs/parallel/*.json | 可复用的并行策略 |
| simulators/sue/harness/v2/hardware_config.py | 单位换算、严格字段检查、快照保存 |
| .../hardware_run.py | CLI、配置优先级、单case/矩阵分发 |
| .../parallel_config.py | TP/EP边界、DP推导、token/专家映射 |
| .../model_config.py | HF模型JSON、补充假设、架构校验与来源快照 |
| .../inference_profile.py | 模型参数、KV与容量公式、解析预算 |
| .../inference_experiments.py | DAG生成、TP collective、MoE、MPU读、矩阵定义 |
| .../inference_prefill.py | 参数chunk阶段设计 |
| .../inference_restore.py | 冷KV页恢复阶段设计 |
| .../moe_traffic.py | 应用任务到每Die流量的归因 |
| .../topologies.py | 物理端口表、VRF、代理Die、哈希与可达性 |
| .../workloads.py | 旧拓扑合成流量和通用task构造 |
| .../run.py | 输入编译、构建、执行、逐项验证和运行台账 |
| .../inputs/ | 冻结模型配置、来源和模型参考实现 |
| simulators/sue/ns-3-alibabacloud/simulation/scratch/sue-harness-v3.cc | 多Die适配器、资源FIFO、本地环、任务生命周期 |
| .../src/sue/model/sue-net-device.* | 设备发送、LLR/FC集成与队列出队 |
| .../src/sue/model/sue-channel.* | 链路事件和传播 |
| .../src/sue/model/sue-switch-node.* | VRF转发、交换机出口与反压 |
| .../src/sue/model/sue-llr-engine.* | 逐链路可靠性状态机 |
| .../src/sue/model/sue-cbfc-engine.* | 信用窗口、计数、更新 |
| .../src/sue/network_frontend/sue-config.* | 配置解析和设备应用 |
| simulators/sue/DESIGN.md | 底座协议实现基准；其中历史里程碑不等于所有入口当前暴露的功能 |

Python模块中的`HERE/ROOT/SIM`按当前源码路径定位；硬件配置从祖先目录定位工作区。目录迁移后根目录保留旧路径软链接，以维护旧编译路径和实验绝对路径。不要随意删除链接或单独复制一个py文件后期待找到全部输入与二进制。

## 6.3 如何增加一个模型

1. 冻结官方配置，保存来源URL、revision和SHA，明确模型版本。
2. 已支持的三个架构，直接修改 `configs/models/<模型>/config.json` 与 `simulation.json`；新架构则扩展 `model_config.py` 校验和 `profile()/kv_budget()` 行为，避免只按总参数反推所有细节。
3. 为`kv_budget()`补充容量与实际访问近似，写出哪些注意力算术尚未覆盖。
4. 确认隐藏/专家宽度与TP分片相容；当前实现不自动padding。
5. 用小规模案例检查token路由数量、专家计算量、数据字节守恒和容量，再加入矩阵。
6. 不在没有执行完整图的情况下把代表块称作该模型完整性能。

如果模型没有MoE，不能直接套用top-k专家框架当作dense模型；应新增对应DAG生成器。

## 6.4 如何增加新的并行机制

当前并行配置只允许连续rank分组和EP包容TP。要支持跨EP边界的TP、专家内部TP>1或PP>1，必须同时修改：参数校验、权重/激活容量、任务位置、集合通信、专家调用依赖、流水微批调度以及统计口径。

例如专家TP4不能仅把每Die专家权重除以4：还需要把专家计算分布到4rank，增加相应通信，并定义dispatch先到哪个rank、结果如何归并。PP2不能只把总层数除以2：需要阶段映射、阶段间激活、微批数量、流水气泡和不同阶段资源占用。

若实现自定义rank映射，应先产生可审计的布局表，再让容量和工作负载共同使用该表，避免一个模块认为专家在Die0、另一个模块把流量发给Die1。

## 6.5 如何修改网络行为

硬件链路速率优先改硬件JSON；信用、队列、分片实验优先使用已有settings接口。新的随机延迟、错误模型或交换机调度涉及C++行为变化时，应在最小拓扑上先验证守恒、时序与基准回归。

协议核心是受控资产。当前工作主要通过外部适配器与Python harness扩展，没有替换LLR/CBFC/AFH算法。修改核心前阅读项目AGENTS与DESIGN，避免把某个实验的快捷假设写进所有设备默认行为。

## 6.6 复现与校准建议

复现同一实验需要硬件原始JSON、解析并行配置、模型冻结输入、task/resource/port输入、协议conf、二进制及动态库版本、seed和命令。单有同名JSON或截图不够。结果manifest的绝对路径是原机器的位置；跨机器复现需保留相对结构并重新生成运行记录，不应伪造旧SHA。

优先校准顺序：单链路小/大消息延迟与吞吐 → 单交换机转发 → 多流/信用窗口 → D2D环与反压 → 单Die计算/HBM服务 → 实际collective → 模型真实route/算子trace。每层校准后再讨论模型级预测；不要靠调一个效率系数同时吸收所有网络、kernel和调度误差。

## 6.7 常见故障与定位

| 现象 | 首先检查 | 处理原则 |
|---|---|---|
| 输出目录已存在 | 是否重复使用`--output` | 换新目录，不覆盖旧证据 |
| TP×DP不等于rank | CLI覆盖了TP但JSON固定了DP | 改为匹配DP或使用null自动推导 |
| EP必须是TP倍数 | 当前组边界是否交叉 | 当前不支持交叉布局，不能忽略报错 |
| expert_tp/pp报错 | 是否设置大于1 | 尚未实现，不用缩小服务时间假装支持 |
| capacity_rejected | die_weight_GB、die_kv_GB、EP副本数、MPU容量 | 先解决容量，再比较速度 |
| 新JSON没有改变延迟 | 改的是容量或未建模PCIe吗？是否被矩阵override覆盖？ | 查看hardware.json、case设置、实际命令，不只看文件名 |
| run返回失败 | stdout.log、execution.json、failures | 区分进程错误、路径错误与未完成 |
| 墙钟超时 | 是否任务太多、消息太碎、同时跑太多进程 | 保存失败，再明确调整超时/工作量；不要当作网络丢包 |
| 仿真时间到仍未完成 | 路径、组选择、信用、资源等待、stop_ns | 不把部分完成当成功 |
| 各Die流量不均 | token归属、专家热度、布局、端口哈希 | 区分发送端与专家返回端，不制造无用流量凑满 |
| 旧公网链接失效 | ops/report-hosting/deployment.json和隧道日志 | 临时域名重启会变，报告路径可保持 |
| 找不到模型/硬件/二进制 | 是否脱离工作区运行了复制出的脚本 | 使用bin/sue和规范目录 |
| 目录迁移后需大规模重编译 | CMake记录了原编译路径 | 允许重建，保留旧路径链接，完成后做时间戳回归 |

## 6.8 明确没有做的事情

没有完整GPU ISA模拟、真实kernel执行、精确DRAM时序、缓存层次、功耗/温度、PCIe路径、生产级KV分页器、请求到达和continuous batching、真实专家门控分布、全模型流水训练、故障恢复服务编排。没有通过FP4/FP8峰值自动推导真实量化精度损失或输出质量。

这些缺失不是说当前网络实验没有价值，而是决定了结论能延伸到哪里：可以在明确工作负载与行为假设下比较SUE拓扑、带宽和布局的作用，不能把未经校准的代表窗口直接转换为生产SLA。

## 6.9 文档维护与生成

本手册Markdown为权威源，`docs/simulator/build_manual.py`将章节、配置示例和架构SVG生成单页离线HTML。运行：

```bash
python3 -m pip install markdown-it-py
python3 docs/simulator/build_manual.py
```

生成器同时保存`manual-manifest.json`，记录文档、配置样例和关键实现源码的SHA，方便识别文档对应哪版代码。修改并行/硬件行为时应同时更新字段表、示例和验证结果，不修改已有历史报告来伪装新实现结果。
