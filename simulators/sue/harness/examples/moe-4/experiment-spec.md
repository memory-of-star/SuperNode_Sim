# sue-moe-demo

验证 dispatch 完成后执行固定计算等待，再进行 combine；网络协议为 SUE。

预测（运行前记录）：任务全部交付，combine 的开始时间晚于依赖的 dispatch 完成和计算等待。

协议：SUE AFH + 配置的 LLR / FC。计算：固定等待，无 GPU 资源排队。

事实来源为 experiment.json 和 traffic.csv；更改实验时同步更新这两个文件。
