"""Freeze and run matched DeepSeek KV+Engram HBM FIFO/fair comparisons."""
import json,sys,traceback,shutil
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
from offload_experiments import definition,execute,DS
from run import dump,sha,SOURCE,BINARY,HERE
import pcie_config,hbm_config

def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
 p=pcie_config.load();h=hbm_config.load();defs=[]
 for mode in ('fifo','fair'):
  for backend,m in [('pcie',0)]+[('hybrid',n) for n in (1,2,4,8,16)]:
   for seed in (1,2,3):
    d=definition(DS,'both',backend,m,seed=seed,pcie_profile=p,timeout_s=600,hbm_policy=mode)
    if mode=='fair':d['hbm_profile']=h
    defs.append(d)
  for seed in (1,2,3):
   d=definition(DS,'both','hybrid',8,seed=seed,pcie_profile=p,timeout_s=600,hbm_policy=mode,mpu_fraction=.25,study_variant='fixed-share-control')
   if mode=='fair':d['hbm_profile']=h
   defs.append(d)
 dump(root/'design.json',defs)
 dump(root/'plan.json',dict(cases=len(defs),scope='DeepSeek KV+Engram, B64 context32768 TP4 DP16 EP64, two representative decode blocks; not TPOT',
  controls='Same workload, task IDs, seed, network, capacity, 256KiB chunks and transactional PCIe. Only HBM service changes. FIFO explicitly rerun and compared against immutable historical evidence.',
  prediction='Fine-grained source sharing can reduce KV waiting behind expert reads but slow the reads. Ingress caps may add staging time. No assumed monotonicity or guaranteed speedup.',
  sharing='Per-Die capped max-min across named source classes, FIFO within class. PCIe completions share one source; no per-packet fairness weight.',
  boundary='SUE still joins a complete network message before its HBM write; cap is an aggregate ingress ceiling, not packet-streaming into HBM. PCIe RX remains bounded until actual HBM service completion. No DRAM bank/row timing.',
  profile=h,pcie_profile=p))
 src=root/'source';src.mkdir()
 files=[SOURCE,BINARY,*SOURCE.parent.glob('sue-*.h'),HERE/'run.py',HERE/'hbm_config.py',HERE/'offload_experiments.py',HERE/'run_hbm_study.py']
 dump(root/'implementation.json',{str(f):sha(f) for f in files})
 for f in files:
  if f!=BINARY:shutil.copy2(f,src/f.name)
 results=[]
 with ThreadPoolExecutor(max_workers=3) as pool:
  fs={pool.submit(execute,root,d,i):(i,d) for i,d in enumerate(defs)}
  for f in as_completed(fs):
   i,d=fs[f]
   try:r=f.result()
   except Exception as e:r=dict(**d,status='runner_error',error=str(e),traceback=traceback.format_exc())
   r['case_index']=i;results.append(r);dump(root/'results.json',sorted(results,key=lambda x:x['case_index']))
   print(json.dumps(dict(done=len(results),total=len(defs),mode=d['hbm_policy'],mpus=d['mpus'],seed=d['seed'],status=r['status'],block_us=r.get('block_us'),error=r.get('error'))),flush=True)
 assert all(r['status']=='passed' for r in results)
if __name__=='__main__':main(sys.argv[1])
