"""Two TP8-chip groups with two disjoint expert partitions; actual SUE packet DAG.
All groups synchronize at layer phases. This conservative bulk-synchronous policy
is explicit; we do not assume dispatch/compute overlap or asynchronous serving.
"""
import argparse,concurrent.futures,csv,hashlib,json,math,pathlib,shutil,subprocess,time
import numpy as np
import run

def vec(model,by=0,fl=0,dt=0,nt=0):
 t,c,h=model.op(by,fl);return np.array([t+dt+nt,c+nt,h,dt,by,fl],dtype=float)

def route_stream(c,out):
 S=c['workload']['steps'];B=max(c['workload']['batches']);rng=np.random.default_rng(c['workload']['seed']);a=np.zeros((S,93,B,16),dtype=np.uint16);digest=hashlib.sha256()
 for st in range(S):
  for l in range(1,93):
   x=rng.integers(896,size=(B,16),dtype=np.uint16)
   for j in range(1,16):
    bad=np.any(x[:,:j]==x[:,j,None],axis=1)
    while bad.any():x[bad,j]=rng.integers(896,size=int(bad.sum()),dtype=np.uint16);bad=np.any(x[:,:j]==x[:,j,None],axis=1)
   a[st,l]=x;digest.update(x.tobytes())
 np.save(out/'expert-assignments.npy',a)
 run.dump(out/'routing.json',{'route_sha256':digest.hexdigest(),'assignments':S*92*B*16,'seed':c['workload']['seed'],'requests':B,'steps':S,'layers':92,'top_k':16,'expert_count':896,'numpy':np.__version__,'placement':'expert id <448 in group0, else group1; token origin=index%2'})
 return a

def capacity(model,B):
 result=[];saved=4*92*(896//2)*model.expert/32
 for group in range(2):
  local=(B+1-group)//2
  rows=model.capacity(8,local,'TP',model.partition(8)[0])
  for r in rows:
   r['card']+=group*8;r['weights_bytes']-=saved;r['buffer_bytes']+=4*2*B*(model.hf['routed_expert_hidden_size']*2+8+16*8)
   r['total_bytes']=sum(r[k] for k in ('weights_bytes','state_bytes','workspace_bytes','buffer_bytes'));result.append(r)
 return result

class Plan:
 def __init__(self,model,B,assignments):
  self.model=model;self.B=B;self.S=model.w['steps'];self.counts=[(B+1)//2,B//2];self.tasks=[];self.phases=[];self.prev=None;self.metrics=np.zeros((self.S,16,6));self.costs=np.zeros((self.S,1,1));self.routing=[]
  self.assignments=assignments
 def node(self,kind,src=0,dst=0,st=0,ps=0,by=0,label='-',deps=None):
  ident=len(self.tasks);deps=[] if deps is None else sorted(set(deps));self.tasks.append((ident,kind,src,dst,st,int(ps),int(by),label,','.join(map(str,deps)) or '-'));return ident
 def deps(self):return [] if self.prev is None else [self.prev]
 def local(self,st,label,vs):
  start=self.prev;ids=[]
  for g,v in enumerate(vs):
   ids.append(self.node('C',src=g,st=st,ps=math.ceil(v[0]*1e12),label=label,deps=self.deps()))
   self.metrics[st,g*8:(g+1)*8]+=v
  self.prev=self.node('B',st=st,label=label,deps=ids);critical=vs[int(np.argmax([v[0] for v in vs]))]
  self.costs[st,0,0]+=math.ceil(critical[0]*1e12)
  self.phases.append({'end_id':self.prev,'start_id':start,'step':st,'label':label,'kind':'local','critical_resources':critical.tolist()})
 def ring(self,st,label,width):
  start=self.prev;ends=[]
  for g,m in enumerate(self.counts):
   deps=self.deps()
   if not m:continue
   payload=m*width*2//8
   for k in range(14):
    ids=[self.node('N',src=g*8+c,dst=g*8+(c+1)%8,st=st,by=payload,label=label,deps=deps) for c in range(8)]
    deps=[self.node('B',st=st,label=label,deps=ids)]
   ends+=deps
  self.prev=self.node('B',st=st,label=label,deps=ends)
  self.phases.append({'end_id':self.prev,'start_id':start,'step':st,'label':label,'kind':'network','fixed_steps':14})
 def exchange(self,st,label,per_die):
  start=self.prev;ids=[]
  for g,by in enumerate(per_die):
   if by:ids.extend(self.node('N',src=g*8+c,dst=(1-g)*8+c,st=st,by=4*by,label=label,deps=self.deps()) for c in range(8))
  self.prev=self.node('B',st=st,label=label,deps=ids or self.deps())
  self.phases.append({'end_id':self.prev,'start_id':start,'step':st,'label':label,'kind':'network','fixed_steps':int(bool(ids))})
 def build(self):
  M=self.model;latent=M.hf['routed_expert_hidden_size'];zero=np.zeros(6);cache={}
  for l in range(93):
   for g,m in enumerate(self.counts):
    if m:cache[l,g]=M.layer(l,m,32,np.zeros((self.S,1)))[:,0]
  for st in range(self.S):
   for l in range(93):
    values=[cache[l,g][st].copy() if m else np.zeros((3,6)) for g,m in enumerate(self.counts)]
    for g,m in enumerate(self.counts):
     if not m:continue
     if l==0:values[g][0]+=M.endpoints(m)[0]
     elif l>1:values[g][0]+=M.up(m)
    self.local(st,f'L{l}.attention',[v[0] for v in values]);self.ring(st,f'L{l}.attention_AR',M.H)
    self.local(st,f'L{l}.shared',[v[1] for v in values]);self.ring(st,f'L{l}.shared_AR',M.H)
    if not l:continue
    router=[]
    for g,m in enumerate(self.counts):
     if not m:router.append(zero.copy());continue
     dummy=vec(M,4*m*M.K*latent/32,2*m*M.K*M.expert_params/32,dt=M.d2d(m*latent*2))
     v=values[g][2]-dummy;assert (v>=-1e-7).all();router.append(np.maximum(v,0))
    self.local(st,f'L{l}.router',router)
    routes=self.assignments[st,l,:self.B];origin=np.arange(self.B)%2;owner=routes//448
    remote_counts=[int(np.sum(np.any(owner[origin==g]!=g,axis=1))) for g in range(2)]
    remote_assign=[int(np.sum(owner[origin==g]!=g)) for g in range(2)]
    dispatch=[remote_counts[g]*(2*latent+8)+8*remote_assign[g] for g in range(2)]
    counts=np.bincount(routes.ravel(),minlength=896);assigned=[int(counts[:448].sum()),int(counts[448:].sum())]
    assert sum(assigned)==self.B*16
    tiles=[int(np.sum((counts[:448]+31)//32)),int(np.sum((counts[448:]+31)//32))]
    self.routing.append({'step':st,'layer':l,'group0_assignments':assigned[0],'group1_assignments':assigned[1],'group0_tiles':tiles[0],'group1_tiles':tiles[1],'remote_tokens_0':remote_counts[0],'remote_tokens_1':remote_counts[1],'dispatch_per_die_0':dispatch[0],'dispatch_per_die_1':dispatch[1]})
    self.local(st,f'L{l}.dispatch_read',[vec(M,x) for x in dispatch]);self.exchange(st,f'L{l}.dispatch',dispatch)
    self.local(st,f'L{l}.dispatch_write',[vec(M,x) for x in dispatch[::-1]])
    self.local(st,f'L{l}.experts',[vec(M,tiles[g]*M.expert/32+4*assigned[g]*latent/32,2*assigned[g]*M.expert_params/32) for g in range(2)])
    combine=[remote_counts[1-g]*(2*latent+8) for g in range(2)]
    self.local(st,f'L{l}.combine_read',[vec(M,x) for x in combine]);self.exchange(st,f'L{l}.combine',combine)
    self.local(st,f'L{l}.combine_write',[vec(M,x) for x in combine[::-1]])
    self.local(st,f'L{l}.partial_sum',[vec(M,3*remote_counts[g]*latent*2,nt=remote_counts[g]*latent/M.a['nonmatrix_ops_per_s_per_die']) for g in range(2)])
    self.local(st,f'L{l}.routed_d2d',[vec(M,dt=M.d2d(m*latent*2)) if m else zero.copy() for m in self.counts]);self.ring(st,f'L{l}.routed_AR',latent)
   self.local(st,'output',[M.endpoints(m)[1]+M.up(m) if m else zero.copy() for m in self.counts])
   self.prev=self.node('T',st=st,label='token',deps=self.deps())
  return self

def main():
 p=argparse.ArgumentParser();p.add_argument('--config',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args();c,M=run.load(a.config.resolve());assert c['parallel']['tp_chips']==8 and c['parallel']['ep_groups']==2
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);(out/'conditions').mkdir();(out/'routes').mkdir();(out/'source_snapshot').mkdir();(out/'bin').mkdir()
 for f in run.HERE.glob('*'):
  if f.is_file():shutil.copy2(f,out/'source_snapshot'/f.name)
 run.dump(out/'experiment.json',c);assignments=route_stream(c,out/'routes')
 run.BIN=out/'bin/hm-emu-ep-engine'
 cmd=['g++','-std=c++17','-O3','-include','stdint.h',str(run.HERE/'ep_engine.cc'),'-I'+str(run.SIM/'build/include'),'-L'+str(run.SIM/'build/lib'),'-Wl,-rpath,'+str(run.SIM/'build/lib'),'-lns3.36.1-core-default','-lns3.36.1-sue-default','-lns3.36.1-network-default','-lns3.36.1-point-to-point-default','-o',str(run.BIN)];subprocess.run(cmd,check=True)
 run.dump(out/'implementation.json',{'files':{str(f.relative_to(run.ROOT)):run.sha(f) for f in run.HERE.glob('*') if f.is_file()},'input_files':{str(f):run.sha(f) for f in [a.config.resolve(),a.config.resolve().parent/c['model_config'],a.config.resolve().parent/c['weight_inventory']]},'engine_sha256':run.sha(run.BIN),'engine_relative_path':'bin/hm-emu-ep-engine','numpy':np.__version__,'command':cmd})
 rows=[];jobs=[];caps=[]
 for B in c['workload']['batches']:
  cap=capacity(M,B);admitted=max(x['total_bytes'] for x in cap)<=M.h['memory_bytes'];caps.append({'mode':'TP_EP','chips':16,'batch':B,'max_bytes':max(x['total_bytes'] for x in cap),'admitted':admitted,'cards':cap})
  plan=Plan(M,B,assignments).build() if admitted else None
  pairs=[('sum','base'),('independent','base')]+([('sum',v) for v in c['sensitivity']['variants']] if B in c['sensitivity']['batches'] else [])
  for duplex,variant in pairs:
   ident=f'TP8_EP2-b{B}-m{B}-{duplex}-{variant}';d=out/'conditions'/ident;d.mkdir()
   case={'id':ident,'mode':'TP_EP','chips':16,'batch':B,'microbatch':B,'duplex':duplex,'variant':variant,'steps':M.w['steps'],'topology':'single_switch','backend':'sue','sue_profile':c['modeling']['sue_profile'],'path':str(d),'timeout':c['execution']['case_timeout_s'],'capacity':cap,'partition':[0,93],'tp_chips':8,'ep_groups':2,'tp_group_batch_counts':[(B+1)//2,B//2]}
   if not admitted:case['status']='capacity_rejected';run.dump(d/'result.json',case);rows.append(case);continue
   plan.costs.astype('<f8').tofile(d/'service.bin');(d/'dag.tsv').write_text('\n'.join(' '.join(map(str,t)) for t in plan.tasks)+'\n');run.dump(d/'phases.json',plan.phases);run.dump(d/'expert-assignment-counts.json',plan.routing)
   case.update(resource_totals=plan.metrics.sum(axis=0).tolist(),input_sha256=run.sha(d/'service.bin'),dag_sha256=run.sha(d/'dag.tsv'),service_shape=list(plan.costs.shape),stage_mean_service_ms=[])
   bw=M.h['scaleup_bytes_per_s']*(2 if variant=='bandwidth_x2' else 1);alpha=M.h['scaleup_hop_ns']*(.5 if variant=='latency_half' else 1)
   (d/'input.txt').write_text(f'EP 16 {B} {B} {M.w["steps"]} 1 {int(duplex=="independent")} {bw} {int(alpha*1000)} 1000 {d}/service.bin\n0\nsingle_switch\n{d}/dag.tsv\n');run.dump(d/'case.json',case);jobs.append(case)
  print('planned EP B=',B,flush=True)
 run.dump(out/'validation.json',{'backend':'sue','partitions':{},'capacity':caps,'parallel':c['parallel']})
 with concurrent.futures.ThreadPoolExecutor(max_workers=c['execution']['workers']) as pool:
  for r in pool.map(run.execute,jobs):
   rows.append(r)
   with (out/'ledger.jsonl').open('a') as f:f.write(json.dumps(r)+'\n')
   print(r['id'],r['status'],r.get('tpot_ms',r.get('error')),flush=True)
 rows.sort(key=lambda x:x['id']);run.dump(out/'results.json',rows);counts={k:sum(r['status']==k for r in rows) for k in ('completed','capacity_rejected','failed')};run.dump(out/'verification.json',{'planned':len(rows),**counts,'backend':'sue','all_requested_points_accounted':True,'token_conservation':counts['failed']==0,'resolution':'1ps','notification_interval':'1ns'})
 with (out/'summary.csv').open('w') as f:
  w=csv.DictWriter(f,['id','mode','chips','batch','duplex','variant','status','tpot_ms','throughput'],extrasaction='ignore');w.writeheader();w.writerows(rows)
 if counts['failed']:raise SystemExit(1)
 print(counts,flush=True)
if __name__=='__main__':main()
