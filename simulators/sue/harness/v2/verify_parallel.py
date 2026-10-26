"""Parallel policies: capacity, causal routing and real SUE runs."""
import copy,json,sys
from pathlib import Path
from parallel_config import resolve_parallel,layout_map
from inference_experiments import model_block,execute
from inference_profile import profile,budget
from run import dump,csvread
root=Path(sys.argv[1]).resolve();root.mkdir(parents=True,exist_ok=False)
negative=[]
for values in [{'tp':3},{'tp':8,'dp':8},{'tp':8,'ep':4},{'ep':12},{'expert_tp':2},{'pp':2},{'decode_expert_reuse_probability':1.1},{'expert_placement':'typo'},{'typo':1}]:
 try:resolve_parallel(8,values)
 except ValueError:negative.append(values)
 else:raise AssertionError(values)
specs=[]
for tp,ep,placement,tokens in [(4,32,'round_robin','round_robin'),(8,32,'round_robin','round_robin'),(4,16,'round_robin','round_robin'),(8,16,'contiguous','contiguous'),(1,16,'contiguous','round_robin')]:
 p=resolve_parallel(8,dict(tp=tp,ep=ep,expert_placement=placement,token_assignment=tokens))
 specs.append(dict(study='parallel-validation',model='glm-5.3',chips=8,tp=tp,parallel=p,batch=16,context=8192,mpus=0,seed=1,steps=2,moe_transport='tp-striped',hardware_overrides={'capacity_bytes_per_die':144e9}))
dump(root/'design.json',specs);results=[]
for i,d in enumerate(specs):
 dag,routes,meta=model_block(d);p=d['parallel']
 for r in routes:
  anchor=r['source_group']*p['tp'];assert r['owner_rank']//p['ep']==anchor//p['ep']
  assert r['expert'] in meta['parallel_layout']['ranks'][r['owner_rank']]['experts']
 if i==0:logical=[(r['step'],r['token'],r['expert']) for r in routes]
 else:assert logical==[(r['step'],r['token'],r['expert']) for r in routes]
 r=execute(root,d,i);assert r['status']=='passed',r;results.append(r)
 tasks=json.loads((Path(r['run'])/'tasks.json').read_text());stats=csvread(Path(r['run'])/'task_statistics.csv')
 assert all(int(stats[t['id']]['start_ps'])>=int(stats[parent]['end_ps']) for t in tasks for parent in t['deps'])
 for row in meta['parallel_layout']['ep_groups']:
  experts=[e for rank in row for e in meta['parallel_layout']['ranks'][rank]['experts']]
  assert sorted(experts)==list(range(256))
 dump(root/'results.json',results)
m=profile('glm-5.3');a=budget(m,8,4,8,8192,parallel={'ep':32});b=budget(m,8,4,8,8192,parallel={'ep':16})
assert b['die_weight_GB']>a['die_weight_GB'] and b['expert_replica_groups']==2
assert a['admitted'] and not b['admitted'] # standard36GB cannot fit EP16 copies
for reuse in (0,1):
 d=copy.deepcopy(specs[0]);d['parallel']['decode_expert_reuse_probability']=reuse;_,routes,_=model_block(d)
 selected={(step,t):[r['expert'] for r in routes if r['step']==step and r['token']==t] for step in (0,1) for t in range(d['batch'])}
 if reuse==1:assert all(selected[0,t]==selected[1,t] for t in range(d['batch']))
 else:assert any(selected[0,t]!=selected[1,t] for t in range(d['batch']))
dump(root/'verification.json',dict(status='passed',packet_cases=len(results),negative_configs=len(negative),checks=['TP1/4/8 and EP16/32','replica-local expert dispatch','complete expert set per EP group','unchanged logical expert choices across placement','all dependencies and payload completion','EP replication capacity increase/rejection','reuse probability endpoints']))
print('PASS',len(results),'packet cases;',len(negative),'invalid configurations rejected')
