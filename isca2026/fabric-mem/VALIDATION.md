# 本次验证记录

日期：2026-11-03（Asia/Shanghai）。

## 构建结果

- `make -C isca2026/fabric-mem` 成功，退出码0；Tectonic 0.17.0，TeX bundle v33。
- 生成 `paper.pdf`，3页，Letter（612×792 pt），50,772 bytes。
- 本次 PDF SHA256：`17a0582051cedf05149a3efb5f4910193da132b7889aa5d9dcbbc5993b18b481`。PDF含构建时间，后续构建不保证二进制哈希不变。
- 最终 `build/main.log` 无 Warning、Overfull、Underfull、undefined 或 Missing character。
- BibTeX成功运行，3条参考文献均解析，作者列表未截断为et al.。
- `pdffonts` 确认正文为嵌入的 Nimbus Roman（Times兼容）字体，公式字体也已嵌入。
- 初次构建发现Tectonic默认TU编码与IEEEtran的ptm字体不匹配；已在加载文档类前指定T1编码并重新编译，消除字体替换警告，未修改官方文档类。
- 已渲染并查看首页、机制图/公式页及参考文献页；无可见溢出或裁切，带页码。不是正式投稿合规认证。

## 环境和来源

- 本工程重新下载官方模板ZIP；`vendor/source-manifest.json`保存来源和文件SHA256。
- `python3 isca2026/fabric-mem/tools/fetch_template.py` 再次运行成功，校验现有原件且不覆盖。
- Tectonic发行包独立下载并验证固定SHA256。首次宏包下载过程中，为减少等待中止了初始下载，复制已有TeX缓存到本工程后重新构建成功；源缓存未修改，工程不依赖该源目录。
- `bash -n isca2026/fabric-mem/tools/setup.sh isca2026/fabric-mem/tools/build.sh` 通过。
- 工具、缓存及中间产物由本目录`.gitignore`排除。原有`isca2026/`及根`.gitignore`的用户改动保留。

## 仿真检查范围

执行：`bin/sue check --config configs/experiments/glm-single.json`。

结果：退出码0，`status=validated`；`base-seed1`的`valid=true`、`capacity.admitted=true`。展开配置仍为1 ps精度、事件唤醒、1 ns请求/依赖通知间隔。检查原始输出保存在本地`build/config-check.json`。

本次没有构建或运行逐包仿真，没有新增性能结果或修改仿真器。论文中的163.84 GB/s是给定假设下的解析计算；SAPA与可服务容量主张均为待实现、待校准、待验证的研究方案。

## 目录迁移与专项文献核查（2026-11-03）

- 工程整体迁入 `isca2026/fabric-mem/`，原根目录入口已移除；编译脚本按自身位置解析路径。
- `make -C isca2026/fabric-mem` 成功，PDF 已重新生成；本次日志无 Warning 或 Overfull。
- 官方模板 manifest 校验、shell 语法检查通过。
- 修正仓库文档相对路径、文献索引生成路径及两级 README 导航。
- 资料库扩展为 42 项：34 篇论文 PDF、1 份产业演示 PDF，其余为网页或元数据；4 篇学术论文仍无全文。
- 全部已下载文件 SHA256/PDF 结构检查和研究文档本地链接检查通过，见 `ref/verification.json`。
- 新候选机制只记入 `SCALEUP_MEMORY.md`，未修改仿真器，也未运行逐包实验；英文稿保留原提案。
