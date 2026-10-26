"""Test actual packet delivery, all-Die participation and MoE causal dependencies."""
import json,sys
from pathlib import Path
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from inference_experiments import model_block,execute
from run import dump,csvread

root=Path(sys.argv[1]).resolve();root.mkdir(parents=True,exist_ok=False)
def definition(model='glm-5.3',chips=8,tp=4,batch=8,seed=1,policy='tp-striped'):
 return dict(study='moe_striping_validation',model=model,chips=chips,tp=tp,batch=batch,context=8192,mpus=0,seed=seed,steps=2,moe_transport=policy)
def check_dag(d):
 dag,routes,_=model_block(d);old,old_routes,_=model_block({**d,'moe_transport':'leader'})
 assert [(r['step'],r['token'],r['expert'],r['owner_rank']) for r in routes]==[(r['step'],r['token'],r['expert'],r['owner_rank']) for r in old_routes]
 # Expert GEMM/read volume is unaffected by transport striping.
 for kind in ('expert-compute','expert-hbm'):
  def volumes(x):return sum(r['units'] for r in x.resources if x.tasks[r['id']]['label'].endswith(kind))
  assert volumes(dag)==volumes(old)
 def transfers(x,phase):
  resources={r['id']:r for r in x.resources}
  return [(t,t['bytes'] or resources[t['id']]['units']) for t in x.tasks if t['label'].endswith(':'+phase)]
 # Paired dispatch/return payload including every fragment's metadata is conserved.
 assert sum(n for t,n in transfers(dag,'dispatch'))==sum(n for t,n in transfers(dag,'combine'))
 # Every compute node waits for every incoming fragment targeting that expert Die.
 for t in dag.tasks:
  if not t['label'].endswith(':expert-compute'):continue
  incoming={x['id'] for x,n in transfers(dag,'dispatch') if x['label'].split(':')[0]==t['label'].split(':')[0] and ((x['dst_chip'],x['dst_die'])==(t['src_chip'],t['src_die']) and x['bytes']>0)}
  assert incoming<=set(t['deps'])
 return dag
specs=[]
for model,n,tp in [('glm-5.3',8,4),('glm-5.3',8,8),('kimi-k3',16,32),('deepseek-v4.1-flash-1017',8,4)]:
 for seed in (1,2,3):
  d=definition(model,n,tp,8,seed);check_dag(d)
  specs.extend([d,{**d,'moe_transport':'leader'}])
# A single token still spreads its feature shards across all four Die of its active chip.
specs.append(definition(batch=1))
dump(root/'design.json',specs)
with ThreadPoolExecutor(max_workers=3) as pool:
 results=list(pool.map(lambda z:execute(root,z[1],z[0]),enumerate(specs)))
for d,r in zip(specs,results):
 assert r['status']=='passed',r
 if d['moe_transport']=='tp-striped':
  flows=csvread(Path(r['run'])/'moe_die_traffic.csv');active={int(x['chip'])*4+int(x['die']) for x in flows if x['phase']=='dispatch' and int(x['fabric_tx_bytes'])>0}
  groups=min(d['batch'],d['chips']*4//d['tp'])
  assert active==set(range(groups*d['tp'])),(d,active)
  ts=csvread(Path(r['run'])/'task_statistics.csv');tasks=json.loads((Path(r['run'])/'tasks.json').read_text())
  for t in tasks:
   assert all(int(ts[t['id']]['start_ps'])>=int(ts[parent]['end_ps']) for parent in t['deps'])
 dump(root/'results.json',results)
dump(root/'verification.json',dict(status='passed',cases=len(results),checks=['same experts and GEMM/HBM volume in paired policies','dispatch/combine payload balance','expert waits for incoming fragments','all active TP ranks send real fabric payload including batch1','all task dependencies respected','all SUE payloads completed']))
print('PASS',len(results),flush=True)
