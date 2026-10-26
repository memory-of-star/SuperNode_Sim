#!/usr/bin/env python3
"""Build a self-contained manual from maintained Markdown and live example configs."""
from pathlib import Path
import hashlib,json,html,datetime,shutil,zipfile,base64,re
from markdown_it import MarkdownIt
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
 from build_offload_chapter import build as build_offload_chapter
 build_offload_chapter(HERE,ROOT)
 examples=HERE/'examples';examples.mkdir(exist_ok=True)
 shutil.copy2(ROOT/'hardware/profiles/current.json',examples/'current-hardware.json')
 for name in ['tp4-global.json','tp8-global.json','tp8-ep16.json','deepseek-16chip-tp4-ep64.json']:shutil.copy2(ROOT/'configs/parallel'/name,examples/name)
 for name in ['kimi-k3','glm-5.3','deepseek-v4.1-flash-1017']:
  for filename in ['config.json','simulation.json','source.json']:shutil.copy2(ROOT/'configs/models'/name/filename,examples/(name+'-'+filename))
 shutil.copy2(ROOT/'configs/experiments/deepseek-16chip-study.json',examples/'deepseek-16chip-study.json')
 study=ROOT/'results/sue/deepseek-16chip-case-study'
 for name in ['study-summary.csv','verification.json','sensitivity.json','case-metrics.json']:
  shutil.copy2(study/name,examples/('deepseek16-'+name))
 verification=ROOT/'results/sue/parallel-config-validation/verification.json'
 if verification.exists():shutil.copy2(verification,HERE/'validation.json')
 files=[HERE/'README.md',*sorted(HERE.glob('[0-9][0-9]-*.md'))];md=MarkdownIt('commonmark').enable('table');toc=[];parts=[]
 images=sorted({HERE/link for p in files for link in re.findall(r'!\[[^\]]*\]\(([^)]+)\)',p.read_text()) if '://' not in link})
 images=list(dict.fromkeys(images))
 assets=[HERE/'images/deepseek-16chip-parallel-prompt.txt',HERE/'images/deepseek-16chip-prompt.txt',HERE/'images/deepseek-16chip-image-record.json']
 assets.extend([HERE/'harness-capability-check.json',HERE/'report-catalog.json'])
 assets.extend(p for p in sorted((HERE/'communication').rglob('*')) if p.is_file() and p not in images)
 assets.extend(p for p in sorted((HERE/'offload').rglob('*')) if p.is_file() and p not in images)
 assets.append(HERE/'build_offload_chapter.py')
 for p in files:
  sid=p.stem;title=p.read_text().splitlines()[0].lstrip('# ')
  toc.append(f'<a href="#{sid}">{html.escape(title)}</a>')
  if sid=='01-overview':toc.append('<a href="#ai-harness">↳ AI harness 能力图</a>')
  body=md.render(p.read_text())
  if sid=='01-overview':body=body.replace('<h3>AI harness：','<h3 id="ai-harness">AI harness：',1)
  for q in files:body=body.replace(f'href="{q.name}"',f'href="#{q.stem}"')
  for image in images:
   mime='image/svg+xml' if image.suffix=='.svg' else 'image/png'
   body=body.replace(f'src="{image.relative_to(HERE)}"',f'src="data:{mime};base64,'+base64.b64encode(image.read_bytes()).decode()+'"')
  parts.append(f'<section id="{sid}">{body}</section>')
 manifest={'generated_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chapters':len(files),'markdown_characters':sum(len(p.read_text()) for p in files),'files':{str(p.relative_to(HERE)):sha(p) for p in files+images+assets+[HERE/'build_manual.py',*sorted(examples.glob('*'))]},'implementation':{}}
 for rel in ['hardware_config.py','hardware_run.py','parallel_config.py','model_config.py','inference_profile.py','inference_experiments.py','run.py','topologies.py','moe_traffic.py','run_communication_case.py','communication_report.py','offload_experiments.py','offload_confirm.py','offload_report.py']:
  p=ROOT/'simulators/sue/harness/v2'/rel;manifest['implementation'][str(p.relative_to(ROOT))]=sha(p)
 p=ROOT/'simulators/sue/ns-3-alibabacloud/simulation/scratch/sue-harness-v3.cc';manifest['implementation'][str(p.relative_to(ROOT))]=sha(p)
 css='''*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#f3f7fb;color:#19334b;font:16px/1.85 system-ui,-apple-system,"Noto Sans CJK SC",sans-serif}nav{position:fixed;width:255px;height:100vh;overflow:auto;background:#142c3e;color:white;padding:25px 18px}nav strong{display:block;font-size:23px;margin-bottom:22px}nav a{display:block;color:#b9d2df;text-decoration:none;margin:10px 0;font-size:14px}nav a:hover{color:white}main{margin-left:255px;max-width:1240px;padding:35px 42px}section{background:white;border:1px solid #dce6ee;border-radius:12px;padding:26px 32px;margin-bottom:28px;scroll-margin-top:15px}h1{font-size:28px;line-height:1.5}h2{font-size:22px;margin-top:36px;border-bottom:1px solid #dce6ee;padding-bottom:8px}h3{font-size:18px}a{color:#176b92}pre{background:#132c3f;color:#e5eef4;padding:18px;border-radius:8px;overflow:auto;font:13px/1.7 ui-monospace,monospace}code{font-family:ui-monospace,monospace;font-size:.88em;overflow-wrap:anywhere}pre code{overflow-wrap:normal}table{border-collapse:collapse;width:100%;font-size:14px;margin:20px 0}th,td{border:1px solid #dbe5ed;padding:9px 12px;vertical-align:top;overflow-wrap:anywhere}th{background:#eaf2f7;text-align:left}img{max-width:100%}blockquote{margin:15px 0;padding:8px 15px;border-left:4px solid #d98d36;background:#fff5e5}.meta{padding:15px 0;color:#597387;font-size:14px}button{cursor:pointer;background:#fff;border:1px solid #a8c3d4;border-radius:6px;padding:8px 14px;margin-right:12px}ul,ol{padding-left:24px}@media(max-width:850px){nav{position:relative;width:100%;height:auto}nav a{display:inline-block;margin:5px 10px}main{margin:0;padding:18px 10px}section{padding:20px 14px}table{display:block;overflow:auto}h1{font-size:24px}}@media print{nav,.toolbar{display:none}main{margin:0;padding:0;max-width:none}body{background:white;font-size:11pt}section{border:0;padding:0;page-break-before:always}section:first-of-type{page-break-before:auto}h1,h2,h3{break-after:avoid}pre{white-space:pre-wrap;overflow-wrap:anywhere;color:#183049;background:#f2f4f5}a{color:inherit}table{font-size:9pt}tr,img{break-inside:avoid}}'''
 page='<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SUE v2 完整使用与实现手册</title><style>'+css+'</style><nav><strong>SUE v2 · 完整手册</strong>'+''.join(toc)+'<hr><a href="manual-bundle.zip">下载手册与配置示例</a><a href="manual-manifest.json">文档与实现版本SHA</a><a href="validation.json">并行配置验收记录</a></nav><main><div class="toolbar"><button onclick="window.print()">打印 / 保存PDF</button><span>Ctrl / ⌘ + F 全文查找</span></div><div class="meta">Python行为模型 × C++ ns-3逐包网络 · 包含可配置TP/DP/EP、硬件JSON与已知边界</div>'+''.join(parts)+'</main></html>'
 (HERE/'manual.html').write_text(page)
 (HERE/'manual-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2))
 with zipfile.ZipFile(HERE/'manual-bundle.zip','w',zipfile.ZIP_DEFLATED) as z:
  for p in files+images+assets+[HERE/'manual.html',HERE/'manual-manifest.json',HERE/'validation.json',*sorted(examples.glob('*'))]:
   if p.exists():z.write(p,p.relative_to(HERE))
 print(json.dumps({'chapters':manifest['chapters'],'characters':manifest['markdown_characters'],'html':str(HERE/'manual.html')},ensure_ascii=False))
if __name__=='__main__':main()
