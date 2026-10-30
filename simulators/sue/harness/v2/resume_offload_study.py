"""Resume a registered offload matrix using isolated processes; preserve interrupted attempts."""
import argparse,json,shutil,traceback,os
from pathlib import Path
from multiprocessing import Pool
from run import dump,sha,SOURCE,BINARY,HERE,stamp
from offload_experiments import execute
from run_offload_fair_study import COUNTS,GRID

def job(args):
 root,d,i=args
 try:
  r=execute(root,d,i)
  if r['status']=='passed' and os.environ.get('SUE_COMPRESS_TRACES')=='1':
   from trace_storage import compress_completed
   compress_completed(Path(r['run']))
 except Exception as e:r=dict(**d,status='runner_error',error=str(e),traceback=traceback.format_exc())
 r['case_index']=i
 return r

def batch(root,defs,workers):
 root.mkdir(exist_ok=True)
 if (root/'design.json').exists():assert json.loads((root/'design.json').read_text())==defs
 else:dump(root/'design.json',defs)
 old=json.loads((root/'results.json').read_text()) if (root/'results.json').exists() else []
 done={r['case_index']:r for r in old if r['status']=='passed'}
 pending=[(i,d) for i,d in enumerate(defs) if i not in done]
 archive=root/'interrupted-attempts'
 for i,d in pending:
  case=root/'cases'/f'{i:03d}-{d["model"]}-{d["target"]}-{d["backend"]}-m{d["mpus"]}'
  if case.exists():
   archive.mkdir(exist_ok=True);shutil.move(str(case),archive/(case.name+'-'+stamp()))
 if old:dump(root/('previous-results-'+stamp()+'.json'),old)
 with Pool(processes=workers,maxtasksperchild=1) as pool:
  allrows=dict(done)
  for r in pool.imap_unordered(job,[(root,d,i) for i,d in pending],chunksize=1):
   i=r['case_index'];allrows[i]=r;dump(root/'results.json',sorted(allrows.values(),key=lambda x:x['case_index']))
   print(json.dumps(dict(stage=root.name,done=len(allrows),total=len(defs),index=i,status=r['status'],block_us=r.get('block_us'),error=r.get('error'))),flush=True)
 rows=sorted(allrows.values(),key=lambda x:x['case_index'])
 assert all(r['status']=='passed' for r in rows),'Incomplete or failed cases retained; publication stopped'
 return rows

def main():
 ap=argparse.ArgumentParser();ap.add_argument('root',type=Path);ap.add_argument('--workers',type=int,default=12);args=ap.parse_args();root=args.root.resolve()
 plan=json.loads((root/'plan.json').read_text());defs=json.loads((root/'matrix/design.json').read_text())
 profile=Path(os.environ['SUE_IO_DIE_PROFILE']).resolve()
 if (root/'resume-implementation.json').exists():shutil.copy2(root/'resume-implementation.json',root/('resume-implementation-'+stamp()+'.json'))
 dump(root/'resume-implementation.json',dict(io_profile=json.loads(profile.read_text()),io_sha256=sha(profile),sources={str(f):sha(f) for f in [SOURCE,BINARY,*SOURCE.parent.glob('sue-*.h'),*HERE.glob('*.py')]},workers=args.workers))
 results=batch(root/'matrix',defs,args.workers);selected=[];confirm=[]
 for model,target in plan['workloads']:
  for m in COUNTS:
   rs=[r for r in results if (r['model'],r['target'],r['mpus'],r['backend'],r['seed'])==(model,target,m,'hybrid',1)]
   assert len(rs)==len(GRID)+1
   fraction=lambda r:r.get('mpu_fraction',190*m/(2048+190*m))
   best=min(rs,key=lambda r:(r['block_us'],fraction(r)))
   selected.append(dict(model=model,target=target,mpus=m,mpu_fraction=fraction(best),selection_case_index=best['case_index'],selection_seed=1,selection_block_us=best['block_us'],selected_variant=best.get('study_variant','main')))
   for seed in (2,3):confirm.append({**defs[best['case_index']],'seed':seed,'mpu_fraction':fraction(best),'study_variant':'share-confirmation'})
 dump(root/'selection.json',selected)
 heldout=batch(root/'confirmation',confirm,args.workers)
 analysis=root/'analysis';analysis.mkdir(exist_ok=True)
 dump(analysis/'design.json',defs+confirm);dump(analysis/'plan.json',{**plan,'selected':selected,'io_profile':json.loads(profile.read_text()),'time_resolution_ns':1,'visibility_ns':1,'resume_implementation_sha256':sha(root/'resume-implementation.json')})
 dump(analysis/'results.json',results+[{**r,'confirmation_case_index':r['case_index'],'case_index':len(results)+r['case_index']} for r in heldout])
 print('COMPLETE',flush=True)
if __name__=='__main__':main()
