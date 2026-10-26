# 5. 输出文件、指标解释、验证与实验设计

## 5.1 输出目录是什么结构

```text
实验目录/
  hardware-profile.json        原始硬件配置内容
  hardware-source.json         原文件位置、SHA、内容
  hardware.json                每Die/端口换算后的有效参数
  parallel.json                单case入口解析后的并行策略
  parallel-source.json         并行原始JSON、SHA及CLI覆盖
  parallel-layout.json         实际TP/EP组、rank与专家位置
  invocation.json              顶层命令与cwd
  design.json                  运行前定义的条件
  results.json                 全部条件结果，包括容量拒绝
  cases/某个条件/
    experiment.json           拓扑、settings、工作负载元数据
    parallel.json             该case实际策略
    parallel-layout.json      该case映射
    tasks.json / tasks.tsv     DAG原始输入与编译输入
    resources.json / .tsv     计算/访存/本地环侧车
    token_routes.csv          合成专家选择与owner
    ports.csv / ports.tsv     物理端口映射
    path_plan.csv             运行前预期端口/代理Die/跳数
    sue.conf                  协议设置
    validation.json           输入结构校验
    runs/时间戳-唯一ID/
      上述输入的逐份副本
      command-manifest.json   可执行命令、二进制/源码/输入/库SHA
      execution.json          退出码、墙钟耗时
      stdout.log              C++输出
      engine.json             任务总数、完成数、路径错误、结束时间
      task_statistics.csv
      resource_statistics.csv
      port_statistics.csv
      ring_statistics.csv
      moe_die_traffic.csv     资源MoE实验额外生成
      summary.json            通用分析器结果
```

预设矩阵可能不生成根目录统一parallel.json，因为每个case的TP不同；它们以case级元数据与布局为准。容量拒绝可能只有设计和结果，没有cases/runs。专用`window_us`与`per_step_block_us`由上层写入根`results.json`，不能假设通用`summary.json`含全部专用字段。

## 5.2 DAG和资源输入字段

任务字段：`id`连续编号，`src_chip/src_die`、`dst_chip/dst_die`、`bytes`、`delay_ns`、`deps`、`label`、`group`、`vc`。依赖必须指向更早任务；消息正字节必须跨SA；0字节等待必须同SA。group=-1自动选组，0/1强制组；VC为0–7。

`delay_ns`是无依赖任务的启动时刻，或有依赖任务在父完成通知之后的额外等待，不是网络传播时延。资源生成器通常使无依赖根任务在1000ns开始。

资源侧车以同id关联一个0字节本地任务：

| kind | 含义 | units | service_ns / peer_die |
|---:|---|---|---|
| 0 | Die计算FIFO | 近似FLOPs | 正服务时间；peer=-1 |
| 1 | Die HBM FIFO | byte | 正服务时间；peer=-1 |
| 2 | MPU内存FIFO | byte | 正服务时间；peer=-1 |
| 3 | 本地D2D环传输 | byte | service=0，peer为其他Die |

无侧车的0字节任务只是本地等待，不会进入计算/HBM队列。这使旧固定等待实验可以继续复现，但也意味着用户手写一个等待任务并不能声称建模了GPU竞争。

## 5.3 任务时间戳

| 字段 | 解释 |
|---|---|
| ready_ps | DAG依赖满足后的可见时刻 |
| start_ps | 任务开始，资源任务此时可能尚需排队 |
| data_end_ps | 网络载荷完整到最终目的，或本地服务完成 |
| end_ps | 任务最终完成；网络任务通常还要等64B响应回到源 |
| service_start_ps | 资源真正开始服务 |
| queued_ps | 资源开始服务减任务start，不是整个应用排队 |

网络任务时延`(end-start)/10^6`µs包含它启动后的传输与完成响应，不包含启动前的上游DAG等待。资源排队和环排队是不同计数。

## 5.4 主要指标的口径

| 指标 | 计算与含义 | 不能直接解释成 |
|---|---|---|
| makespan_us | 全部任务完成时，max(end)−min(start) | 全模型TPOT |
| window_us | 资源实验max(end)减固定1000ns起点 | 任意自定义DAG都通用的窗口 |
| per_step_block_us | window_us / 代表步骤数 | 每生成一个真实token的延迟 |
| latency_p50/p99_us | 正字节网络任务时延的经验分位 | 请求级P99或每个模型token的P99 |
| wall_s | 程序墙钟耗时 | 模拟的芯片处理时间 |
| logical_bytes | tasks里正字节载荷之和 | 全部线上字节或本地D2D字节 |
| received_bytes | 应用接收载荷 | LLR重放流量 |
| wire_bytes | 链路Trace记录的Packet大小累计 | 单跳goodput；多链路求和会重复计同一载荷 |
| tx_busy_ps | 统计窗口中设备发送占用时间 | 包含所有IFG/处理等待的应用忙碌时间 |
| credit_exhausted_checks | 信用不足检查次数 | 丢包数或阻塞总时长 |
| queue_blocked_polls | 任务因出口/池资源重试次数的相应统计 | 不可直接相加为关键路径时间 |
| resource_queue_us | 各资源任务排队时间总和 | 整个窗口增加了多少延迟 |
| ucie_byte_hops | 各环边传输字节总和 | 唯一有效载荷；2跳会记两次 |
| ring_peak_bytes | 芯片共享池峰值中的最大值 | 所有芯片峰值之和 |

`wire_bytes`遵从Trace中`Packet::GetSize()`口径，不应凭字段名自动认为包含所有PHY编码开销。端口CSV的side=0是host端、side=1是交换机端，计算出口利用率要明确选择端点和时间窗口，不能把两个side直接加成某个Die的吞吐。

`moe_die_traffic.csv`把完成的dispatch/combine任务按实际源/目的Die归因，分开fabric与D2D。其平均GB/s用载荷/整个窗口计算，窗口包含计算等待，不是峰值端口能力或纯通信阶段利用率。rank内不经过网络的片不计入该表。

## 5.5 成功判据与失败

通用分析器要求：C++退出码0、engine.ok、每个任务完成且载荷守恒、端口/代理Die/环跳数与计划一致、依赖时序满足、没有非法路径或LLR丢弃；资源服务时长准确、同一FIFO服务区间不重叠、本地D2D交付字节正确。

- `passed`：该定义条件完整完成。
- `capacity_rejected`：全模型预算未通过，没有执行性能仿真。
- `failed`：进程失败、任务未完成、路径错误或统计校验失败。
- 外层`runner_error`：Python编排异常，需读错误堆栈。
- 墙钟超时通常记录退出码124；仿真stop_ns截止但未完成同样是失败，不能纳入均值。

不能只看“SUE PASS”日志行，也不能只看进程没有崩溃。负例验证可能预期不可达，这种实验判据通过不表示网络任务本身成功。

## 5.6 验证脚本与本次证据

| 脚本 | 验证内容 |
|---|---|
| verify_resources.py | 同资源FIFO、计算/HBM独立、Die独立、本地环、MPU因果与共享队列 |
| verify_hardware.py | 非法硬件配置拒绝，XPU/MPU/D2D降速实际影响传输 |
| verify_moe_striping.py | 多Die实际发包、默认与leader控制、专家计算不变、batch1 |
| verify_parallel.py | TP1/4/8、EP16/32、副本边界、放置、容量、非法配置和复用概率 |
| verify.py | 物理拓扑、可达性、LLR注错、环资源及旧工作负载验证 |
| verify_inference_browser.py | 历史硬件报告离线交互与导出 |

本次`results/sue/parallel-config-validation/verification.json`记录5项逐包配置、9项非法输入拒绝；测试为了验证EP副本通信，对部分case显式扩大了每Die容量，**不意味着原36GB硬件能装下EP16布局**。标准容量下EP副本增多会被拒绝也有断言。

`results/sue/parallel-default-regression/`通过配置入口运行默认GLM条件，其任务时间戳与目录迁移后的既有基准逐字节一致。它验证默认行为未被配置重构改变，不验证所有真实模型性能准确。

## 5.7 如何做有意义的实验

1. 写明问题、固定项和变化项。例如固定芯片数、全局batch和上下文，只比较公共TP4/8/16/32，EP保持全局。
2. 先检查容量和实际组映射。不要比较一个有8个专家副本、另一个只有1套却不说明的实验。
3. 每个条件使用多个相同种子；保留失败和容量拒绝，均值只包含口径一致的完成条件。
4. 同时看完成窗口、每Die流量、端口忙碌、环与资源排队，避免仅凭一个平均时延解释瓶颈。
5. 区分应用载荷变化与协议/分片开销变化。TP增大时每片变小、元数据变多，端口哈希也可能改变。
6. 把结果限定在本模型假设内，再使用实测trace、kernel profile、真实链路延迟逐步校准。

多种子min–max不是统计置信区间。容量预算拒绝不是“速度为零”，缺失点不是“没有瓶颈”。旧报告的任何数值都应连同硬件快照、布局版本和计算模型一起阅读。


## 模型JSON验收

`results/sue/model-json-validation-v2/verification.json`：三个默认模型与一个改参GLM配置共4个逐包案例通过，12类非法配置被拒绝；另有真实CLI案例 `results/sue/model-json-cli/`。GLM默认JSON入口的任务时间戳与改造前基线逐字节一致。验证包含Top-K、专家数、精度对资源/流量的影响以及Engram头数对远程回传字节的影响。首次测试选择Kimi16芯片TP4被正常容量拒绝，记录保留在 `model-json-validation/`；随后按现有容量规则选择能容纳模型的TP，未调高硬件容量。
