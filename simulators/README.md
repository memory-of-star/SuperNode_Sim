# 当前仿真器

`sue/` 是当前使用的 SUE Scale-Up Ethernet 仿真平台 v2，保留完整源码、协议实现、构建目录和harness。项目内部文件位置保持不变，仅将报告与早期日志通过软链接指向工作区的结果/归档目录。

请从工作区根目录使用 `bin/sue build`、`bin/sue check`、`bin/sue run ...`。

旧的ns-3-ub和原生ns-3.47已移入 `archive/simulators/`，不作为当前默认实验入口。
