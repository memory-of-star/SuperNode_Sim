"""Transparent hardware/resource budgets; these are not calibrated model benchmarks."""
import math
from hardware_config import HW, HARDWARE_SOURCE
from parallel_config import resolve_parallel,expert_offset,token_group
from collections import Counter
from model_config import NAMES,load_model,validate

def profile(name,model_input=None):
 bundle=model_input or load_model(name=name);raw=bundle['config'];a=bundle['simulation']
 family=validate(raw,a)
 if name!=family:raise ValueError('model definition and config family mismatch')
 c=raw.get('text_config',raw);L=c['num_hidden_layers'];E=c.get('num_experts',c.get('n_routed_experts'));k=c.get('num_experts_per_token',c.get('num_experts_per_tok'));h=c.get('routed_expert_hidden_size',c['hidden_size']);d=c['moe_intermediate_size'];lm=L-c.get('first_k_dense_replace',0)
 expert=3*h*d;routed=lm*E*expert
 emb=(1 if c.get('tie_word_embeddings',raw.get('tie_word_embeddings',False)) else 2)*c['vocab_size']*c['hidden_size']
 engram=sum(c.get('engram_num_embeddings',[]))*c.get('engram_head_dim',0)*a['engram_storage_bytes_per_element']
 lookup=len(c.get('engram_layer_ids',[]))*max(0,c.get('engram_max_ngram_size',1)-1)*c.get('engram_n_heads',0)*(c.get('engram_head_dim',0)*a['engram_lookup_bytes_per_element']+a['engram_lookup_metadata_bytes'])
 return dict(name=name,layers=L,moe_layers=lm,experts=E,top_k=k,width=h,hidden=c['hidden_size'],expert_params=expert,routed_params=routed,common_active_params=a['common_active_params'],embedding_params=emb,
  expert_weight_bytes=a['expert_weight_bytes'],common_weight_bytes=a['common_weight_bytes'],expert_dtype=a['expert_dtype'],common_dtype=a['common_dtype'],engram_bytes=engram,engram_lookup_bytes_per_token=lookup,base_chips=a['base_chips'],
  engram_rows_per_token=len(c.get('engram_layer_ids',[]))*max(0,c.get('engram_max_ngram_size',1)-1)*c.get('engram_n_heads',0),engram_row_bytes=c.get('engram_head_dim',0)*a['engram_lookup_bytes_per_element']+a['engram_lookup_metadata_bytes'],config=raw,approximation='common matrix parameters explicitly supplied by simulation.json; embedding reserve separate; no vision, DSpark/MTP or tensor-by-tensor parameter inventory')
def kv_budget(m,length):
 """Per sequence stored bytes and decode read bytes. Explicit access assumptions."""
 c=m['config'].get('text_config',m['config']);L=m['layers']
 if m['name']=='kimi-k3':
  a=c['linear_attn_config'];n=len(a['full_attn_layers']);k=len(a['kda_layers']);state=k*a['num_heads']*a['head_dim']**2*4
  cache=n*length*(c['kv_lora_rank']+c['qk_rope_head_dim'])*2
  return cache+state,cache+2*state,f'{n} full-MLA caches + FP32 KDA matrix states; KDA read/write once per step; excludes convolution scratch'
 if m['name']=='glm-5.3':
  indexers=math.ceil(L/c['index_topk_freq']);latent=(c['kv_lora_rank']+c['qk_rope_head_dim'])*2
  cap=L*length*latent+indexers*length*c['index_head_dim']*2
  read=L*min(length,c['index_topk'])*latent+indexers*length*c['index_head_dim']*2
  return cap,read,f"sparse attention {c['index_topk']} selected latents/layer + full key scan at ceil(layers/{c['index_topk_freq']}) indexers; simplified index schedule"
 sources=c['kv_source_layer_ids'];ratios=c['compress_ratios'];compressed=sum(length//ratios[i] for i in sources)
 window=L*c['sliding_window']*c['head_dim']*2
 cap=compressed*(c['head_dim']+c['index_head_dim'])*2+window
 read=L*(c['sliding_window']+min(length,c['index_topk']))*c['head_dim']*2+sum(length//max(1,ratios[i]) for i in c['index_source_layer_ids'])*c['index_head_dim']*2
 return cap,read,f'{len(sources)} shared compressed KV caches + per-layer window; index reads are conservative full scans; text only, no DSpark'
def budget(m,chips,tp,batch,context,mpus=0,engram_remote=False,expert_remote_fraction=0,hw=None,parallel=None):
 hw=hw or HW;ranks=chips*4;p=resolve_parallel(chips,parallel,tp=tp);ep=p['ep'];groups=p['dp'];active_groups=min(batch,groups)
 cap,kvread,kvnote=kv_budget(m,context)
 remote_experts={e for e in range(m['experts']) if expert_remote_fraction and e%100<round(expert_remote_fraction*100)}
 local_counts=[sum(expert_offset(e,ep,m['experts'],p['expert_placement'])==r and e not in remote_experts for e in range(m['experts'])) for r in range(ep)]
 epcap=max(local_counts)*m['moe_layers']*m['expert_params']*m['expert_weight_bytes']
 actual_remote_fraction=len(remote_experts)/m['experts']
 common=(m['common_active_params']*m['common_weight_bytes']+m['embedding_params']*2)/tp
 eg=0 if engram_remote else m['engram_bytes']/ranks
 # Static per-sequence/page striped placement; TP-replicated KV is a separate upper capacity bound.
 die_weights=epcap+common+eg
 die_kv=math.ceil(batch/ranks)*cap if batch<ranks else batch/ranks*cap
 # Pages may be striped across Die, so use balanced page placement for admitted capacity.
 die_kv=cap*batch/ranks
 used=die_weights+die_kv;limit=hw['capacity_bytes_per_die']*(1-hw['runtime_reserve_fraction'])
 remote_capacity=m['engram_bytes']*engram_remote+m['routed_params']*m['expert_weight_bytes']*actual_remote_fraction
 capacity_ok=used<=limit and remote_capacity<=mpus*hw['mpu_capacity_bytes']
 ep_tokens=Counter((token_group(t,batch,groups,p['token_assignment'])*tp)//ep for t in range(batch))
 unique=sum(m['experts']*(1-(1-m['top_k']/m['experts'])**count) for count in ep_tokens.values())
 expert_read=unique*m['moe_layers']*m['expert_params']*m['expert_weight_bytes'];common_read=m['common_active_params']*m['common_weight_bytes']*active_groups
 hbm_bytes=expert_read+common_read+kvread*batch+(0 if engram_remote else batch*m['engram_lookup_bytes_per_token'])
 # Remote weights are written to HBM then read for GEMM. They do not eliminate local read traffic.
 remote_bytes=expert_read*actual_remote_fraction+batch*m['engram_lookup_bytes_per_token']*engram_remote
 hbm_bytes+=expert_read*actual_remote_fraction
 hbms=hbm_bytes/(ranks*hw['hbm_bytes_s_per_die']*hw['hbm_efficiency'])
 ar=m['moe_layers']*m['expert_params']*m['top_k'];ac=m['common_active_params']
 cs=batch*(2*ar/(ranks*hw[m['expert_dtype']+'_flops_s_per_die'])+2*ac/(ranks*hw[m['common_dtype']+'_flops_s_per_die']))/hw['compute_efficiency']
 net_remote_bw=min(mpus*hw['mpu_bytes_s']*hw['mpu_efficiency'],mpus*hw.get('mpu_fabric_bytes_s',200e9),chips*hw['scaleup_chip_send_bytes_s']) if mpus else 0
 remote_s=remote_bytes/net_remote_bw if remote_bytes and net_remote_bw else 0
 groups_active=min(groups,batch);tp_bytes=2*(tp-1)/tp*batch*m['hidden']*2*2*m['layers']
 dispatch_bytes=2*batch*m['top_k']*(m['width']*2+8)*m['moe_layers']
 net_floor=(tp_bytes+dispatch_bytes)/(chips*hw['scaleup_chip_send_bytes_s'])
 floor=max(hbms,cs,remote_s,net_floor)
 return dict(model=m['name'],chips=chips,tp=tp,ep=ep,expert_replica_groups=ranks//ep,parallel=p,batch=batch,context=context,mpus=mpus,engram_remote=engram_remote,expert_remote_fraction=expert_remote_fraction,
  admitted=capacity_ok,actual_remote_expert_fraction=actual_remote_fraction,die_weight_GB=die_weights/1e9,die_kv_GB=die_kv/1e9,die_used_GB=used/1e9,die_limit_GB=limit/1e9,
  local_used_GB=used*ranks/1e9,remote_capacity_GB=remote_capacity/1e9,kv_total_GB=cap*batch/1e9,
  tp_replicated_kv_total_GB=cap*batch*tp/1e9,expected_unique_experts=unique,hbm_read_GB=hbm_bytes/1e9,
  hbm_service_ms=hbms*1000,parameter_compute_ms=cs*1000,remote_payload_GB=remote_bytes/1e9,remote_bandwidth_floor_ms=remote_s*1000,
  aggregate_network_volume_floor_ms=net_floor*1000,roofline_floor_ms=floor*1000,roofline_tokens_s=batch/floor,
  kv_assumption=kvnote,boundary='analytic optimistic aggregate service bound; excludes imbalance, queueing, link latency and non-matrix attention arithmetic; NOT measured TPOT')
def best_tp(m,chips,batch,context,engram_remote=False,mpus=0):
 for tp in (4,8,16,32,64,128):
  if tp<=chips*4 and (chips*4)%tp==0 and budget(m,chips,tp,batch,context,mpus,engram_remote)['admitted']:return tp
 return chips*4
