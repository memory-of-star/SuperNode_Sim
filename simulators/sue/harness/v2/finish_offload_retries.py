"""Reconcile wall-clock-only retries with a completed primary matrix, preserving failures."""
import json,sys,shutil
from pathlib import Path
from multiprocessing import Pool
from run import dump,sha
from offload_experiments import execute

def retry(args):
 root,d,i=args
 r=execute(root,d,i);r['case_index']=i
 return r

def main(root):
 root=Path(root).resolve();matrix=root/'matrix';rs=json.loads((matrix/'results.json').read_text());defs=json.loads((matrix/'design.json').read_text())
 assert len(rs)==len(defs),'Primary batch has not finished'
 failed=[r for r in rs if r['status']!='passed']
 if not failed and (root/'retry-verification.json').exists():
  assert json.loads((root/'retry-verification.json').read_text())['status']=='passed'
  print('RETRIES ALREADY VERIFIED',flush=True);return
 assert all(r.get('exit_code')==124 for r in failed),'Investigate non-timeout failures before retrying'
 out=root/'timeout-retries';out.mkdir(exist_ok=True)
 completed=json.loads((out/'results.json').read_text()) if (out/'results.json').exists() else []
 good={r['case_index']:r for r in completed if r['status']=='passed'}
 pending=[(out,{**defs[r['case_index']],'timeout_s':7200},r['case_index']) for r in failed if r['case_index'] not in good]
 dump(out/'all-original-failures.json',failed)
 dump(out/'registered-retry-definitions.json',[{**defs[r['case_index']],'timeout_s':7200} for r in failed])
 if pending:
  with Pool(processes=min(6,len(pending)),maxtasksperchild=1) as pool:
   for r in pool.imap_unordered(retry,pending):
    completed.append(r);dump(out/'results.json',completed);assert r['status']=='passed',r
    good[r['case_index']]=r
 proof=[]
 for old in failed:
  i=old['case_index'];new=good[i];a=Path(old['run']);b=Path(new['run'])
  aa=json.loads((a/'experiment.json').read_text());bb=json.loads((b/'experiment.json').read_text())
  aa['settings'].pop('timeout_s');bb['settings'].pop('timeout_s')
  aa['workload']['definition'].pop('timeout_s');bb['workload']['definition'].pop('timeout_s');assert aa==bb
  for name in ('tasks.json','resources.json','token_routes.csv','io-die.json','hbm.json','pcie.json','sue.conf'):
   assert sha(a/name)==sha(b/name),name
  proof.append(dict(case_index=i,original_run=str(a),retry_run=str(b),original_exit=124,original_timeout_s=1200,retry_timeout_s=7200,model_inputs_identical=True))
 if failed:
  shutil.copy2(matrix/'results.json',matrix/'results-before-timeout-recovery.json')
  shutil.copy2(matrix/'design.json',matrix/'design-before-timeout-recovery.json')
  for i,d in enumerate(defs):
   if i in good:d['timeout_s']=7200
  dump(matrix/'design.json',defs)
  dump(matrix/'results.json',[good.get(r['case_index'],r) for r in rs])
 if not (root/'registered-plan.json').exists():shutil.copy2(root/'plan.json',root/'registered-plan.json')
 plan=json.loads((root/'plan.json').read_text());plan['execution_retries']=proof;dump(root/'plan.json',plan)
 dump(root/'retry-verification.json',dict(status='passed',retries=proof,changes='Wall-clock limit only; all modeled parameters, routes and bytes unchanged'))
 print('RETRIES VERIFIED',len(proof),flush=True)
if __name__=='__main__':main(sys.argv[1])
