"""Verify a paired 64-to-128-slot sweep and summarize measured differences."""
import argparse,json,statistics
from pathlib import Path
from run import dump,sha,csvwrite

def main():
 ap=argparse.ArgumentParser();ap.add_argument('old',type=Path);ap.add_argument('new',type=Path);a=ap.parse_args()
 old=json.loads((a.old/'results.json').read_text());new=json.loads((a.new/'results.json').read_text())
 oldmain={r['case_index']:r for r in old if r.get('study_variant')!='share-confirmation'}
 main=[r for r in new if r.get('study_variant')!='share-confirmation'];assert len(main)==280
 checks=[]
 for r in main:
  prev=oldmain[r['case_index']];p,q=Path(prev['run']),Path(r['run']);assert r['status']==prev['status']=='passed'
  x,y=[json.loads((d/'experiment.json').read_text())['workload']['hardware'] for d in (p,q)]
  assert x['mpu_max_inflight']==64 and y['mpu_max_inflight']==128
  x['mpu_max_inflight']=128;assert x==y
  # Workloads, placement, network and other resource inputs are unchanged.
  names=['tasks.json','resources.json','token_routes.csv','sue.conf','pcie.json','hbm.json','io-die.tsv','offload-requests.json']
  for n in names:assert sha(p/n)==sha(q/n),(r['case_index'],n)
  x,y=[json.loads((d/'io-die.json').read_text()) for d in (p,q)]
  if 'axi_mpu' in x:
   assert x['axi_mpu']['max_inflight']==64 and y['axi_mpu']['max_inflight']==128
   x['axi_mpu']['max_inflight']=128
  assert x==y
  for name in ('command-manifest.json',):
   x,y=[json.loads((d/name).read_text()) for d in (p,q)]
   for key in ('binary_sha256','adapter_sha256','adapter_headers_sha256'):assert x[key]==y[key]
  if r['backend']=='pcie':
   assert r['block_us']==prev['block_us']
   for n in ['task_statistics.csv','port_statistics.csv','resource_statistics.csv','pcie_descriptors.csv']:assert sha(p/n)==sha(q/n),(r['case_index'],n)
  checks.append(dict(index=r['case_index'],model=r['model'],target=r['target'],backend=r['backend'],mpus=r['mpus'],seed=r['seed'],old_block_us=prev['block_us'],new_block_us=r['block_us'],change_pct=(r['block_us']/prev['block_us']-1)*100))
 csvwrite(a.new/'concurrency-paired-cases.csv',checks)
 rows=[]
 for model,target in sorted({(r['model'],r['target']) for r in main}):
  rs=[r for r in checks if (r['model'],r['target'],r['backend'],r['mpus'])==(model,target,'mpu',16)]
  assert len(rs)==3
  x=statistics.mean(r['old_block_us'] for r in rs);y=statistics.mean(r['new_block_us'] for r in rs)
  baseline=statistics.mean(r['block_us'] for r in main if (r['model'],r['target'],r['backend'])==(model,target,'pcie'))
  rows.append(dict(model=model,target=target,old_64_us=x,new_128_us=y,change_pct=(y/x-1)*100,pcie_us=baseline,new_speedup=baseline/y))
 csvwrite(a.new/'concurrency-comparison.csv',rows)
 dump(a.new/'concurrency-comparison.json',dict(status='passed',paired_cases=280,pcie_baselines_bit_identical=15,changed='MPU AXI slots 64 -> 128; execution wall timeout raised to 7200s',checks=['task/resource/route/network/PCIe/HBM inputs identical','IO profile identical except MPU max_inflight','same simulator binary and C++ headers','all pure PCIe times and task/port/resource/descriptor statistics byte-identical'],mpu16=rows))
 print(json.dumps(rows,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
