#!/usr/bin/env python3
"""Explicit JSON-selected entry point for resource-coupled inference experiments."""
import argparse,os,json,sys,hashlib
from pathlib import Path

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--hardware',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--mode',choices=['case','matrix','restore','prefill'],default='case')
 p.add_argument('--model',choices=['kimi-k3','glm-5.3','deepseek-v4.1-flash-1017'],default=None)
 p.add_argument('--model-config',type=Path,help='Hugging Face config.json (data only)')
 p.add_argument('--model-simulation',type=Path,help='simulation assumptions JSON; default sibling simulation.json')
 p.add_argument('--chips',type=int,choices=[4,8,16,32],default=8);p.add_argument('--tp',type=int)
 p.add_argument('--parallel',type=Path,help='parallel strategy JSON; explicit CLI fields override it')
 p.add_argument('--ep',type=int);p.add_argument('--dp',type=int)
 p.add_argument('--moe-transport',choices=['tp-striped','leader'])
 p.add_argument('--batch',type=int,default=8);p.add_argument('--context',type=int,default=8192)
 p.add_argument('--mpus',type=int,choices=[0,2,4,8],default=0);p.add_argument('--engram-remote',action='store_true')
 p.add_argument('--port-sample-ns',type=int,default=0,help='passive port telemetry period, 0 off, >=100ns on')
 p.add_argument('--seed',type=int,default=1);p.add_argument('--steps',type=int,default=2)
 p.add_argument('--workers',type=int,default=3);p.add_argument('--validate-only',action='store_true')
 a=p.parse_args();os.environ['SUE_HARDWARE_JSON']=str(a.hardware.resolve())
 from hardware_config import HW,HARDWARE_SOURCE
 from run import dump
 from parallel_config import resolve_parallel,layout_map
 from inference_profile import profile
 from model_config import load_model,save_model
 if a.mode!='case' and a.port_sample_ns:p.error('port telemetry override requires --mode case')
 if a.port_sample_ns<0 or 0<a.port_sample_ns<100:p.error('--port-sample-ns must be 0 or >=100')
 if a.mode!='case' and (a.model_config or a.model_simulation or a.model):p.error('model overrides apply only to --mode case')
 if a.mode!='case' and any(v is not None for v in (a.parallel,a.tp,a.ep,a.dp,a.moe_transport)):
  p.error('parallel overrides apply only to --mode case; preset matrices own their parallel definitions')
 values=json.loads(a.parallel.read_text()) if a.parallel else {}
 if not isinstance(values,dict):p.error('parallel JSON must contain an object')
 try:
  parallel=resolve_parallel(a.chips,values,tp=a.tp,ep=a.ep,dp=a.dp,moe_transport=a.moe_transport)
  model_input=load_model(a.model_config,a.model_simulation,a.model);a.model=model_input['family']
  model=profile(a.model,model_input)
  if model['width']%parallel['tp'] or model['hidden']%parallel['tp']:raise ValueError('model dimensions must be divisible by TP')
 except (ValueError,OSError) as e:p.error(str(e))
 if min(a.batch,a.context,a.steps,a.workers)<1:p.error('positive sizes required')
 if a.engram_remote and (a.mpus==0 or a.model!='deepseek-v4.1-flash-1017'):p.error('--engram-remote requires DeepSeek and MPU nodes')
 layout=layout_map(a.chips,parallel,model['experts'])
 if a.validate_only:print(json.dumps({'hardware':HW,'source':HARDWARE_SOURCE,'parallel':parallel,'parallel_layout':layout,'model_profile':{k:v for k,v in model.items() if k!='config'},'model_input':model_input},ensure_ascii=False,indent=2));return
 root=a.output.resolve();root.mkdir(parents=True,exist_ok=False)
 dump(root/'hardware-profile.json',HARDWARE_SOURCE['profile']);dump(root/'hardware-source.json',HARDWARE_SOURCE);dump(root/'hardware.json',HW)
 if a.mode=='case':
  save_model(root,model_input)
  dump(root/'parallel.json',parallel);dump(root/'parallel-layout.json',layout)
  dump(root/'parallel-source.json',{'path':str(a.parallel.resolve()) if a.parallel else None,'sha256':hashlib.sha256(a.parallel.read_bytes()).hexdigest() if a.parallel else None,'input':values,'cli_overrides':{k:getattr(a,k) for k in ('tp','ep','dp','moe_transport') if getattr(a,k) is not None}})
 dump(root/'invocation.json',{'argv':sys.argv,'cwd':str(Path.cwd())})
 if a.mode=='case':
  from inference_experiments import execute
  d=dict(study='hardware-profile-case',port_sample_ns=a.port_sample_ns,model=a.model,model_input=model_input,chips=a.chips,tp=parallel['tp'],parallel=parallel,batch=a.batch,context=a.context,mpus=a.mpus,seed=a.seed,steps=a.steps,engram_remote=a.engram_remote,include_engram=a.model.startswith('deepseek'),moe_transport=parallel['moe_transport'])
  dump(root/'design.json',[d]);result=execute(root,d,0);dump(root/'results.json',[result]);print(json.dumps(result,ensure_ascii=False,indent=2))
  if result['status'] not in ('passed','capacity_rejected'):raise SystemExit(1)
 elif a.mode=='matrix':
  from inference_experiments import main as run
  run(root,a.workers)
 elif a.mode=='restore':
  from inference_restore import main as run
  run(root)
 else:
  from inference_prefill import main as run
  run(root)
if __name__=='__main__':main()
