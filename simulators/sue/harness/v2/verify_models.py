"""Validate JSON adapters, causal changes, snapshots and real packet runs."""
import copy,json,sys,hashlib
from pathlib import Path
from model_config import load_model,validate,NAMES,ROOT
from inference_profile import profile,budget,best_tp
from inference_experiments import model_block,execute
from run import dump

def main():
 root=Path(sys.argv[1]).resolve();root.mkdir(parents=True,exist_ok=False)
 specs=[];checks=[];negative=0
 for name,chips,mpus in [(NAMES[0],16,0),(NAMES[1],8,0),(NAMES[2],4,2)]:
  bundle=load_model(ROOT/name/'config.json');assert bundle['source']['upstream_hash_matches']
  d=dict(study='model-json',model=name,chips=chips,tp=best_tp(profile(name),chips,8,8192,bool(mpus),mpus),batch=8,context=8192,mpus=mpus,engram_remote=bool(mpus),include_engram=name==NAMES[2],seed=1,steps=2)
  implicit=model_block(d);explicit=model_block({**d,'model_input':bundle})
  assert implicit[0].tasks==explicit[0].tasks and implicit[0].resources==explicit[0].resources and implicit[1]==explicit[1]
  specs.append({**d,'model_input':bundle})
 checks.append('all three names and explicit HF JSON generate identical tasks/resources/routes')
 glm=copy.deepcopy(specs[1]);glm['model_input']['config']['num_experts_per_tok']=4;glm['model_input']['config']['n_routed_experts']=128
 glm['model_input']['simulation']['expert_dtype']='fp4';glm['study']='modified-glm'
 a=model_block(specs[1]);b=model_block(glm)
 assert len(b[1])*2==len(a[1])
 assert b[2]['capacity']['die_weight_GB']<a[2]['capacity']['die_weight_GB']
 assert sum(x['service_ns'] for x in b[0].resources if x['kind']==0)<sum(x['service_ns'] for x in a[0].resources if x['kind']==0)
 specs.append(glm);checks.append('changed experts/top-k/precision affect dispatch, capacity and compute service')
 ds=copy.deepcopy(specs[2]);c=ds['model_input']['config']['text_config'];c['engram_n_heads']=4
 a=model_block(specs[2]);b=model_block(ds)
 assert sum(t['bytes'] for t in b[0].tasks if ':engram:data-response' in t['label'])*2==sum(t['bytes'] for t in a[0].tasks if ':engram:data-response' in t['label'])
 checks.append('DeepSeek configured lookup heads change remote payload')
 for field,value in [('num_experts_per_tok',257),('num_hidden_layers',0),('hidden_size',True),('index_topk_freq',0),('model_type','llama'),('moe_layer_freq',2)]:
  b=copy.deepcopy(specs[1]['model_input']);b['config'][field]=value
  try:validate(b['config'],b['simulation'])
  except ValueError:negative+=1
  else:raise AssertionError(field)
 for field,value in [('family',NAMES[0]),('expert_dtype','fp32'),('expert_weight_bytes',-1),('common_active_params',float('nan')),('typo',1)]:
  b=copy.deepcopy(specs[1]['model_input']);b['simulation'][field]=value
  try:validate(b['config'],b['simulation'])
  except ValueError:negative+=1
  else:raise AssertionError(field)
 missing=root/'no-sidecar';missing.mkdir();dump(missing/'config.json',specs[1]['model_input']['config'])
 try:load_model(missing/'config.json')
 except ValueError:negative+=1
 else:raise AssertionError('missing sidecar accepted')
 # Reproducible user-edited inputs: hash the actual modified JSON, not its ancestor.
 edited=root/'edited-glm';edited.mkdir();dump(edited/'config.json',glm['model_input']['config']);dump(edited/'simulation.json',glm['model_input']['simulation']);glm['model_input']=load_model(edited/'config.json')
 dump(root/'design.json',specs);results=[]
 for i,d in enumerate(specs):
  r=execute(root,d,i);assert r['status']=='passed',r;results.append(r);dump(root/'results.json',results)
  run=Path(r['run']);manifest=json.loads((run/'command-manifest.json').read_text())
  for f in ['model-config.json','model-simulation.json','model-source.json']:
   assert manifest['inputs'][f]==hashlib.sha256((run/f).read_bytes()).hexdigest()
 checks.append('each packet run completes and hashes model config/assumptions/source snapshots')
 dump(root/'verification.json',dict(status='passed',packet_cases=len(results),negative_configs=negative,checks=checks))
 print('PASS',len(results),'packet cases;',negative,'negative configs')
if __name__=='__main__':main()
