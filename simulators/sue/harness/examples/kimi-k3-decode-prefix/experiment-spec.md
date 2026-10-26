# kimi-k3-decode-prefix-on-sue

在 SUE 网络上回放 UB CSV 的逻辑通信字节和 phase 依赖；不保留 UB 事务语义。

预测（运行前记录）：任务全部交付，combine 的开始时间晚于依赖的 dispatch 完成和计算等待。

协议：SUE AFH + 配置的 LLR / FC。计算：固定等待，无 GPU 资源排队。

事实来源为 experiment.json 和 traffic.csv；更改实验时同步更新这两个文件。
