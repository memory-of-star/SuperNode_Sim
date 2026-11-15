# ISCA 论文工作目录

- [transaction-aware/](transaction-aware/README.md)：**Beyond Bandwidth: Transaction-Aware Design of Ethernet Scale-Up Fabrics for AI Inference**，包含正文、PDF、冻结证据、模板和编译工具。
- [fabric-mem/](fabric-mem/README.md)：Scale-Up 独立内存池与推理数据访问研究，包含 MPU 论文初稿、相关文献和编译环境。
- [相关文献库](transaction-aware/ref/README.md)：本地论文、阅读笔记及相关仿真器源码，已加入 Git 忽略。

在仓库根目录编译与检查：

```bash
./isca2026/transaction-aware/tools/build.sh
python3 isca2026/transaction-aware/tools/check_paper.py
```
