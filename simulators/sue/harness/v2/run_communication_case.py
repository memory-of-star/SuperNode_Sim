"""Replay frozen DeepSeek cases with passive 500 ns port observations."""
import json,sys,shutil,os
from pathlib import Path
from run import create,run_case,dump,csvread,sha

def main():
 source=Path(sys.argv[1]).resolve();root=Path(sys.argv[2]).resolve();root.mkdir(parents=True,exist_ok=True)
 assert not (root/'results.json').exists(),'use a new directory'
 old=json.loads((source/'results.json').read_text());chosen=sorted([r for r in old if r['study']=='tp4-mpu2'],key=lambda r:r['seed'])
 plan={'source':str(source),'sample_ns':500,'seeds':[r['seed'] for r in chosen],'control':'seed1 telemetry off versus on, exact timestamps and port counters','prediction':'MoE distributed across all Die; local compute and MPU service may leave idle gaps in fabric. Port queue and CBFC are measured, not inferred from application traffic.'};dump(root/'plan.json',plan)
 results=[];control=None
 for i,r in enumerate([chosen[0],*chosen]):
  src=Path(r['run']);spec=json.loads((src/'experiment.json').read_text());settings={**spec['settings'],'port_sample_ns':0 if i==0 else 500}
  if (src/'io-die.json').exists():
   profile=root/f'frozen-io-seed-{r["seed"]}.json'
   dump(profile,json.loads((src/'io-die.json').read_text())['profile'])
   os.environ['SUE_IO_DIE_PROFILE']=str(profile)
  else:os.environ.pop('SUE_IO_DIE_PROFILE',None)
  case=create(root/'cases'/('control-off' if i==0 else f'seed-{r["seed"]}'),r['topology'],json.loads((src/'tasks.json').read_text()),spec['workload'],settings=settings,routes=csvread(src/'token_routes.csv'),resources=json.loads((src/'resources.json').read_text()))
  for name in ['model-config.json','model-simulation.json','model-source.json','parallel.json','parallel-layout.json','hbm.json','hbm.tsv','hbm-requests.json','hbm-requests.tsv']:
   if (src/name).exists():shutil.copy2(src/name,case/name)
  dump(case/'replay-source.json',{'run':str(src),'task_sha256':sha(src/'tasks.json'),'resource_sha256':sha(src/'resources.json')})
  rr=run_case(case);rr.update(seed=r['seed'],sample_ns=settings['port_sample_ns'],original_run=str(src));results.append(rr);dump(root/'results.json',results);assert rr['status']=='passed',rr
  if i==0:control=Path(rr['run'])
  if i==1:
   for f in ['task_statistics.csv','port_statistics.csv','ring_statistics.csv','resource_statistics.csv']:
    assert (control/f).read_bytes()==(Path(rr['run'])/f).read_bytes(),f
  # Frozen old input schedule and current executable remain equivalent.
  assert (src/'task_statistics.csv').read_bytes()==(Path(rr['run'])/'task_statistics.csv').read_bytes()
  print(json.dumps({'seed':r['seed'],'sampling_ns':settings['port_sample_ns'],'status':rr['status'],'us':rr['makespan_us']}),flush=True)
 dump(root/'verification.json',{'status':'passed','packet_cases':len(results),'telemetry_seeds':3,'no_observer_effect':['task timestamps','port totals','ring totals','resource queues'],'all_task_timestamps_match_historical':True})
if __name__=='__main__':main()
