"""Audit a complete SUE matrix, render/verify offline plots, then replace report artifacts."""
import argparse, datetime, json, pathlib, re, shutil, subprocess, sys, time, tempfile, xml.etree.ElementTree as ET
from zipfile import ZipFile,ZIP_DEFLATED
HERE=pathlib.Path(__file__).resolve().parent

def verify_ui(path):
 text=path.read_text();assert '<h1>Kimi K3 Scale-Up 互联收益分析</h1>' in text
 assert re.findall(r'<h2>(\d+)\.',text)==list('12345' if 'id="mtp"' in text else '1234')
 assert 'id="conclusions"' not in text and 'id="sue-results"' not in text
 for stale in ('消息级单交换机后端','没有调用 SUE','cut-through 消息模型','收发独立相对主实验','TX+RX 128 GB/s'):
  assert stale not in text,stale
 for svg in re.findall(r'<svg\b.*?</svg>',text.split('<script>')[0],re.S):ET.fromstring(svg)
 script=r'''
const fs=require('fs'),vm=require('vm');const html=fs.readFileSync(process.argv[1],'utf8');
class Elem{constructor(id){this.id=id;this._html='';this._value=undefined;this.textContent='';this.style={};this.offsetWidth=330;this.offsetHeight=130;}setAttribute(){}
 get innerHTML(){return this._html}set innerHTML(v){this._html=v;}
 get value(){if(this._value!==undefined)return this._value;return this._html.match(/<option(?: value="([^"]*)")?[^>]*>([^<]*)/s)?.slice(1).find(x=>x!==undefined)||''}
 set value(v){this._value=String(v)}addEventListener(){}click(){}querySelector(){return {outerHTML:this._html.match(/<svg\b[\s\S]*?<\/svg>/)[0]}}}
const nodes={};const get=id=>nodes[id]??(nodes[id]=new Elem(id));
for(const [id,v] of Object.entries({duplex:'sum',policy:'best',chips:'8'}))get(id).value=v;
let lastBlob;const context={document:{getElementById:get,createElement:()=>new Elem('a'),body:{appendChild(){}}},window:{innerWidth:1280,innerHeight:800},Blob,URL:{createObjectURL:b=>{lastBlob=b;return 'blob:test'},revokeObjectURL(){}},setTimeout:()=>0,console};vm.createContext(context);
for(const m of html.matchAll(/<script>([\s\S]*?)<\/script>/g))vm.runInContext(m[1],context);
let hoverChecks=0;
for(const id of ['latency','throughput','scaling']){
 const tip=get(id).innerHTML.match(/data-tip="([^"]+)"/)[1];
 const point={getAttribute:()=>tip,getBoundingClientRect:()=>({right:100,top:100})};
 get(id).onpointermove({target:{closest:()=>point},clientX:1200,clientY:760});
 if(vm.runInContext('plotTooltip.hidden || !plotTooltip.textContent.includes("：")',context))throw Error('tooltip not shown');
 if(vm.runInContext('parseFloat(plotTooltip.style.left)>942 || parseFloat(plotTooltip.style.top)>662',context))throw Error('tooltip outside viewport');
 get(id).onpointerleave();if(!vm.runInContext('plotTooltip.hidden',context))throw Error('tooltip not hidden');hoverChecks++;
}
let checks=0,samples=[];
for(const d of ['sum','independent'])for(const p of ['best','one','whole'])for(const b of [1,2,4,8,16,32,64,128,256,512,1024]){
 get('duplex').value=d;get('policy').value=p;get('scaleB').value=b;
 vm.runInContext('refresh()',context);
 for(const id of ['latency','throughput','scaling']){
  const s=get(id).innerHTML;if(/NaN|Infinity|undefined/.test(s))throw Error(id+' invalid SVG');
  if(checks===0||checks===65)for(const svg of s.matchAll(/<svg\b[\s\S]*?<\/svg>/g))samples.push(svg[0]);
 }checks++;
}
vm.runInContext('downloadAll()',context);
lastBlob.text().then(csv=>{if(!csv.includes('backend')||!csv.includes('sue'))throw Error('missing backend provenance');console.log(JSON.stringify({control_combinations:checks,hover_charts_checked:hoverChecks,csv_export:true,svg_samples:samples}))});
'''
 result=json.loads(subprocess.check_output(['node','-e',script,str(path)],text=True))
 for svg in result.pop('svg_samples'):ET.fromstring(svg)
 result.update(svg_xml='passed',method='Node DOM stubs; no browser screenshot claim')
 return result

def main():
 p=argparse.ArgumentParser();p.add_argument('--run',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--wait',action='store_true');p.add_argument('--mtp-run',type=pathlib.Path);a=p.parse_args();run=a.run.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 while True:
  try:
   v=json.loads((run/'verification.json').read_text());break
  except (FileNotFoundError,json.JSONDecodeError):
   if not a.wait:raise SystemExit('Run has not finished; report not changed')
   time.sleep(20)
 assert v['failed']==0 and v.get('pending',0)==0 and v['backend']=='sue'
 subprocess.run([sys.executable,str(HERE/'audit.py'),str(run)],check=True)
 if a.mtp_run:subprocess.run([sys.executable,str(HERE/'mtp_audit.py'),str(a.mtp_run.resolve())],check=True)
 temp=pathlib.Path(tempfile.mkdtemp(prefix='published-report-',dir=run))
 subprocess.run([sys.executable,str(HERE/'report.py'),'--run',str(run),'--output',str(temp/'report.html')]+(['--mtp-run',str(a.mtp_run.resolve())] if a.mtp_run else []),check=True)
 ui=verify_ui(temp/'report.html');(run/'ui-verification.json').write_text(json.dumps(ui,indent=2)+'\n')
 for name in ('summary.csv','results.json','verification.json','audit.json','ui-verification.json','experiment.json','implementation.json'):
  shutil.copy2(run/name,temp/name)
 with ZipFile(temp/'hm-emu-report.zip','w',ZIP_DEFLATED,compresslevel=6) as z:
  for f in temp.iterdir():
   if f.is_file() and f.suffix!='.zip':z.write(f,f.name)
  for d in (run/'conditions').iterdir():
   for name in ('case.json','engine.json','engine-sue-profile.json','engine-sue-ports.csv','engine-sue-wire.csv'):
    if (d/name).exists():z.write(d/name,f'evidence/{d.name}/{name}')
  for name in ('report.py','report.js','audit.py','publish_sue_report.py','bottleneck.py','mtp_run.py','mtp_report.py','mtp_audit.py','README.md'):
   z.write(HERE/name,'analysis_source/'+name)
  if a.mtp_run:
   for f in a.mtp_run.rglob('*'):
    if f.is_file() and 'bin' not in f.relative_to(a.mtp_run).parts:z.write(f,'mtp/'+str(f.relative_to(a.mtp_run)))
  for f in (run/'source_snapshot').iterdir():
   if f.is_file():z.write(f,'source_snapshot/'+f.name)
 with ZipFile(temp/'hm-emu-report.zip') as z:assert z.testzip() is None
 # Preserve the previous published report, including the user's editorial changes.
 archive=out/('archive-before-packets-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'));archive.mkdir()
 for name in ('report.html','hm-emu-report.zip'):
  if (out/name).exists():shutil.copy2(out/name,archive/name)
  staging=out/(name+'.new');shutil.copy2(temp/name,staging);staging.replace(out/name)
 for f in temp.iterdir():
  if f.is_file() and f.name not in ('report.html','hm-emu-report.zip'):shutil.copy2(f,out/f.name)
 (run/'publication.json').write_text(json.dumps({'status':'published','report':str(out/'report.html'),'zip':str(out/'hm-emu-report.zip'),'ui':ui},indent=2)+'\n')
 print('Verified full SUE matrix report published:',out,flush=True)
if __name__=='__main__':main()
