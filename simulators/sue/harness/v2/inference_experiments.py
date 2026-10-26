#!/usr/bin/env python3
"""Resource-coupled SUE experiments, with honest representative-block boundaries."""
import json,math,random,sys,time,traceback,shutil
from pathlib import Path
from collections import Counter,defaultdict
from concurrent.futures import ThreadPoolExecutor,as_completed
from workloads import task
from parallel_config import from_definition,token_group,expert_owner,layout_map
from moe_traffic import die_traffic
from model_config import load_model,save_model
from hardware_config import HARDWARE_SOURCE,network_settings,save_hardware
from run import create,run_case,dump,csvwrite,csvread,sha,SOURCE,BINARY,HERE
from inference_profile import HW,NAMES,profile,budget,best_tp,kv_budget

SETTINGS=dict(payload=8192,interval_ns=10,stop_ns=100000000,ring_gbps=2000,ring_bytes=1048576,queue_bytes=262144,timeout_s=90)
class DAG:
 def __init__(self,hw):self.tasks=[];self.resources=[];self.hw=hw
 def add(self,row):row['id']=len(self.tasks);self.tasks.append(row);return row['id']
 def local(self,c,d,kind,units,deps,label,dtype=None,peer=-1,service_ns=None):
  deps=sorted(set(deps));i=self.add(task(c,d,c,d,0,delay=1000 if not deps else 0,deps=deps,label=label))
  units=max(1,math.ceil(units))
  if service_ns is None:
   rate=self.hw[dtype+'_flops_s_per_die']*self.hw['compute_efficiency'] if kind==0 else self.hw['hbm_bytes_s_per_die']*self.hw['hbm_efficiency']
   service_ns=0 if kind==3 else max(1,math.ceil(units/rate*1e9))
  self.resources.append(dict(id=i,kind=kind,units=units,service_ns=service_ns,peer_die=peer));return i
 def transfer(self,a,b,n,deps,label):
  if a==b:return list(deps)
  n=max(1,math.ceil(n));deps=sorted(set(deps))
  if a[0]==b[0]:return [self.local(*a,3,n,deps,label,peer=b[1])]
  return [self.add(task(*a,*b,n,delay=1000 if not deps else 0,deps=deps,label=label))]
 def remote_read(self,sa,mpu,n,deps,label,rows=0):
  deps=self.transfer(sa,(mpu,0),64+rows*8,deps,label+':request')
  latency=self.hw['mpu_fixed_ns']*(math.ceil(rows/16) if rows else 1)
  ns=latency+math.ceil(n/(self.hw['mpu_bytes_s']*self.hw['mpu_efficiency'])*1e9)
  svc=self.local(mpu,0,2,n,deps,label+':mpu-service',service_ns=ns)
  data=self.transfer((mpu,0),sa,n,[svc],label+':data-response')
  return [self.local(*sa,1,n,data,label+':hbm-write')]
 def allreduce(self,ranks,tp,count_by_group,dep_by_rank,label,width,gather_only=False):
  prev={r:list(dep_by_rank.get(r,[])) for r in range(ranks)}
  for hop in range((tp-1) if gather_only else 2*(tp-1)):
   nxt=defaultdict(list)
   for r in range(ranks):
    g=r//tp;count=count_by_group.get(g,0)
    if not count:continue
    to=g*tp+(r%tp+1)%tp
    deps=prev[r]
    ids=self.transfer((r//4,r%4),(to//4,to%4),math.ceil(count*width*2/tp),deps,f'{label}:hop{hop}')
    nxt[to].extend(ids);nxt[r].extend(ids) # no reuse of a send buffer until transfer completes
   prev=nxt
  return prev

def model_block(d,offload=None):
 m=profile(d['model'],d.get('model_input'));n=d['chips'];ranks=n*4;tp=d['tp'];G=ranks//tp;B=d['batch'];L=d['context'];seed=d['seed'];M=d.get('mpus',0);fraction=d.get('expert_remote_fraction',0)
 parallel=from_definition(d);policy=parallel['moe_transport']
 if policy not in ('tp-striped','leader'):raise ValueError('unknown moe_transport: '+policy)
 assert ranks%tp==0 and m['width']%tp==0 and m['hidden']%tp==0
 hw={**HW,**d.get('hardware_overrides',{})};dag=offload.dag if offload is not None else DAG(hw);rng=random.Random(seed);routes=[];prior=[];previous={}
 for step in range(d.get('steps',2)):
  counts=Counter(token_group(token,B,G,parallel['token_assignment']) for token in range(B));ready={}
  extra_ready={}
  if d.get('include_engram'):
   for g,count in counts.items():
    sa=(g*tp//4,g*tp%4);eg=[]
    if offload is not None and offload.engram:
     eg=offload.read_engram(sa,count,prior,f'{step}:engram')
    elif d.get('engram_remote'):
     for bank in range(M):
      rows=(count*m['engram_rows_per_token']+M-1-bank)//M
      if rows:eg+=dag.remote_read(sa,n+bank,rows*m['engram_row_bytes'],prior,f'{step}:engram',rows=rows)
    else:eg=[dag.local(*sa,1,count*m['engram_lookup_bytes_per_token'],prior,f'{step}:engram-local')]
    extra_ready[g]=eg
  _,kvread,_=kv_budget(m,L+step)
  if d.get('parameter_chunk_only'):kvread=0 # no attention/KV work in this explicitly isolated microbenchmark
  for r in range(ranks):
   b=counts[r//tp]
   if not b:continue
   sa=(r//4,r%4);ops=2*m['common_active_params']/m['layers']*b/tp
   kv_mem=kvread*b/m['layers']/tp
   remote_kv=[]
   base=prior+extra_ready.get(r//tp,[])
   if offload is not None and offload.kv:
    kv_mem,remote_kv=offload.read_kv(sa,b,tp,L+step,base,f'{step}:kv')
   mem=m['common_active_params']*m['common_weight_bytes']/m['layers']/tp+kv_mem
   ready[r]=[dag.local(*sa,0,ops,base,f'{step}:common-compute',m['common_dtype']),dag.local(*sa,1,mem,base,f'{step}:common-hbm-kv')]+remote_kv
  ready=dag.allreduce(ranks,tp,counts,ready,f'{step}:attention-allreduce',m['hidden'])
  traffic=Counter();selected=defaultdict(Counter);expert_deps=defaultdict(list);sources=defaultdict(set)
  for token in range(B):
   group=token_group(token,B,G,parallel['token_assignment']);src=group*tp;chosen=previous.get(token) if step and rng.random()<parallel['decode_expert_reuse_probability'] else None
   if chosen is None:chosen=sorted(rng.sample(range(m['experts']),m['top_k']))
   previous[token]=chosen
   for e in chosen:
    owner=expert_owner(e,src,parallel,m['experts']);selected[owner][e]+=1
    senders=range(src,src+tp) if policy=='tp-striped' else (src,)
    for sender in senders:traffic[sender,owner]+=1;sources[owner].add(sender)
    routes.append(dict(step=step,token=token,expert=e,source_rank=-1 if policy=='tp-striped' else src,source_group=group,owner_rank=owner,moe_transport=policy))
  # Replicated TP activations are sliced by feature. Each fragment carries its
  # own 8B logical metadata. No duplicate full tensors or free cross-Die reads.
  shard_width=m['width']//tp if policy=='tp-striped' else m['width']
  for (src,dst),count in sorted(traffic.items()):
   expert_deps[dst]+=dag.transfer((src//4,src%4),(dst//4,dst%4),count*(shard_width*2+8),ready[src],f'{step}:dispatch')
  expert_ready={}
  for r,experts in sorted(selected.items()):
   sa=(r//4,r%4);deps=expert_deps[r];remote=[]
   for e in experts:
    if fraction and (e%100)<round(fraction*100):
     remote+=dag.remote_read(sa,n+e%M,math.ceil(m['expert_params']*m['expert_weight_bytes']),deps,f'{step}:expert{e}:remote')
   memory=len(experts)*m['expert_params']*m['expert_weight_bytes']+sum(experts.values())*m['width']*4
   ops=2*m['expert_params']*sum(experts.values())
   deps=deps+remote
   expert_ready[r]=[dag.local(*sa,0,ops,deps,f'{step}:expert-compute',m['expert_dtype']),dag.local(*sa,1,memory,deps,f'{step}:expert-hbm')]
  combined=defaultdict(list)
  for (src,dst),count in sorted(traffic.items()):combined[src]+=dag.transfer((dst//4,dst%4),(src//4,src%4),count*(shard_width*2+8),expert_ready[dst],f'{step}:combine')
  if policy=='tp-striped':
   # Each rank owns one result feature shard, sums top-k contributions, then
   # allgathers the reduced shards. Expert GEMM still waits for all dispatch
   # fragments, and the output collective waits for each rank's local reduction.
   ready={}
   for r in range(ranks):
    count=counts[r//tp]
    if not count:continue
    deps=combined[r];elements=count*shard_width
    ready[r]=[dag.local(r//4,r%4,0,elements*m['top_k']*2,deps,f'{step}:combine-reduce','fp16'),
              dag.local(r//4,r%4,1,elements*2*(m['top_k']+1),deps,f'{step}:combine-hbm')]
   done=dag.allreduce(ranks,tp,counts,ready,f'{step}:output-allgather',m['width'],gather_only=True)
  else:
   ready={r:combined.get((r//tp)*tp,[]) for r in range(ranks)}
   done=dag.allreduce(ranks,tp,counts,ready,f'{step}:output-allreduce',m['hidden'])
  prior=sorted({i for ds in done.values() for i in ds})

 cap_batch=d.get('capacity_sequences',B)
 scope=('prefill-style parameter-block chunk; attention arithmetic/KV traffic excluded; ' if d.get('parameter_chunk_only') else 'representative MoE block + 1/L common/attention memory costs; ')+('plus configured Engram lookup per token; ' if d.get('include_engram') else '')+'not full-model TTFT/TPOT'
 return dag,routes,dict(model=m['name'],window='resource-coupled-representative-block',moe_transport=policy,parallel=parallel,parallel_layout=layout_map(n,parallel,m['experts']),scope=scope,definition=d,hardware=hw,model_profile={k:v for k,v in m.items() if k!='config'},capacity=budget(m,n,tp,cap_batch,L,M,d.get('engram_remote',False),fraction,hw,parallel=parallel))

def engram_phase(d):
 m=profile(d['model'],d.get('model_input'));hw={**HW,**d.get('hardware_overrides',{})};dag=DAG(hw);n=d['chips'];M=d['mpus'];B=d['batch'];rng=random.Random(d['seed']);rows=Counter()
 # 2 tables, 3 n-gram orders, 8 heads; synthetic uniformly striped hash rows.
 for token in range(B):
  rank=token%(n*4)
  for _ in range(m['engram_rows_per_token']):rows[rank,rng.randrange(max(1,M))]+=1
 for (rank,bank),count in rows.items():
  sa=(rank//4,rank%4);nbytes=count*m['engram_row_bytes']
  if M:dag.remote_read(sa,n+bank,nbytes,[],'engram',rows=count)
  else:dag.local(*sa,1,nbytes,[],'engram-local-hbm')
 return dag,[],dict(model=m['name'],window='engram-lookup-phase',scope='configured Engram rows per token; excludes embedding projection and the remaining model',definition=d,hardware=hw,capacity=budget(m,n,d['tp'],B,d['context'],M,bool(M),hw=hw))

def definitions():
 out=[]
 for name in NAMES:
  m=profile(name)
  for n in (m['base_chips'],m['base_chips']*2):
   # Keep TP fixed within each chip-count sweep; choose enough capacity at largest batch/context.
   tp=best_tp(m,n,64,131072)
   for B in (1,8,32,64):
    for L in (8192,131072):
     for seed in (1,2,3):out.append(dict(study='deployment',model=name,chips=n,tp=tp,batch=B,context=L,mpus=0,seed=seed,steps=2))
  n=m['base_chips'] if not name.startswith('deepseek') else 8
  tp=best_tp(m,n,32,32768)
  for f in (0,.01,.05):
   for seed in (1,2,3):out.append(dict(study='expert_streaming',model=name,chips=n,tp=tp,batch=32,context=32768,mpus=4,seed=seed,steps=1,expert_remote_fraction=f))
 name=NAMES[2];m=profile(name)
 for B in (1,8,32,64):
  for M in (0,2,4):
   for seed in (1,2,3):out.append(dict(study='engram',model=name,chips=4,tp=4,batch=B,context=32768,mpus=M,seed=seed))
 for eff in (.1,.8):
  for latency in (200,1000):
   for seed in (1,2,3):out.append(dict(study='engram_sensitivity',model=name,chips=4,tp=4,batch=32,context=32768,mpus=2,seed=seed,hardware_overrides={'mpu_efficiency':eff,'mpu_fixed_ns':latency}))
 for n,M in ((4,2),(4,4),(8,0)):
  for B in (1,8,32,64):
   for seed in (1,2,3):out.append(dict(study='deepseek_placement',model=name,chips=n,tp=4,batch=B,context=32768,mpus=M,seed=seed,steps=2,engram_remote=bool(M),include_engram=True))
 for name,tp in ((NAMES[0],64),(NAMES[1],32)):
  m=profile(name)
  for n in (m['base_chips'],m['base_chips']*2):
   for seed in (1,2,3):out.append(dict(study='matched_tp_scale',model=name,chips=n,tp=tp,batch=32,context=8192,mpus=0,seed=seed,steps=2))
 return out

def execute(root,d,i):
 d={**d,'model_input':d.get('model_input') or load_model(name=d['model'])}
 m=profile(d['model'],d.get('model_input'));hw={**HW,**d.get('hardware_overrides',{})};engram=d['study'].startswith('engram')
 parallel=from_definition(d)
 if m['width']%parallel['tp'] or m['hidden']%parallel['tp']:raise ValueError('model dimensions must divide TP without padded shards')
 cap=budget(m,d['chips'],d['tp'],d.get('capacity_sequences',d['batch']),d['context'],d['mpus'],d.get('engram_remote',engram and bool(d['mpus'])),d.get('expert_remote_fraction',0),hw,parallel=parallel)
 # Engram-only probes are allowed even when the full local model cannot fit: explicitly mark this.
 if not cap['admitted'] and not engram:return dict(**d,status='capacity_rejected',capacity=cap,scope='no packet simulation: full deployment does not fit reserved local/remote capacity')
 dag,routes,meta=engram_phase(d) if engram else model_block(d)
 meta['hardware_source']=HARDWARE_SOURCE
 meta['parallel']=parallel
 meta['parallel_layout']=layout_map(d['chips'],parallel,m['experts'])
 case=create(root/'cases'/f'{i:03d}-{d["study"]}-{d["model"]}',f'hw-{d["chips"]}-{d["mpus"]}',dag.tasks,meta,settings={**SETTINGS,**network_settings(hw),'seed':d['seed'],'port_sample_ns':d.get('port_sample_ns',0)},routes=routes,resources=dag.resources)
 dump(case/'parallel.json',parallel);dump(case/'parallel-layout.json',meta['parallel_layout'])
 save_model(case,d['model_input'])
 result=run_case(case);result.update(d);result['moe_transport']=meta.get('moe_transport','not-applicable');result['parallel']=parallel;result['capacity']=cap;result['scope']=meta['scope'];result['tasks_sha256']=sha(case/'tasks.json');result['resources_sha256']=sha(case/'resources.json')
 if result['status']=='passed':
  rows=csvread(Path(result['run'])/'task_statistics.csv');result['window_us']=(max(int(r['end_ps']) for r in rows)-1000000)/1e6
  result['per_step_block_us']=result['window_us']/d.get('steps',1)
  if not engram:
   flows=die_traffic(dag,rows,d['chips']*4,result['window_us']);csvwrite(Path(result['run'])/'moe_die_traffic.csv',flows)
   result['moe_dispatch_active_dies']=sum(r['fabric_tx_bytes']>0 for r in flows if r['phase']=='dispatch')
   result['moe_combine_active_dies']=sum(r['fabric_tx_bytes']>0 for r in flows if r['phase']=='combine')
 return result

def main(root,workers=3):
 root=Path(root).resolve();root.mkdir(exist_ok=True);defs=definitions();assert not (root/'results.json').exists(),'use a fresh matrix directory'
 save_hardware(root)
 dump(root/'design.json',defs);dump(root/'hardware.json',HW)
 dump(root/'implementation.json',{'binary_sha256':sha(BINARY),'adapter_sha256':sha(SOURCE),'sources':{p.name:sha(p) for p in HERE.glob('*.py')}})
 (root/'plan.md').write_text('''# 计算、访存与SUE首轮实验计划

运行前登记：3模型；硬件规格以本目录hardware-profile.json为准，换算参数见hardware.json。计算、HBM、MPU服务与SUE端口/UCIe环速率均由选定JSON生成。本轮远程读走SUE，不经过PCIe。

默认计算30%、HBM50%、MPU50%，MPU访问200ns，预留20%本地容量；这些是敏感性参数，未经硬件校准。每Die计算/HBM各一个FIFO服务资源，彼此可重叠；MPU每节点一个共享FIFO。环传输沿现有包级UCIe模型。远程读为请求事务→MPU服务→数据回传事务→本地写入，两个SUE事务各自原口确认，不宣称等价AXI远程读实现。

全模型容量及roofline预算与逐包实验分开展示。部署实验模拟一个代表MoE块、均摊公共计算/访存和TP环形all-reduce，连续2个decode步骤；不乘层数冒充全模型实测。容量不合格的部署保留拒绝记录。DeepSeek Engram-only实验不要求完整模型放得下，明确区分阶段可执行与全模型容量合格。

实验分组：部署规模×batch(1/8/32/64)×上下文(8K/128K)×3seed；4MPU下专家流式读取0/1/5%对照；DeepSeek Engram本地/2MPU/4MPU×batch×3seed；随机查表有效带宽10/80%与200/1000ns访问延迟敏感性。专家远端集合为expert_id%100<round(100f)，有限专家数量导致实际比例略偏离标称，报告实际流量；每步重取、不做跨步缓存。

预测：加入资源排队后，峰值算力不再直接决定decode速度；扩大batch可能提高吞吐而拉长一步时延。远端专家流式读取可能引入明显额外传输；Engram因容量大而每token查表字节少，更可能适合MPU，但小请求延迟和带宽口径必须控制。失败/超时不纳入均值，不修改种子挑选结果。

注意力/KV预算为明确简化模型，KV页跨Die均衡分片，不假设TP复制；另外输出TP复制KV容量上界。无GPU ISA/缓存/DRAM时序；DeepSeek不启用DSpark/MTP和视觉，prefill编码器差异只进入独立解析预算。专家流量与Engram索引均为合成路由，不是实测trace。
''')
 results=[]
 with ThreadPoolExecutor(max_workers=workers) as pool:
  fs={pool.submit(execute,root,d,i):d for i,d in enumerate(defs)}
  for f in as_completed(fs):
   try:r=f.result()
   except Exception as e:r={**fs[f],'status':'runner_error','error':str(e),'traceback':traceback.format_exc()}
   results.append(r);dump(root/'results.json',results);print(json.dumps({'done':len(results),'total':len(defs),'model':r['model'],'study':r['study'],'status':r['status'],'window_us':r.get('window_us'),'error':r.get('error')},ensure_ascii=False),flush=True)
 return results
if __name__=='__main__':main(sys.argv[1],int(sys.argv[2]) if len(sys.argv)>2 else 3)
