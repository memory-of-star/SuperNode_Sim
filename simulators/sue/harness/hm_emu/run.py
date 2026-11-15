#!/usr/bin/env python3
"""Reproducible hm_emu full-decoder study; explicit alternate backend."""
import argparse, concurrent.futures, csv, hashlib, json, math, os, pathlib, subprocess, sys, time
import numpy as np
from model import Model
HERE=pathlib.Path(__file__).resolve().parent
ROOT=HERE.parents[3]
SIM=ROOT/'simulators/sue/ns-3-alibabacloud/simulation'
BIN=ROOT/'results/build/hm-emu-engine'
def dump(path,obj):path.write_text(json.dumps(obj,indent=2,ensure_ascii=False,allow_nan=False)+'\n')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def build(backend="message"):
 global BIN
 BIN=ROOT/("results/build/hm-emu-sue-engine" if backend=="sue" else "results/build/hm-emu-engine")
 BIN.parent.mkdir(parents=True,exist_ok=True)
 cmd=['g++','-std=c++17','-O3','-include','stdint.h',str(HERE/'engine.cc'),'-I'+str(SIM/'build/include'),'-L'+str(SIM/'build/lib'),'-Wl,-rpath,'+str(SIM/'build/lib'),'-lns3.36.1-core-default','-o',str(BIN)]
 if backend=="sue":cmd += ["-DHM_SUE","-lns3.36.1-sue-default","-lns3.36.1-network-default","-lns3.36.1-point-to-point-default"]
 subprocess.run(cmd,check=True);return cmd

def load(path):
 c=json.loads(path.read_text());assert c['schema']=='hm-emu-1';assert c['hardware']['dies_per_chip']==4
 assert c['hardware']['d2d_scope']=='per_ring_edge';assert c['modeling']['mla_cache']=='compressed_absorbed'
 assert c['modeling'].get('topology','ring') in ('ring','single_switch')
 assert c['modeling'].get('backend','message') in ('message','sue')
 if c['modeling'].get('backend')=='sue':
  assert c['modeling']['topology']=='single_switch'
  assert set(c['modeling'].get('sue_profile',{})) <= {'payload','credits','bad_every','link_propagation_ns','switch_forwarding_ns'}
  assert c['modeling'].get('scaleup_duplex_allocation','fixed_symmetric_per_direction')=='fixed_symmetric_per_direction'
 if c['modeling'].get('backend')=='sue':
  profile=c['modeling'].get('sue_profile',{})
  floor=2*profile.get('link_propagation_ns',20)+profile.get('switch_forwarding_ns',300)
  assert c['hardware']['scaleup_hop_ns']*(.5 if 'latency_half' in c['sensitivity']['variants'] else 1)>=floor
 assert c['workload']['steps']>=2
 for b in c['workload']['batches']:assert b>0 and b&(b-1)==0
 return c,Model(c,path.parent)

def routes(c,out):
 """One common route stream; merge adjacent request groups for all microbatch sizes."""
 w=c['workload'];S=w['steps'];B=max(w['batches']);L=93;E=896;K=16
 arrays={m:np.lib.format.open_memmap(out/f'expert-tiles-m{m}.npy',mode='w+',dtype=np.uint16,shape=(S,L,B//m)) for m in w['batches']}
 rng=np.random.default_rng(w['seed']);digest=hashlib.sha256();t=time.time()
 for st in range(S):
  for l in range(1,L):
   # Rejection sampling of independent ordered uniform draws -> uniform subsets.
   x=rng.integers(E,size=(B,K),dtype=np.uint16)
   for j in range(1,K):
    bad=np.any(x[:,:j]==x[:,j,None],axis=1)
    while bad.any():x[bad,j]=rng.integers(E,size=int(bad.sum()),dtype=np.uint16);bad=np.any(x[:,:j]==x[:,j,None],axis=1)
   digest.update(x.tobytes());counts=np.zeros((B,E),dtype=np.uint16);counts[np.arange(B)[:,None],x]=1
   m=1
   while m<=B:
    arrays[m][st,l]=np.sum((counts+c['modeling']['token_tile']-1)//c['modeling']['token_tile'],axis=1)
    if m<B:counts=counts[0::2]+counts[1::2]
    m*=2
  if st%32==0:print(f'routing {st}/{S}, {time.time()-t:.1f}s',flush=True)
 for a in arrays.values():a.flush()
 dump(out/'routing.json',{'seed':w['seed'],'route_sha256':digest.hexdigest(),'assignments':S*(L-1)*B*K,'requests':B,'steps':S,'layers':92,'top_k':K,'expert_count':E,'sampling':'ordered uniform rejection without replacement; same prefix requests across all conditions','saved':'exact weight-tile counts by step/layer/microbatch; original assignments regenerated from seed and NumPy version','numpy':np.__version__})
 return arrays

def validation(c,model):
 parts={};admission=[]
 for n in c['workload']['chips']:
  bounds,stages=model.partition(n);parts[str(n)]={'bounds':bounds,'stages':stages}
  for mode in c['workload'].get('modes',('TP','PP')):
   for B in c['workload']['batches']:
    caps=model.capacity(n,B,mode,bounds);admission.append({'mode':mode,'chips':n,'batch':B,'max_bytes':max(x['total_bytes'] for x in caps),'admitted':all(x['total_bytes']<=c['hardware']['memory_bytes'] for x in caps),'cards':caps})
 return {'backend':c['modeling'].get('backend','message'),'partitions':parts,'capacity':admission}

def plan_costs(model,m,n,mode,tiles,bounds):
 S=model.w['steps'];G=tiles.shape[2];ranks=4*n if mode=='TP' else 4
 emb,head=model.endpoints(m);up=model.up(m)
 if mode=='TP':cost=np.zeros((S,G,279));metrics=np.zeros((S,G,n,6));payload=[];seg=0
 else:cost=np.zeros((S,G,n));metrics=np.zeros((S,G,n,6));payload=[m*model.H*2*(2+math.ceil(bounds[i+1]/12)) if i<n-1 else m*4 for i in range(n)]
 for l in range(model.L):
  a=model.layer(l,m,ranks,tiles[:,l,:]);ns=2 if l==0 else 3
  if mode=='TP':
   if l==0:a[:,:,0,:]+=emb
   elif l>1:a[:,:,0,:]+=up
   # Layer 1 (0-based) follows dense: no latent-up in its predecessor.
   cost[:,:,seg:seg+ns]=a[:,:,:ns,0]
   for d in ([model.H,model.H] if l==0 else [model.H,model.H,model.hf['routed_expert_hidden_size']]):payload.append(m*d*2)
   metrics+=a[:,:,:ns,:].sum(axis=2)[:,:,None,:];seg+=ns
  else:
   s=next(i for i in range(n) if bounds[i]<=l<bounds[i+1]);v=a[:,:,:ns,:].sum(axis=2)+(up if l else 0)
   cost[:,:,s]+=v[:,:,0];metrics[:,:,s]+=v
 if mode=='TP':
  tail=head+up;cost[:,:,-1]=tail[0];metrics+=tail[None,None,None,:];payload.append(0)
 else:
  cost[:,:,0]+=emb[0];metrics[:,:,0]+=emb;cost[:,:,-1]+=head[0];metrics[:,:,-1]+=head
  # Stage boundaries stripe from four sending dies; receiving dies all-gather state.
  for s in range(n):
   if s>0:
    by=payload[s-1]/4;dt=model.d2d(payload[s-1],3);v=np.array([by/model.bw+dt,0,by/model.bw,dt,by,0]);cost[:,:,s]+=v[0];metrics[:,:,s]+=v
   if s<n-1:
    by=payload[s]/4;v=np.array([by/model.bw,0,by/model.bw,0,by,0]);cost[:,:,s]+=v[0];metrics[:,:,s]+=v
 return cost,metrics,payload

def execute(case):
 p=pathlib.Path(case['path']);t=time.time()
 try:
  with open(p/'stdout.log','w') as o,open(p/'stderr.log','w') as e:subprocess.run([str(BIN),str(p/'input.txt'),str(p/'engine')],stdout=o,stderr=e,check=True,timeout=case['timeout'],env={**os.environ,**{'HM_SUE_'+k.upper():str(v) for k,v in case.get('sue_profile',{}).items()}})
  x=json.loads((p/'engine.json').read_text());end=x['makespan_ps']/1e12
  assert x['finished_tokens']==case['batch']*case['steps']
  assert x['topology']==case['topology']
  if case['topology']=='single_switch':assert x['switch_forwarded_messages']==x['hop_messages']
  assert abs(sum(x['chip_tx_bytes'])-sum(x['chip_rx_bytes']))<=max(1,sum(x['chip_tx_bytes'])*1e-10)
  budget=np.array(x['network_budget_busy_ps'])/(x['makespan_ps']*(2 if case['duplex']=='independent' else 1));assert max(budget)<1.000001
  local=np.array(case['resource_totals']);assert np.all(local[:,1]<=end+1e-7) and np.all(local[:,2]<=end+1e-7)
  r={**case,'status':'completed','wall_s':time.time()-t,'tpot_ms':x['mean_tpot_ps']/1e9,'p50_ms':x['p50_tpot_ps']/1e9,'p95_ms':x['p95_tpot_ps']/1e9,'first_decode_ms':x['mean_first_decode_ps']/1e9,'makespan_s':end,'throughput':case['batch']*case['steps']/end,'per_chip_throughput':case['batch']*case['steps']/end/case['chips'],'compute_util':float(local[:,1].mean()/end),'hbm_util':float(local[:,2].mean()/end),'d2d_util':float(local[:,3].mean()/end),'network_util':float(budget.mean()),'hbm_bytes':float(local[:,4].sum()*4),'flops':float(local[:,5].sum()*4),'stage_util':[b/x['makespan_ps'] for b in x['stage_busy_ps']],'stage_queue_ms':[b/1e9 for b in x['stage_queue_ps']],'network_competition_ms_sum':x['network_competition_ps_sum']/1e9,'network_fixed_ms_sum':x['network_propagation_ps_sum']/1e9,'network_payload_ms_sum':x['network_serial_ps_sum']/1e9,'tx_bytes':x['chip_tx_bytes'],'rx_bytes':x['chip_rx_bytes'],'engine_events_ar_steps':x['ar_steps']}
  if case['mode']=='TP':assert x['ar_steps']==278*2*(case['chips']-1)*case['steps']
  if case.get('backend')=='sue':
   with (p/'engine-sue-ports.csv').open() as f:ports=list(csv.DictReader(f))
   r['sue_counters']={k:sum(int(row[k]) for row in ports) for k in ('tx_ok','replay_frames','discard','credit_exhausted','queue_blocked_wakeups')}
   assert r['sue_counters']['discard']==0
   assert r['sue_counters']['tx_ok']>0
   r['sue_timing']=json.loads((p/'engine-sue-profile.json').read_text())
   assert r['sue_timing']['fixed_path_ps']==int(float((p/'input.txt').read_text().split()[8]))
  dump(p/'result.json',r);return r
 except Exception as ex:
  r={**case,'status':'failed','error':str(ex),'wall_s':time.time()-t};dump(p/'result.json',r);return r

def run(c,model,config,out):
 out.mkdir(parents=True,exist_ok=False);(out/'conditions').mkdir();(out/'routes').mkdir()
 dump(out/'experiment.json',c);val=validation(c,model);dump(out/'validation.json',val)
 impl={'files':{str(p.relative_to(ROOT)):sha(p) for p in HERE.glob('*') if p.is_file()},'input_files':{str(p):sha(p) for p in (config,config.parent/c['model_config'],config.parent/c['weight_inventory'])},'numpy':np.__version__,'command':sys.argv,'git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'branch':subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip()}
 dump(out/'implementation.json',impl)
 import shutil
 (out/'source_snapshot').mkdir()
 for f in HERE.glob('*'):
  if f.is_file():shutil.copy2(f,out/'source_snapshot'/f.name)
 arrays=routes(c,out/'routes');results=[];futures=[];jobs=[]
 def submit(mode,n,B,m,cost,metrics,payload,bounds,duplex,variant='base'):
  ident=f'{mode}{n}-b{B}-m{m}-{duplex}-{variant}';p=out/'conditions'/ident;p.mkdir()
  cap=model.capacity(n,B,mode,bounds);case={'id':ident,'mode':mode,'chips':n,'batch':B,'microbatch':m,'duplex':duplex,'variant':variant,'steps':c['workload']['steps'],'topology':c['modeling'].get('topology','ring'),'backend':c['modeling'].get('backend','message'),'sue_profile':c['modeling'].get('sue_profile',{}),'path':str(p.resolve()),'timeout':c['execution']['case_timeout_s'],'capacity':cap,'partition':bounds}
  if any(x['total_bytes']>model.h['memory_bytes'] for x in cap):
   r={**case,'status':'capacity_rejected'};dump(p/'result.json',r);results.append(r);return
  G=B//m;cost=np.ascontiguousarray(cost[:,:G,:]*1e12,dtype='<f8');cost.tofile(p/'service.bin')
  totals=metrics[:,:G,:,:].sum(axis=(0,1));case['resource_totals']=totals.tolist();case['input_sha256']=sha(p/'service.bin');case['service_shape']=list(cost.shape);case['stage_mean_service_ms']=(cost.mean(axis=(0,1))/1e9).tolist() if mode=='PP' else []
  bandwidth=c['hardware']['scaleup_bytes_per_s']*(2 if variant=='bandwidth_x2' else 1);alpha=c['hardware']['scaleup_hop_ns']*(.5 if variant=='latency_half' else 1)
  text=f'{mode} {n} {B} {m} {c["workload"]["steps"]} {cost.shape[2]} {int(duplex=="independent")} {bandwidth} {int(alpha*1000)} {int(c["hardware"]["notification_ns"]*1000)} {p.resolve()}/service.bin\n'+' '.join(map(str,payload))+'\n'+c['modeling'].get('topology','ring')+'\n';(p/'input.txt').write_text(text);dump(p/'case.json',case);jobs.append(case);futures.append(pool.submit(execute,case))
 with concurrent.futures.ThreadPoolExecutor(max_workers=c['execution']['workers']) as pool:
  for mode in c['workload'].get('modes',('TP','PP')):
   for n in c['workload']['chips']:
    bounds=val['partitions'][str(n)]['bounds']
    for m in c['workload']['batches']:
     # TP has one global batch. PP uses all power-of-two microbatch divisors.
     tt=arrays[m][:,:,:1] if mode=='TP' else arrays[m];cost,metrics,payload=plan_costs(model,m,n,mode,tt,bounds)
     for B in ([m] if mode=='TP' else [b for b in c['workload']['batches'] if b>=m]):
      for duplex in ('sum','independent'):submit(mode,n,B,m,cost,metrics,payload,bounds,duplex)
      if B in c['sensitivity']['batches']:
       for v in c['sensitivity']['variants']:submit(mode,n,B,m,cost,metrics,payload,bounds,'sum',v)
     print(f'planned {mode}{n} m={m}; launched {len(jobs)}; rejected {len(results)}',flush=True)
     del cost,metrics
  for i,f in enumerate(concurrent.futures.as_completed(futures)):
   r=f.result();results.append(r)
   if i%20==0:print(f'completed {i+1}/{len(futures)}: {r["id"]} {r["status"]}',flush=True)
   with open(out/'ledger.jsonl','a') as ledger:ledger.write(json.dumps(r,ensure_ascii=False)+'\n')
 results.sort(key=lambda x:x['id']);dump(out/'results.json',results)
 fields=['id','status','mode','chips','batch','microbatch','duplex','variant','tpot_ms','p50_ms','p95_ms','first_decode_ms','makespan_s','throughput','per_chip_throughput','compute_util','hbm_util','d2d_util','network_util','hbm_bytes','flops','wall_s']
 with open(out/'summary.csv','w') as f:
  w=csv.DictWriter(f,fields,extrasaction='ignore');w.writeheader();w.writerows(results)
 counts={s:sum(x['status']==s for x in results) for s in ('completed','capacity_rejected','failed')}
 dump(out/'verification.json',{'planned':len(results),**counts,'all_requested_points_accounted':True,'token_conservation':counts['failed']==0,'network_budget_and_bytes':counts['failed']==0,'ar_step_count':counts['failed']==0,'resolution':'1ps','notification_interval':'1ns','backend':c['modeling'].get('backend','message'),'not_modeled':(['SUE AFH/LLR/CBFC packets'] if c['modeling'].get('backend','message')!='sue' else [])+['real kernel efficiency curves','inter-operator speculative overlap','nonuniform routing','hardware calibrated D2D latency'],'sources':impl['input_files']})
 print(json.dumps(counts),flush=True)
 if counts['failed']:raise SystemExit(1)

def main():
 p=argparse.ArgumentParser();p.add_argument('command',choices=['build','check','run']);p.add_argument('--config',type=pathlib.Path,default=ROOT/'configs/experiments/hm-emu/study.json');p.add_argument('--output',type=pathlib.Path);a=p.parse_args()
 if a.command=='build':
  cfg=json.loads(a.config.read_text());build(cfg['modeling'].get('backend','message'));return
 c,m=load(a.config.resolve());v=validation(c,m)
 if a.command=='check':print(json.dumps(v,indent=2));return
 if not a.output:p.error('--output required')
 build(c["modeling"].get("backend","message"));run(c,m,a.config.resolve(),a.output.resolve())
if __name__=='__main__':main()
