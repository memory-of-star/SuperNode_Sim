"""Operator/resource planner. Seconds internally; engine interface is integer ps.
No imported HF code is executed. Tensor shapes/bytes come from saved headers.
"""
import json, math
from pathlib import Path
import numpy as np

class Model:
 def __init__(self,c,base):
  self.c=c; self.h=c['hardware']; self.a=c['modeling'];self.w=c['workload']
  self.hf=json.loads((base/c['model_config']).read_text())['text_config'];self.inv=json.loads((base/c['weight_inventory']).read_text())
  self.H=self.hf['hidden_size'];self.E=self.hf['num_experts'];self.K=self.hf['num_experts_per_token'];self.L=self.hf['num_hidden_layers'];self.full=set(x-1 for x in self.hf['linear_attn_config']['full_attn_layers'])
  self.layers=[]
  for l in range(self.L):
   idx=1 if l==0 else 4 if l in self.full else 2
   prefix=f'language_model.model.layers.{idx-1}.'
   self.layers.append({k[len(prefix):]:v for k,v in self.inv['tensors'][str(idx)].items()})
  self.tail=self.inv['tensors']['94'];self.embed=self.tail['language_model.model.embed_tokens.weight']['bytes'];self.head=self.tail['language_model.lm_head.weight']['bytes']
  self.fp=self.h['peak_flops']*self.h['compute_efficiency']/4
  self.bw=self.h['memory_bytes_per_s']*self.h['memory_efficiency']/4
  self.expert=sum(v['bytes'] for k,v in self.layers[1].items() if '.experts.0.' in k)
  self.expert_params=3*self.hf['routed_expert_hidden_size']*self.hf['moe_intermediate_size']
  self.other=sum(v['bytes'] for i in ('95','96') for v in self.inv['tensors'][i].values())
 def rep(self,k):
  return any(x in k for x in ('q_a_proj','kv_a_proj','q_a_layernorm','kv_a_layernorm','f_a_proj','self_attn.b_proj','gate.weight','e_score','routed_expert_down_proj','routed_expert_up_proj','routed_expert_norm','input_layernorm','post_attention_layernorm','_res_'))
 def weight(self,l,ranks):
  t=self.layers[l];return sum(v['bytes']*(1 if self.rep(k) else 1/ranks) for k,v in t.items() if '.experts.' not in k)+(self.E*self.expert/ranks if l else 0)
 def d2d(self,M,steps=6):
  # Four-die clockwise ring: a step sends M/4 on each edge; 16 GB/s/direction.
  return steps*(self.h['d2d_hop_ns']*1e-9+self.h['notification_ns']*1e-9+M/4/(self.h['d2d_bidirectional_bytes_per_s']/2))
 def op(self,bytes_,flops):
  mem=np.asarray(bytes_)/self.bw;comp=np.asarray(flops)/self.fp
  tiles=np.maximum(1,np.ceil(np.asarray(bytes_)/self.a['weight_tile_bytes_per_die']))
  # Equal-sized tile pipeline with bounded double buffer. No inter-op overlap.
  return np.maximum(mem,comp)+np.minimum(mem,comp)/tiles,comp,mem
 def layer(self,l,m,ranks,tiles,steps=None):
  # Features: local elapsed, compute busy, HBM busy, local ring busy, bytes, FLOPs.
  S=self.w['steps'] if steps is None else steps;G=tiles.shape[1]
  tiles=np.asarray(tiles,dtype=np.float64) # uint16 tile counts * expert bytes overflow uint32.
  out=np.zeros((S,G,3,6));context=(self.w['initial_context']+np.arange(S))[:,None]
  def add(seg,by,fl,nonmatrix=0):
   t,ct,mt=self.op(by,fl);nt=np.asarray(nonmatrix)/self.a['nonmatrix_ops_per_s_per_die']
   out[:,:,seg,0]+=t+nt;out[:,:,seg,1]+=ct+nt;out[:,:,seg,2]+=mt;out[:,:,seg,4]+=by;out[:,:,seg,5]+=fl
  def mat(seg,k,v):
   div=1 if self.rep(k) else ranks;p=math.prod(v['shape']);reuse=math.ceil(m/self.a['token_tile'])
   # Auxiliary vectors are handled as non-matrix elementwise operations.
   matrix=len(v['shape'])==2 and v['shape'][0]>1
   add(seg,v['bytes']/div*reuse+4*m*self.H/div,2*p*m/div if matrix else 0,0 if matrix else 4*p*m/div)
  for k,v in self.layers[l].items():
   if '.experts.' in k:continue
   if 'routed_expert_up_proj' in k:continue # Must execute AFTER routed all-reduce.
   seg=0 if k.startswith('self_attn.') or '_res_' in k or 'layernorm' in k else 1 if '.shared_experts.' in k or k.startswith('mlp.') else 2
   mat(seg,k,v)
  if l in self.full:
   heads=self.hf['num_attention_heads']/ranks;kv=self.hf['kv_lora_rank'];rope=self.hf['qk_rope_head_dim']
   # Absorbed MLA: compressed KV replicated per head-shard rank, not expanded KV.
   by=m*(context+1)*(kv+rope)*2
   fl=2*m*heads*context*(2*kv+rope)
   add(0,by,fl,5*m*heads*context)
  else:
   heads=self.hf['linear_attn_config']['num_heads']/ranks;d=self.hf['linear_attn_config']['head_dim']
   state=heads*d*d*self.a['state_bytes'];conv=3*heads*d*4*2
   add(0,2*m*(state+conv),6*m*heads*d*d+6*m*heads*d*4)
  blocks=1+l//self.hf['attn_res_block_size']
  add(0,2*m*self.H*(2*blocks+3)*2,0,8*m*self.H*(blocks+1))
  if l:
   add(2,tiles*self.expert/ranks+4*m*self.K*self.hf['routed_expert_hidden_size']/ranks,2*m*self.K*self.expert_params/ranks)
  dims=[self.H,self.H,self.hf['routed_expert_hidden_size']] if l else [self.H,self.H,0]
  for seg,d in enumerate(dims):
   if d:
    dt=self.d2d(m*d*2);out[:,:,seg,0]+=dt;out[:,:,seg,3]+=dt
  return out
 def up(self,m):
  p=self.H*self.hf['routed_expert_hidden_size'];by=2*p*math.ceil(m/self.a['token_tile'])+4*m*self.H
  t,c,b=self.op(by,2*m*p);return np.array([t,c,b,0,by,2*m*p])
 def endpoints(self,m):
  # Embedding replicated per die; LM head replicated per chip, vocab-sharded across 4 dies.
  te,ce,me=self.op(m*self.H*2,0)
  by=self.head/4*math.ceil(m/self.a['token_tile'])+m*self.H*2;fl=2*m*self.H*self.hf['vocab_size']/4
  th,ch,mh=self.op(by,fl);dt=self.d2d(32*m,3)
  return np.array([te,ce,me,0,m*self.H*2,0]),np.array([th+dt,ch,mh,dt,by,fl])
 def capacity(self,n,B,mode,bounds):
  ranks=4*n if mode=='TP' else 4;ctx=self.w['initial_context']+self.w['steps'];a=[]
  for card in range(n):
   lo,hi=(0,self.L) if mode=='TP' else (bounds[card],bounds[card+1]);weights=4*sum(self.weight(l,ranks) for l in range(lo,hi))
   if mode=='TP' or card==0:weights+=4*self.embed
   if mode=='TP' or card==n-1:weights+=self.head
   weights+=self.other/n
   state=0
   for l in range(lo,hi):
    if l in self.full:state+=4*B*ctx*(self.hf['kv_lora_rank']+self.hf['qk_rope_head_dim'])*2
    else:state+=4*B*(self.hf['linear_attn_config']['num_heads']/ranks)*(128*128*self.a['state_bytes']+3*128*4*2)
   workspace=4*self.a['workspace_bytes_per_die'];buffers=4*2*B*self.H*(2+math.ceil(self.L/12))*2+8*self.a['weight_tile_bytes_per_die']
   a.append(dict(card=card,weights_bytes=weights,state_bytes=state,workspace_bytes=workspace,buffer_bytes=buffers,total_bytes=weights+state+workspace+buffers))
  return a
 def partition(self,n):
  costs=[];weights=[];dummy=np.full((1,1),self.K)
  for l in range(self.L):
   cost=self.layer(l,1,4,dummy,steps=1)[0,0,:,0].sum()+(self.up(1)[0] if l else 0);costs.append(cost);weights.append(4*self.weight(l,4))
  emb,head=self.endpoints(1);costs[0]+=emb[0];costs[-1]+=head[0]
  pre=np.r_[0,np.cumsum(costs)];wp=np.r_[0,np.cumsum(weights)];dp=np.full((n+1,self.L+1),np.inf);dp[0,0]=0;prev={}
  for k in range(1,n+1):
   for end in range(k,self.L+1):
    for start in range(k-1,end):
     extra=(4*self.embed if start==0 else 0)+(self.head if end==self.L else 0)+self.other/n+4*self.a['workspace_bytes_per_die']
     if wp[end]-wp[start]+extra>self.h['memory_bytes']:continue
     v=max(dp[k-1,start],pre[end]-pre[start])
     if v<dp[k,end]:dp[k,end]=v;prev[k,end]=start
  assert np.isfinite(dp[n,self.L]);bounds=[self.L];end=self.L
  for k in range(n,0,-1):end=prev[k,end];bounds.append(end)
  bounds=bounds[::-1]
  return bounds,[{'stage':i,'layer_start':bounds[i]+1,'layer_end':bounds[i+1],'b1_service_s':float(pre[bounds[i+1]]-pre[bounds[i]])} for i in range(n)]
