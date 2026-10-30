"""Full chapter-9 sweep with capped max-min HBM and held-out shares at every MPU count."""
import argparse,json,shutil,traceback,os
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
from offload_experiments import definitions,definition,execute
from run import dump,sha,SOURCE,BINARY,HERE
import hbm_config,pcie_config

COUNTS=(1,2,4,8,16)
GRID=(.125,.25,.5,.75)

def batch(root,defs,workers):
 root.mkdir(exist_ok=False);dump(root/'design.json',defs);results=[]
 with ThreadPoolExecutor(max_workers=workers) as pool:
  fs={pool.submit(execute,root,d,i):(i,d) for i,d in enumerate(defs)}
  for f in as_completed(fs):
   i,d=fs[f]
   try:
    r=f.result()
    if r['status']=='passed' and os.environ.get('SUE_COMPRESS_TRACES')=='1':
     from trace_storage import compress_completed
     compress_completed(Path(r['run']))
   except Exception as e:r=dict(**d,status='runner_error',error=str(e),traceback=traceback.format_exc())
   r['case_index']=i;results.append(r);dump(root/'results.json',sorted(results,key=lambda r:r['case_index']))
   print(json.dumps(dict(stage=root.name,done=len(results),total=len(defs),index=i,model=d['model'],target=d['target'],mpus=d['mpus'],seed=d['seed'],status=r['status'],block_us=r.get('block_us'),error=r.get('error'))),flush=True)
 assert all(r['status']=='passed' for r in results),'Failures preserved; no partial publication'
 return sorted(results,key=lambda r:r['case_index'])

def main():
 ap=argparse.ArgumentParser();ap.add_argument('root',type=Path);ap.add_argument('--workers',type=int,default=6);ap.add_argument('--timeout-s',type=int,default=1200);args=ap.parse_args()
 assert args.timeout_s>0
 root=args.root.resolve();root.mkdir(parents=True,exist_ok=False)
 p=pcie_config.load();h=hbm_config.load()
 defs=[d for d in definitions() if d.get('study_variant','main')=='main']
 workloads=sorted({(d['model'],d['target']) for d in defs})
 for model,target in workloads:
  for m in COUNTS:
   for fraction in GRID:defs.append(definition(model,target,'hybrid',m,seed=1,study_variant='share-sensitivity',mpu_fraction=fraction))
 for d in defs:d.update(pcie_profile=p,hbm_profile=h,hbm_policy='fair',timeout_s=args.timeout_s)
 from hardware_config import HW
 plan=dict(mpu_max_inflight=HW.get('mpu_max_inflight'),primary_cases=len(defs),confirmation_cases=len(workloads)*len(COUNTS)*2,total_cases=len(defs)+50,
  workloads=workloads,mpus=COUNTS,seeds=[1,2,3],fixed_grid=GRID,
  selection='For each workload AND MPU count, select seed1 minimum block_us among fixed grid AND nominal-share seed1. Tie-break by fraction. Freeze selected shares before seeds2/3. No tuning on confirmation seeds; retain all points even if slower.',
  baseline='Main curves use PCIe seeds1/2/3; confirmed curves use PCIe seeds2/3, same HBM/PCIe profiles.',
  controls='16 XPU, B64, context32768, two representative decode blocks, same TP/EP, routing seeds, 256KiB chunking and MPU190GB/s. All arms freshly rerun.',
  boundary='Not full-model TPOT. SUE joins complete messages before HBM staging; no new packet-streaming SUE/HBM backpressure. PCIe RX retained until HBM completion. Fluid source-class fairness, no DRAM bank timing.',
  prediction='No guaranteed monotonicity, or that a seed1-selected share outperforms nominal share on held-out seeds.',hbm_profile=h,pcie_profile=p)
 dump(root/'plan.json',plan);dump(root/'hybrid-selection-plan.json',plan)
 files=[SOURCE,BINARY,*SOURCE.parent.glob('sue-*.h'),*HERE.glob('*.py')]
 dump(root/'implementation.json',{str(f):sha(f) for f in files});src=root/'source';src.mkdir()
 for f in files:
  if f!=BINARY:shutil.copy2(f,src/f.name)
 results=batch(root/'matrix',defs,args.workers)
 selected=[];confirm=[]
 for model,target in workloads:
  for m in COUNTS:
   rs=[r for r in results if (r['model'],r['target'],r['mpus'],r['backend'],r['seed'])==(model,target,m,'hybrid',1)]
   assert len(rs)==len(GRID)+1
   fraction=lambda r:r.get('mpu_fraction',190*m/(2048+190*m))
   best=min(rs,key=lambda r:(r['block_us'],fraction(r)))
   selected.append(dict(model=model,target=target,mpus=m,mpu_fraction=fraction(best),selection_case_index=best['case_index'],selection_seed=1,selection_block_us=best['block_us'],selected_variant=best.get('study_variant','main')))
   for seed in (2,3):
    confirm.append({**defs[best['case_index']],'seed':seed,'mpu_fraction':fraction(best),'study_variant':'share-confirmation'})
 dump(root/'selection.json',selected)
 heldout=batch(root/'confirmation',confirm,args.workers)
 analysis=root/'analysis';analysis.mkdir()
 dump(analysis/'design.json',defs+confirm);dump(analysis/'plan.json',{**plan,'selected':selected,'implementation_sha256':sha(root/'implementation.json')})
 dump(analysis/'results.json',results+[{**r,'confirmation_case_index':r['case_index'],'case_index':len(results)+r['case_index']} for r in heldout])
 print('COMPLETE',flush=True)
if __name__=='__main__':main()
