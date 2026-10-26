#!/usr/bin/env python3
"""Package report and all immutable run snapshots, avoiding duplicate case inputs."""
import json,sys,zipfile,shutil,hashlib
from pathlib import Path
from run import HERE,SOURCE,dump,sha

def package(root):
    root=Path(root).resolve();source=root/'source';source.mkdir(exist_ok=True)
    for p in HERE.iterdir():
        if p.is_file() and p.suffix in ('.py','.js','.html','.md','.svg'):shutil.copy2(p,source/p.name)
    shutil.copy2(SOURCE,source/SOURCE.name)
    inputs=root/'inputs';inputs.mkdir(exist_ok=True)
    for p in (HERE/'inputs').iterdir():
        if p.is_file():shutil.copy2(p,inputs/p.name)
    dump(source/'source-manifest.json',{str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file() and p.name!='source-manifest.json'})
    (root/'打开与复现.md').write_text('''# 受控探索报告

打开report.html可离线筛选；原始实验CSV在cases下，网页相对链接可用。summary.csv为全366项，paired-results.csv为成功配对变化，plan-initial.md与design-initial.json是初始348项条件；执行中核对出口缓冲耦合后，credit-addendum-plan.md单独声明追加18项固定缓冲对照，design.json为合并登记。analysis.json为图表数据；verification.json校验输入、配对及重复控制。

包保留所有runs快照，不重复打包各case根下与快照相同的输入。原96项实验的父报告不包含在本包，公网导航可返回查看。运行JSON中的绝对路径是原机器位置，包内证据链接使用相对路径。

恢复到已有SUE v2工程时，将source里的Python/JS/HTML/Markdown/SVG放入harness/v2；inputs放入harness/v2/inputs，C++适配器保持对应构建位置。原ns-3基座仍需存在，包不是独立可运行的二进制发行版。执行：

```bash
python3 harness/v2/explore.py reports/new-exploration 4
python3 harness/v2/explore_credit_control.py reports/new-exploration
# 报告依赖：python3 -m pip install markdown-it-py
python3 harness/v2/explore_report.py reports/new-exploration
node harness/v2/verify_explore_ui.js reports/new-exploration/report.html
python3 harness/v2/package_exploration.py reports/new-exploration
```

探索runner默认拒绝覆盖目录。每次重新执行使用新目录；失败也保留。4为独立案例并发数，不是单次仿真线程数。固定计算等待不是GPU计算模型；三种子范围不是置信区间。源卡0错误注入不是全网随机BER。

本次中间UCIe占比辅助字段曾因整数/字符串字典键差异错误，已按冻结路径计数重算。修正台账与中间原始结果保留；没有改写原始任务/端口/环测量或重跑以挑选结果。
''')
    selected=set(p for p in root.iterdir() if p.is_file() and '.zip' not in p.name)
    selected.update(p for folder in (source,inputs,root/'diagnostics') for p in folder.rglob('*') if p.is_file())
    for r in json.loads((root/'results.json').read_text()):
        if r.get('run'):selected.update(p for p in Path(r['run']).rglob('*') if p.is_file())
        elif r.get('case') and Path(r['case']).exists():selected.update(p for p in Path(r['case']).rglob('*') if p.is_file())
    dest=root/'exploration-evidence.zip';temp=root/'exploration-evidence.tmp.zip'
    with zipfile.ZipFile(temp,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
        for p in sorted(selected):z.write(p,str(p.relative_to(root.parent)))
    with zipfile.ZipFile(temp) as z:
        assert z.testzip() is None;count=len(z.infolist())
    temp.replace(dest)
    print(json.dumps(dict(archive=str(dest),MiB=dest.stat().st_size/1048576,files=count,status='passed')))

if __name__=='__main__':package(sys.argv[1])
