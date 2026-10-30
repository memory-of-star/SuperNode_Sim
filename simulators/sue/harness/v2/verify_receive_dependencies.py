"""Verify receiver-ready dependencies, retained ACKs, and immutable-case compatibility."""
import json,sys,shutil,copy
from pathlib import Path
from inference_experiments import DAG,HW,SETTINGS
from run import create,run_case,csvread,dump,sha,check_tasks
from topologies import TOPOLOGIES
import hbm_config,pcie_config
from offload_experiments import execute,definition,DS

def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
 dump(root/'plan.json',dict(micro_sizes=[264,3168,20000],policies=['ack','receive'],
   assertions='Receiver tasks start after full reception+configured visibility; ACK-dependent tasks still wait ACK; all ACKs complete; fragmented data not exposed early; mixed dependencies; frozen full-case byte equality; Engram16 full workload'))
 results=[]
 for size in (264,3168,20000):
  for policy in ('ack','receive'):
   d=DAG({**HW,'mpu_efficiency':1.0});d.remote_read((0,0),16,size,[],'read',rows=1 if size==264 else 12 if size==3168 else 0)
   sender=d.local(0,0,0,1,[0],'sender-waits-ack',service_ns=1)
   # Join a receive edge with an unrelated completion edge at the receiver.
   wait=d.local(16,0,2,1,[],'independent',service_ns=1)
   join=d.local(16,0,2,1,[0,wait],'mixed-join',service_ns=1)
   d.tasks[join]['receive_deps']=[0]
   if policy=='ack':
    for t in d.tasks:t.pop('receive_deps',None)
   case=create(root/f'{size}-{policy}','hw-16-1',d.tasks,{'test':'receive-dependencies'},settings=SETTINGS,resources=d.resources)
   hbm_config.write(case,hbm_config.load(),d.tasks,d.resources)
   r=run_case(case);assert r['status']=='passed',r
   gap_ps=json.loads((case/'experiment.json').read_text())['settings']['visibility_ns']*1000
   stats={int(x['id']):x for x in csvread(Path(r['run'])/'task_statistics.csv')}
   for t in d.tasks:
    for parent in t['deps']:
     key='data_end_ps' if parent in t.get('receive_deps',[]) else 'end_ps'
     assert int(stats[t['id']]['start_ps'])>=int(stats[parent][key])+gap_ps
   if policy=='receive':
    assert int(stats[1]['start_ps'])<int(stats[0]['end_ps'])
    assert int(stats[3]['start_ps'])<int(stats[2]['end_ps'])
   assert int(stats[sender]['start_ps'])>=int(stats[0]['end_ps'])+gap_ps
   results.append(dict(size=size,policy=policy,run=r['run'],hbm_ready_us=(int(stats[3]['end_ps'])-int(stats[0]['start_ps']))/1e6))
 # Invalid receive dependencies must not pass Python validation.
 for invalid in ([0,0],[wait],[-1]):
  bad=copy.deepcopy(d.tasks);bad[1]['receive_deps']=invalid
  try:check_tasks(TOPOLOGIES['hw-16-1'],bad,{**__import__('run').DEFAULT,**SETTINGS})
  except AssertionError:pass
  else:raise AssertionError('invalid receive edge accepted')
 baseline=json.loads(Path('results/sue/offload-hbm-fair-full-20261028/analysis/results.json').read_text())
 old=next(r for r in baseline if r['model']==DS and r['target']=='engram' and r['backend']=='mpu' and r['mpus']==16 and r['seed']==1 and r.get('study_variant','main')=='main')
 oldcase=root/'frozen-case';oldcase.mkdir()
 for p in Path(old['run']).iterdir():
  if p.is_file():shutil.copy2(p,oldcase/p.name)
 replay=run_case(oldcase);assert replay['status']=='passed'
 for name in ('task_statistics.csv','resource_statistics.csv','port_statistics.csv','hbm_sources.csv'):
  assert sha(Path(replay['run'])/name)==sha(Path(old['run'])/name),name
 new=execute(root,definition(DS,'engram','mpu',16,seed=1,pcie_profile=pcie_config.load(),hbm_profile=hbm_config.load(),timeout_s=600),0)
 assert new['status']=='passed',new
 dump(root/'verification.json',dict(status='passed',micro=results,invalid_cases=3,frozen_replay=replay['run'],
    frozen_reference=old['run'],full_case=new,previous_block_us=old['block_us']))
 print(json.dumps(dict(status='passed',micro=results,old_us=old['block_us'],new_us=new['block_us']),indent=2))
if __name__=='__main__':main(sys.argv[1])
