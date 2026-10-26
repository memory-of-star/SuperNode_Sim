"""Registered 16-chip DeepSeek study for the manual; use a fresh output directory."""
import json,sys,traceback
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
from inference_experiments import execute
from model_config import load_model,ROOT as MODEL_ROOT
from hardware_config import save_hardware
from parallel_config import resolve_parallel
from run import dump,sha

def main():
 planpath=Path(sys.argv[1]).resolve();root=Path(sys.argv[2]).resolve();root.mkdir(parents=True,exist_ok=False)
 plan=json.loads(planpath.read_text());bundle=load_model(MODEL_ROOT/plan['model']/'config.json')
 dump(root/'registered-plan.json',plan);save_hardware(root);defs=[]
 for arm in plan['arms']:
  for seed in plan['seeds']:
   defs.append(dict(study=arm['name'],model=plan['model'],model_input=bundle,chips=plan['chips'],tp=arm['tp'],parallel=resolve_parallel(plan['chips'],dict(tp=arm['tp'],ep=64)),batch=plan['batch'],context=plan['context'],mpus=arm['mpus'],engram_remote=bool(arm['mpus']),include_engram=True,seed=seed,steps=plan['steps']))
 base=defs[0]
 for arm in plan['sensitivity']:defs.append({**base,'study':arm['name'],'hardware_overrides':arm['hardware_overrides']})
 dump(root/'design.json',defs);dump(root/'implementation.json',{'sources':{p.name:sha(p) for p in Path(__file__).resolve().parent.glob('*.py')}})
 results=[]
 with ThreadPoolExecutor(max_workers=3) as pool:
  futures={pool.submit(execute,root,d,i):d for i,d in enumerate(defs)}
  for f in as_completed(futures):
   try:r=f.result()
   except Exception as e:r={**futures[f],'status':'runner_error','error':str(e),'traceback':traceback.format_exc()}
   results.append(r);dump(root/'results.json',results);print(json.dumps({k:r.get(k) for k in ('study','seed','status','window_us','error')}),flush=True)
 dump(root/'verification.json',{'cases':len(results),'passed':sum(r['status']=='passed' for r in results),'failed':[{'study':r['study'],'seed':r['seed'],'status':r['status']} for r in results if r['status']!='passed']})
 if any(r['status']!='passed' for r in results):raise SystemExit(1)
if __name__=='__main__':main()
