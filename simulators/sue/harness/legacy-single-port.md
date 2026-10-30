# SUE Scale-Up Ethernet 仿真平台：AI harness

当前拓扑适配版本为 **v2**，见 [v2 使用说明](v2/README.md) 和 [模型通信报告](../reports/v2-topology-kimi-glm53/report.html)。下文说明通用单口任务CLI，多Die拓扑CLI见上述链接；两个入口都属于v2。

项目沿用原名称“SUE Scale-Up Ethernet 仿真平台”，当前本地扩展版本标为 v2；AI harness 是其新增的实验编排组件。

这是 ns-3-UB harness 使用方式在 SUE 上的独立适配：自然语言实验由 Codex/AI agent 解释，仓库 skills 指导规划、参数检查、执行和结果解释；Python CLI 负责确定性的配置生成、运行与报告。CLI 本身没有内置 LLM 服务，也不需要 API key。

协议仍为原来的 SUE AFH、LLR 和所选 CBFC/PFC/OFF。没有复制 UB 协议引擎。新增后端是 `ns-3-alibabacloud/simulation/scratch/sue-harness.cc`，通过 SueEntryDispatch 建网、SueNetDevice 发包和目的端回调接入，不改 `src/sue` 与 DESIGN.md。

## 快速使用

从工作区的 simulators/sue 目录执行，Python 仅用标准库：

```bash
python3 harness/sue_harness.py build
python3 harness/sue_harness.py init harness/experiments/my-moe --workload moe --hosts 4
python3 harness/sue_harness.py validate harness/experiments/my-moe
python3 harness/sue_harness.py run harness/experiments/my-moe
```

输出中的 run 路径下有 `report.html`，可以复制到本机直接离线打开。案例目录已有时 init 拒绝覆盖；再次 run 创建新的时间戳目录，原运行证据保留。

也可以直接对 agent 说：“用 SUE 跑 8 端点 MoE，比较信用窗口 32 和 128，其他条件相同，解释完成时间和重传变化。”Agent 应读取 `.agents/skills/sue-*/SKILL.md`，写出明确配置和预测，再调用 CLI。AGENTS.md 已注册这些入口。

## 实验输入

`experiment.json` 是结构化实验定义，`traffic.csv` 是任务事实来源，`experiment-spec.md` 记录人类可读目标和预测。修改工作负载大小后需要同步修改 CSV；run 不会暗中重新生成或覆盖它。

支持的拓扑：single_switch、leaf_spine（hosts 必须整除 hosts_per_leaf）。支持 p2p、ring、moe 示例及 custom CSV。默认 MoE 示例是单个源分发到其他端点，各目的端独立等待后合并回源，不能称为完整模型 MoE 执行。

参数的完整可用集合和范围见 `sue_harness.py` 的 DEFAULT / validate_spec。未知键直接报错，不交给底层会忽略未知参数的配置解析器。主要参数：

| 位置 | 字段 | 含义 |
| --- | --- | --- |
| topology | hosts / hosts_per_leaf / spines | XPU 数、每 leaf 接入数、spine 数 |
| network | rate_gbps / prop_ns / fwd_ns | 链路 Gbps、单跳传播 ns、交换机转发预算 ns |
| network | llr / fc | LLR 开关；CBFC、PFC 或 OFF |
| network | credits / credit_size | 每 VC 信用上限、信用单元 B |
| network | cf_threshold_bytes / ack_spacing_bytes | CF 累计释放触发阈值 B、LLR ACK 周期的线字节量 |
| network | bad_every / error_rate | host 0 对端接收口每 N 帧损坏；或每链路随机帧错误概率，二者互斥 |
| runtime | payload_bytes / interval_ns | 每帧有效载荷上限 B、每任务发送／队列重试间隔 ns |
| runtime | queue_bytes | 每 host 每 VC 的待发队列容量 B，实际注入前检查 |
| runtime | visibility_ns | 依赖完成通知的固定可见延迟 ns，必须不小于 prop_ns |
| runtime | threads / seed | 单进程仿真线程数、随机种子 |
| runtime | stop_ns / timeout_s | 绝对仿真截止时间 ns、进程墙钟超时秒数 |

每个参数都进入运行快照。SUE 完整配置从项目自带 sue-400g.conf 读取后施加明确覆盖，最终写成 sue.conf；其中“400g”预置名不决定链路速率，链路速率由 topology.txt 决定。高级 SUE 参数未全部暴露在这个初版 CLI 中。

### 任务 CSV

```csv
id,src,dst,bytes,delay_ns,vc,deps,label
0,0,1,9999,10000,0,,dispatch
1,1,1,0,5000,0,0,compute-wait
2,1,0,9999,0,0,1,combine
```

- id 连续，从 0 开始；deps 是空格分隔的任务 ID，允许任意 DAG，拒绝环、重复或缺失依赖。
- 网络任务 src != dst 且 bytes > 0；本地等待 src == dst 且 bytes == 0。
- 根任务：delay_ns 相对仿真开始。其他任务：等全部依赖完成通知到达，再等待 delay_ns。
- 本地等待不占计算资源。多个等待可以重叠，没有专家或 GPU 排队模型。
- vc=0..7，以 VLAN PCP 编码并使用恒等 VC 映射；它不自动继承 UB 的优先级调度语义。

### 任务完成与多线程语义

有效载荷按 payload_bytes 分片，最后一帧保留真实剩余大小。任务／分片号通过 PacketTag 传递，仅为仿真元数据，不增加线上字节；不模拟实际上层消息头。目的端收齐所有唯一有效分片才记完成，校验源、目的和有效载荷大小，POISONED 不算成功。网络发送路径完全经过现有 AFH、LLR 和 FC。

TX 状态及依赖计数归源节点 LP，RX 计数归目的节点 LP；跨节点的依赖通知用 ScheduleWithContext，并加入 visibility_ns。该通知是调度抽象，不额外发送网络报文，1 线程与 MTP 使用相同延迟。同步根任务的事件排序可能随引擎改变，不能普遍承诺每任务时间逐位相等；必须分别比较完整性、因果约束和时间差。

每任务有独立注入节奏；多任务共享同一 host/VC 的有限队列，满时保留任务发送进度并重试，不一次性把全部数据灌进无限队列。此调度不是 NCCL 的真实线程／channel 调度。

## 导入原 UB 工作负载

```bash
python3 harness/sue_harness.py import-ub \
  ../../archive/simulators/ns-3-ub/scratch/20260910-kimi-k3-glm52-ep64/cases/kimi-k3-decode-s1/traffic.csv \
  harness/experiments/kimi-prefix --max-tasks 1000 --name kimi-prefix-on-sue
python3 harness/sue_harness.py run harness/experiments/kimi-prefix
```

只接受 URMA_WRITE / URMA_SEND，将其映射为 SUE 单向交付。保留字节、端点、phase 完成依赖和整数 ns/us/ms 等待；Read、Atomic 等拒绝导入。一个依赖 phase 有多任务时，等待它的所有任务。截取前缀必须依赖闭合，不能截断所需 phase。记录原 CSV 的 SHA256、所选行数、总行数及语义转换说明。

该导入不能用于声称 UB 事务语义等价；也不自动迁移原物理拓扑、模型 sidecar 或 UB 报告。Kimi/GLM 示例是原合成 decode CSV 的前 1,000 行，不是完整 decode 或真实模型测速。

## 单变量比较

```bash
python3 harness/sue_harness.py sweep harness/examples/moe-4 harness/experiments/loss-sweep \
  --field network.bad_every --values 0,10 \
  --prediction '错误注入应触发 LLR 重传，完整交付保持，完成时间可能增加。'
```

支持扫描 network.credits、network.bad_every、network.rate_gbps、runtime.threads。预先校验所有值，记录 matrix.json 后逐案运行，失败案例保留，不停止记录后续案例。每个案例有独立日志和报告；组级 run-ledger.json 汇总状态，并输出 comparison.csv 和 report.html 比较表。多变量实验需要单独明确交互关系。

## 输出与指标

每个 `runs/<timestamp-id>/` 保存：experiment.json、traffic.csv、tasks.tsv、topology.txt、sue.conf、validation.json、command-manifest.json（命令及输入／后端哈希）、execution.json（退出码／超时／耗时）、stdout.log、task_statistics.csv、port_statistics.csv、summary.json、report.md、report.html。

- 原始任务时间为 ps，报告时间为 µs。end 是目的端收齐，last_enqueue 是最后分片入队。
- makespan 是最早任务 start 到最后任务 end，包含中间计算等待／通知延迟；不应称为真实 TTFT/TPOT。
- replay_frames 是重传帧次数，replay_sessions 是 REPLAY 完成会话数。
- credit_exhausted_checks 是信用检查失败次数；queue_blocked_polls 是应用队列重试次数。均非阻塞持续时间。
- 端点逻辑字节量排除协议、控制消息和重传，不是链路利用率。
- 仿真截止后未收齐、因果顺序错误、LLR discard 或执行失败均标记 failed；没有 CSV 的超时也有失败报告。

运行案例台账为 run-ledger.jsonl，latest.json 指向最近一次。`analyze <run>` 可重新分析快照并生成报告。

## 验证与边界

```bash
python3 -m unittest discover -s harness -p test_harness.py
```

测试覆盖 DAG 校验、信用／队列配置、非整帧分片与 fan-in、单线程/MTP 交付和最终窗口比较、真实 LLR 重传、CBFC 背压、未完成失败、墙钟超时台账、重复运行隔离及 UB phase 导入。报告脚本以 Node DOM 模拟检查渲染逻辑、筛选和分页（不是实际浏览器截图验收）：`node harness/verify_report.js <report.html>`。

`harness/index.html` 汇总本次示例和对比结果，`python3 harness/make_index.py` 可刷新此索引。

已观察到 credits=16、payload=2048B、每任务 1ns 注入的压力场景发生停顿；32/64 信用同场景通过。失败证据保存在 harness/validation/credit-window/16。根因尚未定位，不自动改写协议或把失败排除。后续真实参数校准需保留这类结果。

未集成完整 AICB/SimCCL、动态 batching、真实 GPU/HBM 计算、真实 NCCL 调度或任意物理拓扑。这次完成的是可运行、可审计的 SUE 实验 harness 与通用 DAG 接入。

## 协议范围

整个原工程还保留 ns-3-alibabacloud 的 RoCEv2/RDMA、DCQCN 等拥塞控制及 NVSwitch 模型，详见 [基座 README](../ns-3-alibabacloud/README.md) 和 [原设计 §1.3](../DESIGN.md)。当前 v2 harness 只驱动 SUE；CBFC/PFC/OFF 是 SUE 路径的流控选择，不是后端协议选择。未提供 RoCE 后端选择器或混合拓扑案例；ENABLE_SCALEUP=0 需要外部建网回调，当前适配器没有传入该回调。
