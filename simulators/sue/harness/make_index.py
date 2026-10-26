"""Rebuild the local example index and comparison pages from latest run pointers."""
import html
import json
from pathlib import Path
import sue_harness as h
root=Path(__file__).resolve().parent
for f in (root/'validation').glob('*/matrix.json'):
    h.comparison_report(f.parent,json.loads((f.parent/'run-ledger.json').read_text()))
rows=[]
for case in sorted((root/'examples').iterdir()):
    if not (case/'latest.json').exists(): continue
    run=Path(json.loads((case/'latest.json').read_text())['run']);s=json.loads((run/'summary.json').read_text())
    link=(run/'report.html').relative_to(root)
    rows.append(f'<tr><td>{html.escape(case.name)}</td><td>{s["completed"]}/{s["tasks"]}</td><td>{s["received_bytes"]:,} B</td><td>{s["makespan_us"]} µs</td><td><a href="{link}">打开交互报告</a></td></tr>')
(root/'index.html').write_text('''<!doctype html><meta charset="utf-8"><title>SUE Scale-Up Ethernet 仿真平台 v2</title><style>body{font:17px/1.7 system-ui;max-width:1100px;margin:45px auto;padding:0 24px;color:#183044}table{border-collapse:collapse;width:100%}td,th{padding:12px;border-bottom:1px solid #ccd9e1}a{color:#126b83}</style><h1>SUE Scale-Up Ethernet 仿真平台 v2：运行结果</h1><p>协议保持 SUE AFH + LLR + CBFC/PFC。新增自然语言实验 skills、任务 DAG、UB CSV 逻辑流量导入、参数扫描、运行台账及离线交互报告。</p><p>Kimi/GLM 是原合成 decode 工作负载前 1,000 个任务的片段，含依赖；不是完整模型执行。两者字节数不同，下面时间不构成模型性能排名。计算为固定等待，未模拟 GPU/HBM 排队。</p><table><tr><th>案例</th><th>完成任务</th><th>接收有效载荷</th><th>合成窗口</th><th>报告</th></tr>'''+''.join(rows)+'''</table><h2>控制变量验证</h2><ul><li><a href="validation/threads/report.html">单线程与 8 线程</a></li><li><a href="validation/errors/report.html">正常链路与周期错误注入</a></li><li><a href="validation/credit-window/report.html">信用窗口扫描（保留 16 信用停顿失败）</a></li></ul><p><a href="README.md">操作说明</a> · <a href="validation/test.log">测试日志</a> · <a href="validation/protocol_integrity.json">SUE 原协议源码完整性对照</a></p><p>此页是索引，跳转需要保留目录结构。各案例 report.html 自带全部数据，可以单独下载到本机离线使用。</p>''')
print(root/'index.html')
