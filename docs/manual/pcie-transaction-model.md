## 9.9 PCIe事务级模型：双路Host、有限队列与背压

§9.1–9.8将本PCIe模型与HBM公平共享联用，登记330项矩阵；完成数、精度与MPU槽位以该章冻结输入及校验记录为准。通过独立JSON配置。它是ns-3时间轴上的事务级离散事件模型，不是PCIe PHY或认证级协议栈。

[机制测试记录](pcie/mechanism-verification.json) · [配置JSON](examples/pcie-dual-socket-ddr5-6400.json)

### Host布局和默认参数

每Host8张XPU、双路CPU、总DDR容量1TB（十进制），每路接4张XPU。全局XPU0–3接CPU0，4–7接CPU1，8–11接下一个Host的CPU0，依此类推。每路12个DDR5-6400通道，理论带宽614.4GB/s；默认效率0.75，得到每路460.8GB/s、双路均衡访问时合计921.6GB/s。没有将DDR读/写再翻倍。

| 资源 | 默认值 | 选取与作用 |
|---|---|---|
| DDR容量 | 500GB/NUMA，1TB/Host | 用户指定总容量，双路均分 |
| DDR有效带宽 | 460.8GB/s/NUMA | 12×6400MT/s×8B×0.75；效率为工程假设 |
| DDR读固定延迟/并发 | 100ns / 每NUMA最多128项 | 延迟阶段可以重叠；之后按12通道分别序列化 |
| 内存访问粒度 | 64B | 按地址覆盖的cache line计DDR字节，再按line号取模分到通道 |
| DMA引擎/描述符容量 | 每芯片2个/64项 | 描述符容量包括已接纳、等待引擎和正在执行者；软件上游可以等待 |
| 提交间隔/引擎启动 | 100ns / 200ns | 提交按芯片串行，引擎启动占用引擎 |
| tag/在途字节 | 每芯片256 / 256KiB | 四个Die共同竞争，不按Die复制额度 |
| 描述符读请求前瞻 | 最多128项 | 已就绪描述符内部的有限read lookahead，不是跨decode步预测预取 |
| 端点/每NUMA共享上行 | 单向128 / 512GB/s | 默认四卡共享一个PCIe Switch上行，无额外4:1超售；上下行独立 |
| 每链路固定延迟 | 50ns | 端点、上行分别计入；读取往返还有DDR和排队 |
| MRRS / MPS / RCB | 512B / 256B / 128B | 读请求不跨4KiB，Completion首段对齐RCB，后续不超过MPS |
| 请求头额度 | 每方向链路64个 | 请求进入下游后才允许返还额度 |
| Completion额度 | 256个头＋64KiB数据 | 数据额度按16B向上取整，不把包头计入数据额度 |
| 额度返还延迟 | 40ns | 下游接纳后延迟返还，形成有限缓存的背压 |
| Completion线上开销 | 每片24B | 固定的序列化开销近似，不对应特定代际FLIT/FEC；读请求为32B |
| 接收缓冲 | 每芯片64KiB | 四Die共享，完成对应HBM写入才释放 |
| HBM写入 | 每Die1.875TB/s | 与当前硬件15TB/s÷4×0.5一致；第9章主实验已启用按源公平共享 |
| NUMA互连 | 单向128GB/s、100ns | 默认本地访问；remote_numa=true时经过CPU间链路，是未校准假设 |
| Host打包吞吐 | 每NUMA64GB/s | 仅host_pack模式启用；另计DDR读写，不代替DDR开销 |
| 页粒度/页缓存 | 4KiB / 每Die1MiB | 仅page模式使用；容量设0可关闭缓存 |

除用户明确指定的Host构成、DDR规格和原有XPU规格外，上述时延、效率、并发、缓冲、tag和包开销均为**可修改且未经实测校准的默认假设**。链路bytes/s是模型中的字节序列化预算，之后再计配置的包开销，不能同时拿实测有效载荷带宽来重复扣同一开销。

### 请求在仿真器里如何推进

```text
依赖就绪 → 上游等待 → 接纳描述符 → 提交 → 获得DMA引擎
  → 引擎启动 → 获取tag/在途额度 → MRRS与4KiB边界拆分读请求
  → 端点请求链路 → Switch共享上行 → [可选NUMA互连]
  → DDR并发槽位 → 读取延迟 → 通道带宽服务
  → Completion按MPS/RCB拆分
  → [可选NUMA互连] → 共享下行 → 端点返回链路
  → 接收缓冲 → HBM调度器写入（本章主实验为按源共享） → 描述符完成 → 消费者
```

同一时刻各段可处理不同请求。tag在该读事务的全部Completion进入端点接收缓冲后释放，而接收缓冲在对应HBM写入后释放。消费者默认等待其描述符全部数据落入HBM；不会假装所有算子都可消费半份输入。HBM拥塞时，接收缓冲逐渐占满、链路额度无法返还、在途额度和tag耗尽，后续请求停止发起。

链路采用FIFO序列化，DMA活跃描述符按轮转尝试发起请求；没有实现PCIe全部TC/VC仲裁。各链路接收空间有限，无法接纳的数据仍由上游生产者持有；软件待提交描述符队列不当作已接纳的硬件描述符槽位。DDR本身也有有限并发槽位，固定延迟可重叠，不是每个小读将整个DDR阻塞100ns。

### 四种访问模式

`access.kv`与`access.engram`可独立选择`sequential`、`gather`、`page`、`host_pack`。默认KV用sequential、Engram用gather。

| 模式 | 实际行为 |
|---|---|
| sequential | 连续地址按MRRS和4KiB边界拆分，Completion流水返回 |
| gather | 按行地址发起请求；默认Engram每行264B、行距4096B，DDR按实际覆盖cache line计费 |
| page | 获取整页，统计传输放大；每Die独立LRU，页面写完HBM才加入缓存；同描述符重复页面合并 |
| host_pack | 先随机读取各行，再执行Host打包服务、写入连续缓冲，最后DMA重新读取缓冲；明确计三类DDR访问 |

地址来自稳定的合成映射，包含chip/Die/请求标签；相同步骤结构跨decode步复用地址，因此页模式刻意体现稳定工作集的热缓存场景。这不是模型的真实查表地址或热度trace。跨描述符同时未命中的同一页不合并，可能重复获取；没有KV脏页写回、页故障OS开销、IOMMU、Host CPU cache或投机跨步预取。Host打包先准备整份描述符，随后PCIe按分片流水。

容量准入也随之补充：KV按XPU分片到NUMA，Host侧Engram分区在每个NUMA保留完整副本以支持本地查表；每NUMA不能超过500GB。报告同时给出唯一数据集字节与实际Host复制占用，不再把它们混为一谈。remote_numa模式交换两路映射并加入互连；不支持跨Host直接寻址。

### 如何配置和运行

默认配置位于工作区`configs/hardware/pcie/dual-socket-ddr5-6400.json`。复制后修改即可运行其他参数；未知字段、非法MRRS/MPS、无法装下Completion的缓冲、零tag等会在运行前被拒绝。

```bash
# 构建新增事务模型
bin/sue build

# 机制测试；需新输出目录
python3 simulators/sue/harness/v2/verify_pcie.py \
  results/sue/pcie-mechanism-repeat

# 默认JSON下的12项联动矩阵，含新旧PCIe、纯MPU控制、混合、三模型
python3 simulators/sue/harness/v2/run_pcie_cases.py \
  results/sue/pcie-integration-repeat \
  --pcie-profile configs/hardware/pcie/dual-socket-ddr5-6400.json --workers 2

# 分析工具依赖markdown-it-py；不参与仿真
python3 simulators/sue/harness/v2/pcie_report.py \
  results/sue/pcie-integration-repeat

# 在原offload矩阵入口启用新模型；--smoke只运行3个DeepSeek代表案例
python3 simulators/sue/harness/v2/offload_experiments.py \
  results/sue/pcie-smoke-repeat --smoke \
  --pcie-profile configs/hardware/pcie/dual-socket-ddr5-6400.json
```

`offload_report.py`核验kind=5事务模型及共享HBM的带宽积分，用于完整扫描报告。新模型没有修改SUE的AFH/LLR/CBFC。

### 输出与证据

每次run保存`pcie.json`和编译后的`pcie.tsv`，以及逐描述符`pcie-requests.json/tsv`；运行前核对二者一致，command manifest保存输入、二进制和C++头文件哈希。

- `pcie_descriptors.csv`：有效/传输/DDR/HBM字节，读请求和Completion个数，入队/接纳/引擎开始/完成时间，tag及在途受限检查次数，页命中数。
- `pcie_resources.csv`：各端点和上行的线上字节、数据额度占用峰值、头额度峰值、累计准入/序列化等待，DDR通道累计忙碌时间、接收缓冲占用和HBM写入服务。
- `pcie_chips.csv`：每芯片的tag峰值、在途字节峰值、已接纳描述符峰值和结束剩余数量。
- `resource_statistics.csv`中kind=5不使用固定service_ns计时，实际过程见上述PCIe表；HBM staging已在kind=5内部完成，不能额外再加一遍整块HBM写入。

所有累计等待是多个请求的时间之和，不能直接与端到端时间相加。DDR busy_ps是各通道服务时间之和，算平均忙碌率需除以通道数与观测窗口。资源表中bytes表示该资源累计计费/占用额度字节，链路真实序列化字节另列wire_bytes。结束时少量链路额度返还事件可能尚未触发，但所有有效数据必须落入HBM、全部tag与描述符必须释放；这不属于丢包。

### 本次机制与联动验证

机制验证覆盖tag/在途窗口、描述符接纳、共享和独立NUMA上行、DDR争用、跨NUMA、直连、HBM背压、随机gather与Host打包、页放大与缓存命中、4KiB边界、MPS拆分、单Completion额度。

完整实验结果见§9.1–9.8。
