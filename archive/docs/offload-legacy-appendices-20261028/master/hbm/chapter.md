## 9.10 HBM公平共享：DeepSeek KV＋Engram重跑对照

本节使用新的HBM共享模型。§9.3–9.6保留上一轮HBM FIFO实验，不应将两轮PCIe基线混用。本轮只重跑DeepSeek KV＋Engram，未声称其他模型也已更新。

### 共享机制与配置

每Die有效HBM带宽仍为1.875 TB/s。活跃访问源先均分；达到自身速率上限的源只取所需份额，其余带宽继续分给未达上限的源。请求到达和完成时更新剩余字节与速率，不等整项专家读取结束。没有需求时不分配带宽，所有源都受限时允许带宽闲置。

源按业务类别划分：专家读取、公共层读取、KV消费、Engram消费、SUE写入、PCIe写入及其他。同类源内FIFO，各源平权；拆分更多请求不会获得更多权重。KV与Engram的SUE写入共享200 GB/s/Die上限；PCIe写入上限128 GB/s/Die，真实可用速率还受四Die共享的全芯片PCIe、DDR及到达时刻限制，不能将四个上限相加当卡带宽。接口限速源用不满的HBM份额会被专家读取等其他源使用。

配置见[HBM JSON](examples/hbm-fair-share.json)，带宽不能超过硬件有效预算。未传入该JSON的历史入口仍采用FIFO。

PCIe保持有限接收缓冲：收到Completion后才提交写入，实际HBM写完后才释放缓冲，慢HBM会反向限制PCIe。SUE仍保留整消息收齐和确认后再写HBM的模型，写入速率受接口上限约束；没有新增逐SUE包到HBM的流式信用耦合。这次改动也没有复刻真实DRAM bank、读写切换、缓存或GPU仲裁优先级。

### 实验口径

42组：FIFO与公平共享分别运行纯PCIe基线、1/2/4/8/16 MPU nominal share、8 MPU固定25%分流，全部seed1/2/3。固定16卡、TP4×DP16、EP64、B64、初始上下文32768、两步代表块、原256 KiB分块、同一事务级PCIe。25%来自先前实验，仅作为固定比例控制，不重新选择，不标为新的confirmed share。结果为µs/代表块，不是完整模型TPOT。

两种HBM模式的任务、资源输入、专家路由、访问字节及PCIe地址均逐字节配对；21个FIFO条件复现历史统计。旧新差异同时包含带宽共享与写入速率上限，不能只归因于某一项。

![HBM公平共享与FIFO对照](hbm/hbm-comparison.svg)

左图直接比较耗时，阴影为三个种子范围。右图分别用各自HBM模式的纯PCIe均值作分子，避免用旧基线夸大新模式收益。

| HBM模式 | 纯PCIe基线µs |
| --- | --- |
| fifo | 178.537 |
| fair | 155.921 |

| MPU | FIFO µs | 公平共享µs | 耗时变化% | FIFO速度比 | 共享速度比 |
| --- | --- | --- | --- | --- | --- |
| 1 | 173.369 | 160.961 | -7.157 | 1.030 | 0.969 |
| 2 | 180.708 | 169.551 | -6.174 | 0.988 | 0.920 |
| 4 | 189.217 | 170.570 | -9.855 | 0.944 | 0.914 |
| 8 | 166.116 | 155.350 | -6.481 | 1.075 | 1.004 |
| 16 | 162.067 | 144.970 | -10.549 | 1.102 | 1.076 |

**结果解读：**4 MPU的均值从189.217降到170.570 µs（变化-9.85%）。本轮共享模型的nominal share最慢点仍是4 MPU，因此取消整项专家读取的独占阻塞并没有保证曲线单调。不能再将剩余拐点全部归因于旧HBM FIFO，仍需用固定分流/时序消融区分网络返回与DAG调度的贡献。纯PCIe也从178.537降到155.921 µs，绝对耗时下降不等于相对PCIe收益增加。

### 8 MPU固定25%分流控制

| 模式 | µs/代表块 | 相对本模式PCIe速度比 |
| --- | --- | --- |
| fifo | 159.644 | 1.118 |
| fair | 139.777 | 1.116 |

### 原始排队反例的复查

以下是4 MPU、seed2、第一步、XPU13 Die0原先受阻的256 KiB KV写入。就绪到首次服务是源内FIFO等待；首次服务到完成包含共享导致的服务拉长，二者不能混淆。

| 模式 | 就绪µs | 首次服务µs | 完成µs | 源内等待µs |
| --- | --- | --- | --- | --- |
| fifo | 113.419 | 136.199 | 136.339 | 22.781 |
| fair | 113.419 | 113.419 | 114.729 | 0.000 |

该请求源内等待从22.781 µs变为0.000 µs；整个任务图的变化仍应以上面的配对均值为准，单个请求不能解释全部性能。

### 验证、统计与复现

机制测试覆盖均分、限速水位分配、源加入/退出、小请求不增加权重、Die独立、旧FIFO精确回归、PCIe接收缓冲填满与专家减速。全量实验核验字节守恒、DAG依赖、源速率上限、Die总带宽积分和PCIe信用/缓冲限制。

共享模式下`resource_statistics.csv`的输入`service_ns`是历史独占预算，不再等于实际服务历时；使用真实`end_ps`。`queued_ps`只统计同类源内部排队。`hbm_sources.csv`的各源活跃时间相互重叠，不能相加当HBM利用率；字节积分和`hbm_dies.csv`总量用于核验带宽守恒。PCIe的HBM busy同样表示经历时间，不是独占时间。

[配对汇总CSV](hbm/comparison.csv) · [全部统计](hbm/summary.csv) · [每例结果](hbm/results.json) · [验证记录](hbm/verification.json) · [运行计划](hbm/plan.json) · [机制验证](hbm/mechanism-verification.json)

```bash
bin/sue build
python3 simulators/sue/harness/v2/verify_hbm.py results/sue/FRESH-hbm-tests
python3 simulators/sue/harness/v2/run_hbm_study.py results/sue/FRESH-hbm-study
PYTHONPATH=/tmp/sue-communication-deps:/tmp/sue-report-deps python3 simulators/sue/harness/v2/hbm_report.py results/sue/FRESH-hbm-study
```

独立运行offload时添加`--hbm-profile hardware/hbm/fair-share.json`；省略则使用旧FIFO。绘图需要matplotlib，HTML需要markdown-it-py；这些依赖不参与仿真。
