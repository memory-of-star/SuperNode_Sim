# HBM：带上限的公平共享

`fair-share.json`启用每Die独立的capped max-min模型。先在活跃访问源间均分；达到自身带宽上限的源固定在上限，其余预算继续均分给未达上限的源。新请求、完成事件到达时按皮秒时间更新剩余字节并重新分配，没有粗粒度轮询。所有源都受限时，允许剩余HBM带宽闲置。

当前每Die有效HBM带宽1.875 TB/s，沿用原硬件的15 TB/s ÷ 4 Die × 0.5效率；不是提高HBM规格。`cap_bytes_s=0`表示仅受总HBM预算限制。每Die的SUE写入合并为一个源，上限200 GB/s；PCIe写入合并为一个源，上限128 GB/s，PCIe模型仍负责全芯片四Die共享128 GB/s链路及NUMA上行，不能把每Die上限相加当作卡带宽。配置上限可调低，offload入口拒绝超过硬件接口预算的值。

源按业务类别聚合：专家权重读取、公共层读取、KV消费、Engram消费、SUE写入、PCIe写入和其他。每类源内部FIFO，源之间平权；不会因为某个DMA拆成更多Completion就增加权重。KV与Engram共用同一个接口写入源，防止两者各占一份接口上限。

从工作区根目录运行：

```bash
bin/sue build
python3 simulators/sue/harness/v2/offload_experiments.py results/sue/fair-hbm-smoke \
  --smoke --workers 3 --timeout-s 600 \
  --pcie-profile configs/hardware/pcie/dual-socket-ddr5-6400.json \
  --hbm-profile configs/hardware/hbm/fair-share.json
```

省略`--hbm-profile`保留旧FIFO，历史实验不会自动改变。HBM JSON里的带宽必须匹配硬件有效HBM及PCIe接收模型；修改硬件规格时应同步修改配置。适配器的sidecar入口也支持其他工作负载使用同一共享调度器。

## 时序与边界

- 所有已就绪HBM请求按真实ns-3事件共享带宽，DAG消费者依然等待所需字节服务完毕。
- PCIe只在Completion抵达并取得接收缓冲后提交HBM写入，实际HBM完成才释放缓冲；HBM拥塞会向PCIe反馈背压。
- SUE采用“完整消息收齐后写HBM、确认并行返回”的边界，写入被接口速率上限约束。它不是逐SUE包直接流入HBM，也未新增HBM到SUE信用的接收侧耦合；该部分仍是消息级近似，不能声称实现了端到端SUE/HBM流式背压。
- `resource_statistics.csv`的HBM `service_ns`仍是输入的原始独占预算，实际共享完成时间必须读取`end_ps`，不可再用固定时长核验。`queued_ps`只表示同类源内部的FIFO等待，不含带宽被其他源分走造成的延长。
- `hbm_sources.csv`记录来源字节、完成/待处理请求、源FIFO等待、活跃时间、带宽积分；`hbm_dies.csv`记录Die总量。逐源活跃时间可以重叠，不能加起来当HBM利用率；PCIe的`hbm-write` busy统计也是服务经历时间，不代表独占带宽。
- 无DRAM bank/row、读写切换、缓存、硬件优先级、SM占用或真实QoS标定。max-min是可解释的行为模型，不是具体GPU仲裁器复刻。

机制测试：`python3 simulators/sue/harness/v2/verify_hbm.py results/sue/fresh-hbm-validation`。

配对DeepSeek实验：`python3 simulators/sue/harness/v2/run_hbm_study.py results/sue/fresh-hbm-study`，随后用`hbm_report.py`分析。`offload_report.py`现可按输入配置选择FIFO或共享HBM审计；共享模式核验服务字节积分及带宽上限，不要求HBM任务固定时长或彼此不重叠。

完整第9章实验（五种工作负载、全部MPU数量与独立种子确认）：

```bash
python3 simulators/sue/harness/v2/run_offload_fair_study.py results/sue/fresh-offload-fair --workers 6
python3 simulators/sue/harness/v2/offload_report.py results/sue/fresh-offload-fair/analysis
python3 simulators/sue/harness/v2/verify_offload_report.py results/sue/fresh-offload-fair/analysis
```

入口默认加载本目录`fair-share.json`及`configs/hardware/pcie/dual-socket-ddr5-6400.json`，冻结配置后运行330个条件。各工作负载与MPU数独立用seed1在四档固定比例和nominal share中选取，再冻结比例并运行seed2/3；不能用确认种子的结果反过来选比例。
