# SUE Scale-Up Ethernet 仿真平台 v2：完整使用与实现手册

本手册对应当前工作区 `simulators/sue/` 下的实现，包含模型HF JSON、硬件JSON、可配置TP/EP策略、多Die MoE收发和资源队列。内部可执行文件名、输入格式号仍保留 `v3` / `version: 3` 以兼容历史记录；对外产品名称统一是 **SUE v2**。

建议先阅读第1章和第7章，完成一次单条件仿真，再根据目的阅读其余章节。配置表中的“支持”指当前入口真正产生了对应行为，不代表底层源码中出现同名功能就已经接入。

## 手册目录

1. [系统定位、架构与能力范围](01-overview.md)
2. [硬件、并行、实验与网络配置](02-configuration.md)
3. [AI工作负载、资源队列与行为模型](03-behavior-model.md)
4. [SUE网络、拓扑、路由、LLR、CBFC与时序](04-network.md)
5. [输出文件、指标解释、验证与实验设计](05-results-validation.md)
6. [源码导航、扩展方式、故障排查与局限](06-implementation.md)
7. [使用教学](07-usage.md)
8. [完整案例：DeepSeek V4.1 Flash 1017，16卡与MPU部署](08-deepseek-16chip-case.md)
9. [MPU与PCIe卸载对比](09-mpu-pcie-offload.md)
10. [历史报告与迭代记录](10-report-catalog.md)

[单页离线HTML手册](manual.html)可全文搜索、浏览器打印或保存PDF。下载示例：[硬件规格](examples/current-hardware.json)、[TP4＋全局EP](examples/tp4-global.json)、[TP8＋全局EP](examples/tp8-global.json)、[TP8＋EP16](examples/tp8-ep16.json)。这些示例由工作区当前配置生成，不与配置源分开维护。

## 先记住的五件事

- 它是 **Python工作负载生成器＋C++ ns-3逐包网络仿真＋计算/内存资源队列**，不是在芯片上实际运行模型权重。
- 一个rank代表一个Die，当前每芯片4个Die。TP/DP用于公共部分，EP用于路由专家；不能把TP×EP直接当成设备总数。
- 默认MoE由所有活跃TP rank分片收发；链路是否满载由真实任务、消息规模、端口哈希和竞争决定。
- 完整模型容量预算、解析资源下界、代表阶段逐包实测是三种证据，不能互相替代。单块时延不是TPOT，参数chunk时延不是TTFT。
- 原始输入、命令、二进制SHA、任务统计和失败记录比截图更重要。历史实验不因代码升级自动变成新实现的结果。

## 可直接复制的入口

以下命令从 `/home/dell/scale_up_evaluation` 执行，输出目录需要是新目录：

```bash
bin/sue check --parallel configs/parallel/tp4-global.json
bin/sue build
bin/sue run --hardware hardware/profiles/current.json \
  --parallel configs/parallel/tp4-global.json \
  --output results/sue/manual-first-run \
  --model glm-5.3 --chips 8 --batch 8 --context 8192
```

本次并行配置实现的验收记录在 `results/sue/parallel-config-validation/verification.json`：5项逐包布局测试、9种非法配置拒绝、默认布局时间戳回归。单条件默认JSON入口的实际运行保存在 `results/sue/parallel-default-regression/`。

模型输入也可配置：`bin/sue check --model-config configs/models/glm-5.3/config.json`。原始 Hugging Face 结构和独立仿真假设见第2章；每次运行保存两份JSON及来源哈希。

模型示例下载：[GLM HF配置](examples/glm-5.3-config.json)及[补充假设](examples/glm-5.3-simulation.json)。离线包将二者分别保存，运行时显式传入 `--model-config` 和 `--model-simulation`。

第8.6节新增12张通信与拥塞图，包含卡/Die矩阵、端口/轨道时序、出口队列、CBFC信用、时延、专家负载、UCIe和资源观测：[交互看板](communication/report.html)。
