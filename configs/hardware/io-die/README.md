# IO Die AXI与拼包配置

完整机制、字段长度、统计口径及验证记录统一维护在[IO Die模型文档](../../../docs/manual/io-die-model.md)，此处不另存一份副本。

| 配置 | 行为 |
|---|---|
| [best-effort.json](best-effort.json) | 同端口、同目的SA、同VC、同类别的已就绪记录尽力拼包；不等待凑包 |
| [no-packing.json](no-packing.json) | 保留相同AXI事务与内存模型，每条记录独立成帧 |

两者均为256 B事务、1 ns产生间隔、逐AXI MPU服务。AW与W始终成对；AR、B、R分别同类拼包。MPU在途上限、200 ns访问延迟和内存带宽来自所选硬件配置；当前`configs/hardware/profiles/current.json`显式配置128槽位。

```bash
bin/sue run --hardware configs/hardware/profiles/current.json \
  --io-die-profile configs/hardware/io-die/best-effort.json \
  --hbm-profile configs/hardware/hbm/fair-share.json \
  --model deepseek-v4.1-flash-1017 --chips 16 --tp 4 \
  --batch 1 --context 128 --mpus 2 --engram-remote --steps 1 \
  --output results/sue/deepseek-io-packed
```

从工作区根运行，使用新输出目录。未指定IO配置时使用兼容任务模式，与“AXI不拼包”不同。新案例默认1 ps全局精度；请求间隔和MPU服务窗口仍是1 ns，历史输入按自己的冻结精度重放。
