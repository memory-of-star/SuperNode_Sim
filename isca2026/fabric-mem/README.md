# ISCA 2026 MPU 论文工程

暂定题目：**Beyond Capacity: Service-Aware Memory Pooling for Ethernet Scale-Up Inference**。

中文：**超越容量：面向以太网 Scale-Up 推理的服务能力感知内存池**。

当前交付是研究提案与可编译英文初稿；没有把待验证假设写成实验结论。中文论证、机制定义、实验优先级和停止条件见 [RESEARCH_STORY.md](RESEARCH_STORY.md)。

2026-11-03 文献调研更新：已建立 [ref 资料库](ref/README.md)，扩展为 46 项论文与产业资料。优先阅读 [Scale-Up 内存池专项核查及新候选机制](SCALEUP_MEMORY.md)：已有 ScalePool、UnifabriX、Photonic Fabric 和 Leo X 等直接相关工作，当前更值得检验的是远程读取与多消费者交付的融合。此前的 [综合分析](RESEARCH_SYNTHESIS.md) 保留读准入、模型缺口和实验基线。英文初稿仍为最初提案，尚未按新方向改写。

## 编译

从仓库根目录运行：

```bash
make -C isca2026/fabric-mem
```

输出 `isca2026/fabric-mem/paper.pdf`，中间产物与日志在 `isca2026/fabric-mem/build/`。也可从任意工作目录调用 `isca2026/fabric-mem/tools/build.sh` 的绝对路径。`make -C isca2026/fabric-mem clean` 仅清除此工程的 PDF 和 build 目录。

本工程独立安装 **Tectonic 0.17.0 / Linux x86_64 musl** 到 `.tools/`，安装包校验 SHA256；不需要 sudo，不依赖同级的 `transaction-aware/`。构建脚本指定 TeX bundle v33，首次编译联网按需获取 TeX 宏包、字体及 BibTeX 样式，缓存留在 `.cache/`；Tectonic 自动完成交叉引用和参考文献的多轮编译。网络受限时需先保留本工程已填充的缓存。本次初始化复制了已有 TeX 缓存来减少下载，复制后为独立文件，没有软链接依赖。工具来源：[Tectonic 官方 release](https://github.com/tectonic-typesetting/tectonic/releases/tag/tectonic%400.17.0)。发行包哈希是本次下载固定值，不宣称是上游签名认证。

已有完整 TeX Live 的环境也可在本目录执行：

```bash
latexmk -pdf -interaction=nonstopmode -halt-on-error -outdir=build main.tex
```

默认脚本使用 Tectonic；TeX Live 命令是替代入口，未在本机验证。

## 模板与文件

- `main.tex`、`sections/*.tex`：英文研究初稿，标注研究状态，不包含虚构性能数据。
- `refs.bib`：已核对作者列表的相关工作；当前使用官方模板中的 `IEEEtranS` 样式。
- `IEEEtran.cls`：从官方包原样取得的文档类。
- `vendor/official/`：官方模板原件，保留原始说明及样例；不直接编辑。
- `vendor/source-manifest.json`：下载时间、来源 URL、ZIP 与解包文件 SHA256。
- `tools/fetch_template.py`：首次下载模板；已有 manifest 时仅校验，不覆盖。
- `RESEARCH_STORY.md`：中文研究故事和证据计划。
- `VALIDATION.md`：本次实际构建与配置检查记录。

模板来自 [ISCA 2026 官方投稿指南](https://www.iscaconf.org/isca2026/submit/guidelines.php)链接的 [IEEE 模板 ZIP](https://www.iscaconf.org/isca2026/submit/ISCA2026_IEEE_template.zip)。沿用 conference 文档类、Letter 纸张、双栏、页码与匿名投稿横幅；编号 `NaN` 是待填项。正文上限 11 页，参考文献另计。目录名和模板按本次要求使用 2026 版；不表示会议仍开放投稿。未进行正式投稿合规认证。

新仿真实验输入仍统一维护在仓库 `configs/`；此目录只保存论文及研究文档。本次仅运行既有 `glm-single.json` 的配置/容量检查，没有运行逐包矩阵或重建历史实验。
