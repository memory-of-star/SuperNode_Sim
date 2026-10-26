# SUE Scale-Up Ethernet 仿真平台 v2

沿用原项目 `DESIGN.md` 中的名称 **SUE Scale-Up Ethernet 仿真平台**，当前本地扩展版本标为 **v2**，不是上游发布版本号。v2增加16种多Die/MPU拓扑、端口组选路、目的侧UCIe环及原口响应；通用任务入口和历史报告保留。

- [v2 拓扑与 Kimi K3 / GLM 5.3 通信报告](reports/v2-topology-kimi-glm53/report.html)
- [v2 使用说明、输入与模型边界](harness/v2/README.md)
- [v2 新功能与真实场景对齐报告](reports/20260914-feature-alignment/report.html)
- [v2 案例与对比结果](harness/index.html)
- [使用说明和命令](harness/README.md)
- [原设计基准](DESIGN.md)

## 协议与模型范围

| 层次 | 范围 |
| --- | --- |
| 原 SUE 模块 | AFH Gen1 Lite、LLR、CBFC/PFC、交换机转发与背压 |
| 原 ns-3-alibabacloud 底座 | RoCEv2/RDMA、QP、ACK/NACK、PFC/ECN/CNP、DCQCN/HPCC/TIMELY/DCTCP/HPCC-PINT 等拥塞控制模式，以及 NVSwitch/NVLS 模型 |
| v2 拓扑入口 | 16种拓扑、Die/MPU、组选路、目的侧环及原口确认；单线程SUE LLR＋CBFC |
| v2 通用任务入口 | 通用任务 DAG 接入 SUE；流控可选 CBFC/PFC/OFF。尚未接通 RoCE 后端，也不模拟 UB 协议 |

原项目不只包含 SUE 能力。依据为 [DESIGN.md §1.3](DESIGN.md) 和 [底座 README](ns-3-alibabacloud/README.md)；这些底座能力在本次工作中没有逐项重新验证。NVSwitch 模型也不等于完整 NVLink 物理层仿真。

`SueEntryDispatch` 在 `ENABLE_SCALEUP=0` 时需要调用方提供 `externalSetup`，才会进入其他建网路径。当前 harness 传入空回调，所以不能仅修改这个开关就切换 RoCE。混合 SUE/RoCE 拓扑尚无本次验证案例。

## v2 通用任务能力

新增四阶段实验 skills、消息分片和收齐判定、任务依赖 DAG、固定计算等待、有限端点队列准入、UB 逻辑流量 CSV 导入、参数扫描、失败台账和离线报告。原 SUE 核心协议实现及设计基准保持不变。计算仍为固定等待，未模拟 GPU/HBM 排队。
