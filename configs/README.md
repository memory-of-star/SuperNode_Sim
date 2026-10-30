# 仿真输入配置

统一入口为`bin/sue check/run --config 实验.json`。硬件、模型和并行JSON仍是可复用组件；运行时只需选择一份实验定义，它把所有组件和工作负载组合起来。组件既可通过`file`引用，也可直接内联。

```bash
bin/sue check --config configs/experiments/glm-single.json
bin/sue run --config configs/experiments/glm-single.json --output results/sue/glm-unified
```

| 目录 | 内容 | 统一JSON中的字段 |
|---|---|---|
| [hardware/profiles/](hardware/profiles/current.json) | XPU/MPU规格、资源效率、接入延迟 | hardware |
| [hardware/hbm/](hardware/hbm/README.md) | HBM共享策略和访问源限速 | profiles.hbm |
| [hardware/io-die/](hardware/io-die/README.md) | AXI事务、拼包、队列 | profiles.io_die |
| [hardware/pcie/](hardware/pcie/README.md) | Host DDR、DMA和PCIe事务 | profiles.pcie，配合offload |
| [models/](models/README.md) | HF模型结构及仿真假设 | model.config、model.simulation |
| parallel/ | TP/DP/EP及放置策略 | parallel |
| [experiments/](experiments/README.md) | 完整输入、工作负载、种子和对照组 | 一份文件作为唯一入口 |

完整格式、参数覆盖规则、PCIe/MPU卸载和输出说明见[统一实验JSON说明](experiments/README.md)。旧的`--hardware`、`--model-config`和`--parallel`命令保留兼容，但不能与`--config`混用。

运行前校验全部条件并冻结有效输入，历史实验不会随源JSON修改而变化。原根目录`hardware/`仅为指向`configs/hardware/`的兼容软链接，不含重复配置。硬件字段见[硬件说明](hardware/README.md)，模型实现边界见[手册](../docs/manual/README.md)。
