# 芯片与内存节点硬件配置

这里集中保存资源联合推理 harness 的硬件参数。`profiles/current.json` 是目前确认的规格；`profiles/example-half-scaleup.json` 只将 scale-up 双向带宽减半，便于对照。当前模型配置在 `configs/models/`，历史参考输入保留于 `simulators/sue/harness/v2/inputs/`，拓扑规则仍在 `docs/topology/topology.md` 及 harness 中；旧实验内部数据不改写，统一存放在 `results/sue/`。

## 参数与单位

所有容量、带宽都采用十进制：GB=10^9 byte，TB=10^12 byte。字段 `bytes_s` 是 byte/s，`port_bits_s` 是 bit/s；不是同一个单位。

| JSON路径 | 当前值与含义 | 使用位置 |
|---|---|---|
| chip.dies | 4 Die/芯片 | 当前固定，只接受4 |
| chip.fp4_tflops_per_die | 每Die 900 TFLOPS | FP4计算资源服务 |
| chip.fp8_tflops_per_die | 每Die 450 TFLOPS | FP8计算资源服务 |
| chip.fp16_tflops_per_die | 每Die 225 TFLOPS | FP16计算资源服务 |
| chip.memory_capacity_bytes | 每芯片144 GB，均分36 GB/Die | 容量检查 |
| chip.memory_bandwidth_bytes_s | 每芯片15 TB/s，均分3.75 TB/s/Die | HBM资源服务 |
| chip.d2d_one_way_bytes_s | 每芯片单向2 TB/s；双向合计4 TB/s | 均分4 Die、每Die再分两个邻接方向：环每条有向边250 GB/s |
| chip.scaleup_bidirectional_bytes_s | 每芯片双向1.6 TB/s；单向0.8 TB/s | 每Die单向200 GB/s，每端口200 Gbit/s；真实SUE链路速率随此值变化 |
| chip.scaleup_ports_per_die | 8端口/Die | 当前固定，只接受8 |
| chip.pcie_bytes_s | 每芯片128 GB/s，四Die共享 | 普通SUE入口不生成PCIe流量；卸载入口另由PCIe JSON配置端点与共享资源 |
| memory_node.capacity_bytes | 每MPU 512 GB | 远端容量检查 |
| memory_node.memory_bandwidth_bytes_s | 每MPU 190 GB/s | MPU共享服务队列 |
| memory_node.scaleup_ports | 每MPU 8端口 | 当前固定，只接受8 |
| memory_node.port_bits_s | 每端口200 Gbit/s | MPU接入链路速率，独立于XPU端口速率 |

`assumptions` 单独存放建模假设，不是芯片规格：计算效率30%、HBM效率50%、MPU效率100%、MPU固定延迟200ns、容量预留20%。`kv_element_bytes=2` 是当前KV模型的固定元素宽度，暂不支持其他值。D2D每方向速率目前要求可换算为整数Gbit/s。

## 修改并运行

从工作区根目录 `/home/dell/scale_up_evaluation` 执行。先复制配置，再修改副本，便于保留基准。

```bash
cp configs/hardware/profiles/current.json configs/hardware/profiles/my-chip.json
# 编辑 my-chip.json 中需要调整的参数

# 校验并打印换算后的每Die/每端口参数，不执行仿真
python3 simulators/sue/harness/v2/hardware_run.py \
  --hardware configs/hardware/profiles/my-chip.json \
  --output results/sue/my-chip-check --validate-only

# 首次或C++适配器变化后构建
python3 simulators/sue/harness/v2/run.py build

# 一个GLM 5.3条件；MoE默认多Die分片收发
python3 simulators/sue/harness/v2/hardware_run.py \
  --hardware configs/hardware/profiles/my-chip.json \
  --output results/sue/my-chip-glm \
  --model glm-5.3 --chips 8 --tp 4 --batch 8 --context 8192
```

输出目录必须不存在，防止混用不同规格的数据。每次保存 `hardware-profile.json` 原始配置、`hardware-source.json` 来源路径和SHA256、`hardware.json` 换算参数、`design.json` 条件、`results.json` 结果。单个case元数据还保留硬件参数及来源；端口速率和环速率写入case设置及运行命令。

DeepSeek可加 `--mpus 2 --engram-remote`，芯片数、TP、batch等按条件设置。容量不足会留下 `capacity_rejected` 记录，不伪造性能。

`--mode matrix` 使用现有267条件主矩阵，`--mode restore` 跑24项KV页恢复，`--mode prefill` 跑36项参数块；每种模式使用一个全新的输出目录。这三个模式采用各自预设的模型、规模和种子，单case选项只对 `--mode case` 生效。改变硬件后，原先可行的条件可能被容量检查拒绝。

旧脚本默认也读取 `current.json`；高级用法可用环境变量 `SUE_HARDWARE_JSON=/绝对路径/profile.json` 选择配置。优先使用上面的显式入口以自动保存根目录快照。实验里的效率敏感性扫描会覆盖JSON中对应效率参数，case内保存最终值。

此入口输出机器可读结果；旧327项交互报告含特定硬件的文字，不应直接拿它作为任意新规格的报告模板。结果仍是代表块/阶段实测，不是完整模型TPOT或TTFT。

工作区已整理，推荐使用根目录 `bin/sue check`、`bin/sue build` 和 `bin/sue run`。详见 [工作区说明](../../README.md)。

并行策略现已独立配置：使用 `--parallel configs/parallel/tp4-global.json`，以及 `--tp/--ep/--dp/--moe-transport` 覆盖。完整字段与支持边界见 [手册第2章](../../docs/manual/02-configuration.md)。

## Scale-Up 接入延迟

当前配置通过`scaleup_latency.xpu_port_delay_ns`和`scaleup_latency.mpu_port_delay_ns`分别设置XPU、MPU端口到网络的单向接入延迟，均为400 ns。它是端口/接入路径的等效固定时间，替代原来的每段20 ns传播设置，不叠加计算。数据、确认和信用控制帧都使用该延迟，不消耗额外序列化带宽。单交换机小包基准为400＋300＋400＝1100 ns，另计入口序列化及拥塞排队。

字段接受0～1,000,000的整数纳秒，可独立修改。未提供该配置块的旧硬件JSON保留兼容设置；已冻结实验输入不会自动改变。更换硬件JSON后需重新生成实验。

### MPU并发访问与FIFO带宽

当前硬件JSON的`memory_node.max_inflight_requests`默认128，可改成1～65536的整数。它控制每个MPU内存引擎的在途请求槽位数，是可调建模参数，不是实测硬件规格。多个请求可并发等待访问延迟，数据服务仍按FIFO共享单节点带宽预算；`memory_node`原有带宽和效率设置继续生效。修改后须重新生成案例，直接运行旧冻结案例不会应用新配置。省略该字段时，未启用AXI层的旧入口保留原串行批服务；显式启用AXI模式时适配器兼容缺省值为64槽位。当前current.json明确配置128。

### IO Die

AXI事务与拼包的独立配置见[io-die/README.md](io-die/README.md)。支持256 B事务、尽力拼包和不拼包两种模式。
