"""Read HF config data only (never execute auto_map code or load weights)."""
import hashlib,json,math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]/'configs/models'
NAMES=['kimi-k3','glm-5.3','deepseek-v4.1-flash-1017']
TYPES={'kimi_k3':NAMES[0],'kimi_linear':NAMES[0],'glm_moe_dsa':NAMES[1],'deepseek_v41':NAMES[2],'deepseek_v41_text':NAMES[2]}

def read(path):
 path=Path(path).resolve();data=path.read_bytes();obj=json.loads(data)
 if not isinstance(obj,dict):raise ValueError(f'{path}: expected JSON object')
 return obj,dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())

def number(c,key,minimum=1,integer=True):
 v=c.get(key)
 if isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v) or v<minimum or (integer and not isinstance(v,int)):
  raise ValueError(f'{key}: expected {"integer" if integer else "number"} >= {minimum}')
 return v

def validate(raw,a):
 c=raw.get('text_config',raw)
 if not isinstance(c,dict):raise ValueError('text_config must be an object')
 family=TYPES.get(raw.get('model_type')) or TYPES.get(c.get('model_type'))
 if family is None:raise ValueError('unsupported model_type; supported adapters: Kimi K3, GLM-5.3, DeepSeek V4.1')
 if a.get('family')!=family:raise ValueError('simulation.family does not match HF model_type')
 allowed={'schema_version','family','notes','common_active_params','expert_weight_bytes','common_weight_bytes','expert_dtype','common_dtype','base_chips','engram_storage_bytes_per_element','engram_lookup_bytes_per_element','engram_lookup_metadata_bytes'}
 if set(a)-allowed:raise ValueError(f'unknown simulation fields: {sorted(set(a)-allowed)}')
 if type(a.get('schema_version')) is not int or a['schema_version']!=1:raise ValueError('simulation schema_version must be 1')
 for k in ('common_active_params','expert_weight_bytes','common_weight_bytes','engram_storage_bytes_per_element','engram_lookup_bytes_per_element'):number(a,k,0 if k=='common_active_params' else 1e-12,False)
 number(a,'base_chips');number(a,'engram_lookup_metadata_bytes',0)
 for k in ('expert_dtype','common_dtype'):
  if a.get(k) not in ('fp4','fp8','fp16'):raise ValueError(f'{k}: supported fp4/fp8/fp16')
 for k in ('num_hidden_layers','hidden_size','moe_intermediate_size','vocab_size'):number(c,k)
 for keys in [('num_experts','n_routed_experts'),('num_experts_per_token','num_experts_per_tok')]:
  present=[k for k in keys if k in c]
  if not present:raise ValueError(f'missing {keys}')
  for k in present:number(c,k)
  if len(present)==2 and c[present[0]]!=c[present[1]]:raise ValueError(f'conflicting aliases {keys}')
 E=c.get('num_experts',c.get('n_routed_experts'));k=c.get('num_experts_per_token',c.get('num_experts_per_tok'));L=c['num_hidden_layers']
 if k>E:raise ValueError('top_k exceeds number of experts')
 if 'routed_expert_hidden_size' in c:number(c,'routed_expert_hidden_size')
 dense=c.get('first_k_dense_replace',0)
 if type(dense) is not int or not 0<=dense<L:raise ValueError('first_k_dense_replace must be in [0, layers)')
 if 'mlp_layer_types' in c and c['mlp_layer_types']!=['dense']*dense+['sparse']*(L-dense):raise ValueError('unsupported mlp_layer_types; expected dense prefix then sparse layers')
 if c.get('moe_layer_freq',1)!=1:raise ValueError('only contiguous MoE layers with moe_layer_freq=1 supported')
 if family==NAMES[0]:
  for key in ('kv_lora_rank','qk_rope_head_dim'):number(c,key)
  linear=c.get('linear_attn_config')
  if not isinstance(linear,dict):raise ValueError('Kimi requires linear_attn_config')
  for key in ('num_heads','head_dim'):number(linear,key)
  ids=[]
  for key in ('full_attn_layers','kda_layers'):
   arr=linear.get(key)
   if not isinstance(arr,list) or not arr or any(type(i) is not int or not 1<=i<=L for i in arr):raise ValueError(f'{key}: expected 1-based layer IDs')
   ids+=arr
  if len(set(ids))!=len(ids) or set(ids)!=set(range(1,L+1)):raise ValueError('Kimi attention layers must partition all layers')
 elif family==NAMES[1]:
  for key in ('kv_lora_rank','qk_rope_head_dim','index_topk_freq','index_head_dim','index_topk'):number(c,key)
 else:
  for key in ('sliding_window','head_dim','index_head_dim','index_topk','engram_max_ngram_size','engram_n_heads','engram_head_dim'):number(c,key)
  ratios=c.get('compress_ratios')
  if not isinstance(ratios,list) or len(ratios)<L or any(type(r) is not int or r<0 for r in ratios):raise ValueError('compress_ratios must cover all layers with nonnegative integers')
  for key in ('kv_source_layer_ids','index_source_layer_ids','engram_layer_ids'):
   arr=c.get(key)
   if not isinstance(arr,list) or not arr or len(set(arr))!=len(arr) or any(type(i) is not int or not 0<=i<L for i in arr):raise ValueError(f'{key}: expected unique 0-based layer IDs')
   if key=='kv_source_layer_ids' and any(ratios[i]==0 for i in arr):raise ValueError('KV source compression ratio must be positive')
  emb=c.get('engram_num_embeddings')
  if not isinstance(emb,list) or len(emb)!=len(c['engram_layer_ids']) or any(type(v) is not int or v<=0 for v in emb):raise ValueError('invalid engram_num_embeddings')
 return family

def load_model(config=None,simulation=None,name=None):
 if config is None:
  name=name or NAMES[1]
  if name not in NAMES:raise ValueError('unsupported model name')
  config=ROOT/name/'config.json'
 raw,source=read(config);c=raw.get('text_config',raw)
 if not isinstance(c,dict):raise ValueError('text_config must be an object')
 family=TYPES.get(raw.get('model_type')) or TYPES.get(c.get('model_type'))
 if family is None:raise ValueError('unsupported HF model_type')
 if name is not None and name!=family:raise ValueError('--model conflicts with --model-config family')
 # Never silently borrow numeric assumptions for an arbitrary downloaded architecture.
 simulation=Path(simulation) if simulation else Path(config).with_name('simulation.json')
 if not simulation.is_file():raise ValueError('missing simulation.json: provide --model-simulation explicitly; HF config alone lacks compute/precision assumptions')
 assumptions,simsource=read(simulation);validate(raw,assumptions)
 provenance=Path(config).with_name('source.json')
 upstream=json.loads(provenance.read_text()) if provenance.is_file() else None
 return dict(config=raw,simulation=assumptions,source={'config':source,'simulation':simsource,'upstream':upstream,'upstream_hash_matches':bool(upstream and upstream.get('sha256')==source['sha256'])},family=family)

def save_model(path,bundle):
 path=Path(path);path.mkdir(parents=True,exist_ok=True)
 for filename,key in [('model-config.json','config'),('model-simulation.json','simulation'),('model-source.json','source')]:
  (path/filename).write_text(json.dumps(bundle[key],ensure_ascii=False,indent=2)+'\n')
