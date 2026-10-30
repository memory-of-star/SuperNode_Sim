# PCIe事务级模型：实现与联动验证

此报告采用双路、每路12通道DDR5-6400、每Host8卡/1TB。DDR效率0.75，理论/有效带宽每路614.4/460.8GB/s；其余队列、延迟、tag等均为可修改的工程假设。详细机制见手册§9.9。

DeepSeek为B64/32K/两步完整代表块；Kimi/GLM为B8/2K/一步小规模联动验证，不能把后两者与旧第9章B64/32K结果比较。表中时间均为窗口除以本次步数，不是全模型TPOT。旧FIFO保留1µs启动，事务模型使用100ns描述符提交与200ns引擎启动，延迟按各环节事件产生，两者不叠加。

| 模型 | 方案 | 模型版本 | MPU | seed | B/context | µs/代表块 |
|---|---|---|---:|---:|---|---:|
| deepseek-v4.1-flash-1017 | pcie | legacy | 0 | 1 | 64/32768 | 130.844 |
| deepseek-v4.1-flash-1017 | pcie | transaction | 0 | 1 | 64/32768 | 178.440 |
| deepseek-v4.1-flash-1017 | hybrid | legacy | 8 | 1 | 64/32768 | 185.151 |
| deepseek-v4.1-flash-1017 | hybrid | transaction | 8 | 1 | 64/32768 | 178.514 |
| deepseek-v4.1-flash-1017 | mpu | legacy | 8 | 1 | 64/32768 | 245.453 |
| deepseek-v4.1-flash-1017 | mpu | transaction | 8 | 1 | 64/32768 | 245.453 |
| deepseek-v4.1-flash-1017 | pcie | transaction | 0 | 2 | 64/32768 | 178.659 |
| deepseek-v4.1-flash-1017 | pcie | transaction | 0 | 3 | 64/32768 | 178.513 |
| kimi-k3 | pcie | small-integration | 0 | 1 | 8/2048 | 142.826 |
| kimi-k3 | hybrid | small-integration | 8 | 1 | 8/2048 | 141.980 |
| glm-5.3 | pcie | small-integration | 0 | 1 | 8/2048 | 117.499 |
| glm-5.3 | hybrid | small-integration | 8 | 1 | 8/2048 | 105.512 |

## 如何解读

- CSV中旧FIFO未建模/未记录的事务指标留空，不表示零开销。
- 旧FIFO与事务模型的差异同时涉及队列、内存、启动、分片和背压，不能将差值全部归因为某一机制。
- MPU-only控制组的任务、端口和资源统计逐字节一致；PCIe模型不会凭空改变SUE协议。
- 累加排队时间是多个请求的总等待量，允许重叠，不能与端到端窗口直接相加。
- 本次并未重跑历史210项全矩阵；这些是集成验证与选定案例对照。
- 原始run目录中pcie_descriptors.csv记录每描述符字节与时序；pcie_resources.csv记录链路/DDR/接收缓冲；pcie_chips.csv记录队列、tag与在途峰值。
- 信用按头部个数和16B数据额度抽象；没有配置空间、序列号、ACK/NAK重放、FLIT/FEC或电气PHY。DDR为通道带宽与受限并发/固定延迟模型，不包含DRAM逐命令时序。
