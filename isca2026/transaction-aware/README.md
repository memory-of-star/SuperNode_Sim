# Beyond Bandwidth 论文工作区

题目：**Beyond Bandwidth: Transaction-Aware Design of Ethernet Scale-Up Fabrics for AI Inference**。

这是使用 ISCA 2026 官方投稿 LaTeX 模板的**英文初稿：正文11页＋参考文献1页，PDF共12页**，不是已投稿/录用论文。ISCA 2026 会期已结束；目录名和格式遵照本次要求保留。后续选择实际投稿会议时需重新核对截止日期与规则。

## 阅读与编译

- [编译后的论文](paper.pdf)
- [LaTeX 主文件](main.tex)，详细章节位于 `sections/`
- [参考文献](references.bib)
- [研究缺口与补实验计划](RESEARCH_PLAN.md)
- [模板与工具链来源](vendor/manifest.json)

从仓库根执行：

```bash
./isca2026/transaction-aware/tools/build.sh
python3 isca2026/transaction-aware/tools/check_paper.py
```

或 `make -C isca2026`。`paper.pdf` 是最终阅读文件，`build/` 保存可删除的中间产物和日志。编译使用本目录 `.tools/tectonic` 0.17.0，无须 sudo、不修改系统 TeX。首次自动下载引擎及 TeX bundle 中的宏包/字体，后续复用 `.cache/`。setup 脚本固定 Linux x86_64 musl 发行包并校验 SHA-256。其他系统需要替换对应发行包。需要 curl、tar、sha256sum；验收需要 Python 3、pdfinfo、pdftotext、pdffonts（Poppler）。

普通编译不需要原始仿真结果，也不启动仿真。`make -C isca2026 evidence` 才会从本工作区原始实验刷新摘要；新克隆没有这些被忽略的数据时，不要执行这个目标。

## 初稿的证据边界

1. 已实现：DAG、IO Die AXI式记录、尽力拼包、SUE协议、HBM共享、MPU事务流水、事务级PCIe与事件唤醒。
2. 实测（仿真）：第8章三种子拼包对照；PCIe 1 ns/1 ps × 轮询/通知四组消融。
3. 解析推导：128槽位、256 B、200 ns给出163.84 GB/s乐观并发上限；不是实测吞吐。
4. 待实现：独立交换机包处理率、完整MPU结果缓冲背压、联合拼包/信用策略。
5. 待完成：硬件或RTL校准、真实负载轨迹、策略收益与完整模型性能验证。不能将初稿现有数字写成这些内容的结果。

`evidence/` 是小型冻结摘要，携带原始文件SHA和来源路径；不含大型原始轨迹。正文图由这些摘要绘制，架构图为本论文独立绘制的TikZ示意图，没有使用未公开论文截图。文件中的精度实验按两步窗口除以2给出代表块平均量，不能解释成全模型TPOT。

## 格式

官方来源：<https://www.iscaconf.org/isca2026/submit/guidelines.php>。
模板ZIP：<https://www.iscaconf.org/isca2026/submit/ISCA2026_IEEE_template.zip>。

`vendor/official/` 保存下载模板原件，根部 `IEEEtran.cls` 与原件逐字节一致。Tectonic使用XeTeX引擎，主文件显式选择T1编码和Times兼容字体，避免IEEE类回退到Latin Modern；PDF字体均嵌入。正文使用双栏、Letter纸和页码；未压缩边距或缩小正文以塞入内容。官方网页在2025-11-11更新过格式说明，早期sample PDF存在不同尺寸描述，保留原模板并以最新官方网页为准。官方上限为正文11页，参考文献不限。当前横幅明确是工作草稿，不伪造投稿编号；真的投稿时需要改成对应会议、编号、保密横幅，并核对匿名和元数据。

按官方AI使用要求，初稿末尾包含AI辅助起草披露。作者信息尚未提供，暂不添加。正文尚不是可直接提交的完整研究结果。

## Git与第三方材料

`ref/`（源码位于 `ref/projects/`） 完整加入gitignore，保存三项相关工作的PDF、文本、源码和版本清单。源码没有编译运行，下载不代表复现了其论文结果。CXL-DMSim目前合入官方SimCXL；ASTRA默认分支也可能晚于2.0论文，具体SHA见资料清单，不能宣称是论文当年的精确artifact。

本论文源码、参考文献、冻结小型证据和PDF可以入库；工具链、缓存、中间产物与模板ZIP忽略。此次没有自动commit、push或投稿。

## 11页扩展版

相对首版，补充事务状态与资源所有权、精确拼包选择流程、待实现的有限响应缓冲、事件调度与数值精度、资源瓶颈指标、内存放置和容量目标、联合策略约束与基线、额外包数/线上字节/事件统计、分层外部验证与artifact协议。保持官方正文大小和边距；参考文献独立起页。`check_paper.py`按正文结束标签检查正文恰为11页，不把参考文献算入11页。
