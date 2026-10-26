#!/usr/bin/env python3
"""Audit experimental evidence and generate a standalone hardware exploration report."""
import json,sys,statistics,math,hashlib
from pathlib import Path
from collections import defaultdict,Counter
from markdown_it import MarkdownIt
from run import dump,csvwrite,sha,HERE,csvread
from inference_profile import HW,NAMES,profile,budget,best_tp

TITLES={'deployment':'容量约束下的decode代表块','expert_streaming':'每步远端专家权重读取','engram':'Engram查表阶段','engram_sensitivity':'Engram随机访问敏感性','deepseek_placement':'DeepSeek部署取舍','matched_tp_scale':'固定TP扩卡对照','kv_restore_pages':'冷KV页恢复','prefill_parameter_chunk':'Prefill风格参数块'}
def audit(root):
 results=[];planned=0
 for name in ('','restore-','prefill-'):
  rr=json.loads((root/(name+'results.json')).read_text());design=json.loads((root/(name+'design.json')).read_text());assert len(rr)==len(design);results+=rr;planned+=len(design)
 checked=0;binary=set();rejected=0
 for r in results:
  if r['status']=='capacity_rejected':assert not r['capacity']['admitted'];rejected+=1;continue
  if r.get('run'):
   p=Path(r['run']);r['evidence']=str(p.relative_to(root));man=json.loads((p/'command-manifest.json').read_text());binary.add(man['binary_sha256'])
   for f,h in man['inputs'].items():assert sha(p/f)==h;checked+=1
   if r['status']=='passed':
    assert r['completed']==r['tasks'] and r['received_bytes']==r['logical_bytes'] and not r['failures']
    ts=csvread(p/'task_statistics.csv');assert r['window_us']==(max(int(x['end_ps']) for x in ts)-1000000)/1e6
    rss=csvread(p/'resource_statistics.csv');spec=json.loads((p/'resources.json').read_text());assert len(rss)==len(spec)
    pools=defaultdict(list)
    for z,q in zip(rss,spec):
     # C++ emits task order; JSON resources are appended in task order too.
     assert int(z['id'])==q['id'];a,b=int(z['service_start_ps']),int(z['end_ps']);assert b>=a>=0
     if q['kind']==3:assert int(z['local_received_bytes'])==q['units']
     else:assert b-a==q['service_ns']*1000;pools[z['chip'],z['die'],z['kind']].append((a,b))
    for intervals in pools.values():
     intervals.sort();assert all(b[0]>=a[1] for a,b in zip(intervals,intervals[1:]))
 assert len(binary)==1
 paired=defaultdict(list)
 for r in results:
  if r['status']=='passed' and r['study']=='expert_streaming':paired[r['model'],r['seed']].append(sha(Path(r['run'])/'token_routes.csv'))
 assert all(len(x)==3 and len(set(x))==1 for x in paired.values())
 verification=dict(status='passed',planned=planned,packet_passed=sum(r['status']=='passed' for r in results),capacity_rejected=rejected,failed=sum(r['status'] not in ('passed','capacity_rejected') for r in results),input_hashes_checked=checked,binary_versions=len(binary),expert_streaming_logical_route_controls=len(paired),checks=['all registered cases accounted for','input SHA256','complete payload delivery','resource service duration and non-overlap','local D2D bytes','expert choices preserved across streaming fractions'])
 dump(root/'verification.json',verification);dump(root/'all-results.json',results)
 return results,verification

def analytic_matrix(root):
 rows=[];prefill=[]
 for name in NAMES:
  m=profile(name)
  for n in (m['base_chips'],m['base_chips']*2):
   for remote in ([False,True] if name==NAMES[2] else [False]):
    M=2 if remote else 0
    for B in (1,8,32,64):
     for L in (8192,32768,131072):
      for tp in (4,8,16,32,64,128):
       if tp<=n*4 and n*4%tp==0:rows.append(budget(m,n,tp,B,L,M,remote))
    for prompt in (8192,32768,131072):
     tp=best_tp(m,n,1,prompt,remote,M);cap=budget(m,n,tp,1,prompt,M,remote);ar=m['moe_layers']*m['expert_params']*m['top_k'];ac=m['common_active_params'];factor=.5 if name==NAMES[2] else 1
     sec=prompt*factor*(2*ar/(n*4*HW[m['expert_dtype']+'_flops_s_per_die'])+2*ac/(n*4*HW[m['common_dtype']+'_flops_s_per_die']))/HW['compute_efficiency']
     prefill.append(dict(model=name,chips=n,tp=tp,prompt_tokens=prompt,mpus=M,admitted=cap['admitted'],parameter_compute_budget_ms=sec*1000,scope='analytic parameter-matrix service only; no attention QK/AV, network or queue; DeepSeek prefill encoder half; NOT TTFT'))
 csvwrite(root/'capacity-budget.csv',rows);csvwrite(root/'prefill-budget.csv',prefill);return rows,prefill

def main(root):
 root=Path(root).resolve();results,v=audit(root);caps,prefill=analytic_matrix(root)
 keys=['study','model','chips','tp','batch','context','mpus','expert_remote_fraction','bytes_per_session','hardware_overrides','tokens_per_chunk']
 groups=defaultdict(list)
 for r in results:
  if r['status']=='passed':groups[tuple(json.dumps(r.get(k),sort_keys=True) for k in keys)].append(r)
 means=[]
 for key,rr in groups.items():
  g={k:json.loads(value) for k,value in zip(keys,key)};g['n']=len(rr)
  for metric in ('window_us','per_step_block_us','resource_queue_us','effective_GB_s','local_d2d_bytes','logical_bytes'):
   vals=[r[metric] for r in rr if r.get(metric) is not None]
   if vals:g[metric]=statistics.mean(vals);g[metric+'_min']=min(vals);g[metric+'_max']=max(vals)
  means.append(g)
 profiles=[{k:v for k,v in profile(n).items() if k!='config'} for n in NAMES]
 data={'results':results,'groups':means,'capacity':caps,'prefill':prefill,'hardware':HW,'profiles':profiles,'verification':v,'studies':TITLES}
 dump(root/'assessment.json',data)
 fields=['study','model','chips','tp','batch','context','mpus','seed','status','window_us','per_step_block_us','logical_bytes','resource_queue_us','effective_GB_s','scope','evidence']
 csvwrite(root/'summary.csv',[{k:r.get(k,'') for k in fields} for r in results])
 def get(study,model,**kw):return next(g for g in means if g['study']==study and g['model']==model and all(g[k]==value for k,value in kw.items()))
 text=f'''# SUE v2：算力、内存与通信联合探索

本轮登记{v['planned']}项：{v['packet_passed']}项逐包实验完成，{v['capacity_rejected']}项因容量预算不足而拒绝，{v['failed']}项执行失败。另有5项资源验收和2项旧案例回归，旧任务时间戳完全一致。所有主条件seed1/2/3，均值和min–max不是置信区间。

**三种证据分开读：全模型容量检查、解析资源下界、逐包阶段实测。没有把单个代表块乘层数称作完整模型TPOT，也没有把参数运算预算称作TTFT。**

## 本轮实际新增的能力

- 每Die一个计算FIFO和一个HBM FIFO，资源独立，可近似计算/内存重叠；服务时长由FLOPs、精度或字节数与有效带宽换算，多个任务不能各自独占资源。
- 每MPU一个共享服务FIFO；远程读由请求事务、MPU服务、数据回传事务、本地HBM写入组成。MPU服务在请求事务确认后启动，较接收立即服务保守；两个SUE事务各自原口确认，不宣称完整AXI读协议。
- 同芯片跨Die通信真正走本地UCIe环，不绕交换机；环缓冲与链路继续竞争。保持原SUE AFH/LLR/CBFC算法。
- 新增DeepSeek-V4.1-Flash-1017冻结配置、混合精度、Engram查表和共享压缩KV预算；文本场景，不启用视觉、DSpark/MTP投机解码。

## 硬件与假设

每Die：FP4/FP8/FP16分别900/450/225TFLOPS，36GB、3.75TB/s本地内存。芯片D2D单向2TB/s，按均分解释为每Die发送500GB/s，再分到两个邻接方向各250GB/s；8条有向环边各2000Gbit/s。scale-up双向1.6TB/s对应单向0.8TB/s，保持32×200G。每MPU512GB、190GB/s；接入8×200G，单向理论200GB/s。PCIe128GB/s仅作为备选路径参数，本轮远程读走SUE，不扣PCIe带宽。

有效计算30%、HBM50%、MPU50%；MPU固定访问200ns，Engram假设16行随机访问可并行，按ceil(行数/16)个访问周期计固定延迟；敏感性扫描MPU10/80%及200/1000ns。以上效率、延迟、随机并发均为假设，尚未校准硬件。FP4吞吐用于FP4专家算子，若实际W4A8内核达不到该档需调整。

容量预留20%，每Die可分配上限28.8GB。KV按页均衡分片，不按TP复制；同时输出TP复制KV的容量上界。公共权重副本按TP/DP分组计入，专家按owner=expert_id%(4×芯片数)分片。非专家参数由公开激活参数量反推，另预留embedding/head容量，尚非完整逐张量盘点；不应拿小于几个百分点的余量作硬件采购保证。

## 重要发现

### 1. DeepSeek的Engram更适合内存节点

该模型不能按“763B全部FP8”估算。官方配置为路由专家FP4、其余多为FP8；两张Engram表约196.6GB原始权重，计入32元素一字节scale后约202.8GB。一次token查询2张表×3种n-gram×8个head=48行，压缩载荷约12.7KB。容量大、每步实际取数小，是它适合远端存放的原因。索引为合成均匀散列，无真实文本命中率或缓存假设。

4芯片全本地在20%预留条件下不合格；4芯片+MPU可释放约202.8GB本地容量。下面的对照是“代表MoE块+完整两表查表阶段”，不是全模型生成一步：

| batch | 4芯片+2MPU µs | 4芯片+4MPU µs | 8芯片全本地 µs |
|---:|---:|---:|---:|
'''
 for B in (1,8,32,64):
  zz=[get('deepseek_placement',NAMES[2],chips=n,mpus=M,batch=B)['per_step_block_us'] for n,M in ((4,2),(4,4),(8,0))]
  text+=f'| {B} | '+' | '.join(f'{x:.3f}' for x in zz)+' |\n'
 text+='''
它证明的是容量取舍和这一阶段的性能代价：减少芯片并不免费，增加MPU也不能消除计算端差距。没有节点价格与真实SLO，不能宣布哪组成本最优。独立Engram本地查表探针可运行，不代表4芯片本地完整模型通过容量检查。

### 2. 每步重新搬专家权重，代价明显

固定4MPU、batch32、32K上下文；仅改变放在远端并每步重取的专家集合。0/1/5%是按expert_id%100选取的标称比例，实际比例见预算CSV；相同seed保持相同专家选择。本地暂存后仍需HBM读，写入成本也计入。未实现跨步专家缓存或预取，结论不能外推到高命中率缓存。

| 模型 | 全本地 µs/代表块 | 1%远端 µs | 5%远端 µs | 1%相对本地倍数 |
|---|---:|---:|---:|---:|
'''
 for name in NAMES:
  zz=[get('expert_streaming',name,expert_remote_fraction=f)['per_step_block_us'] for f in (0,.01,.05)]
  text+=f'| {name} | {zz[0]:.3f} | {zz[1]:.3f} | {zz[2]:.3f} | {zz[1]/zz[0]:.2f}× |\n'
 text+='''
这支持优先把热专家留本地。Engram查表只取少量行，而专家GEMM需要整块权重，二者不可用同一个offload收益结论。新增事务也改变任务ID/哈希输入，种子控制逻辑路由而不保证所有报文端口完全相同。

### 3. 容量不够是部署拒绝，不是仿真超时

DeepSeek4芯片全本地的24项部署条件被拒绝；GLM8芯片在128K、batch32/64的6项条件被拒绝。Kimi16芯片的高batch/长上下文需要更大的TP以压低公共权重副本；32芯片可使用较小TP。主扫描改变卡数时可能同时改变TP，因此另有固定TP扩卡对照。容量热力图按每个条件选择最小可行TP，和主扫描固定TP的性能图不是同一选择算法。

### 4. 冷KV恢复与活跃KV远程读取是不同问题

本轮恢复实验传输每会话4/32MiB页集合，最后完成时间包含所有会话的本地写入；有效GB/s=所有会话数据字节/窗口。它是恢复阶段测试，不是每步读取全量KV，也不是完整128K会话恢复实测。更多MPU仍受客户端端口、流哈希和请求粒度限制。

### 5. Prefill结果的口径

逐包实验覆盖128/512 token的参数块：公共矩阵、专家矩阵、权重读取与TP/EP通信；明确排除注意力QK/AV和KV流量，不能直接称TTFT。全模型prefill预算另按每序列8K/32K/128K输入计算参数矩阵服务时间；DeepSeek使用编码器半栈。该解析预算也不包含网络和排队。

## KV与模型边界

Kimi：24层完整MLA缓存，69层KDA以FP32矩阵状态近似，含状态读写，不含卷积scratch。GLM：78层MLA缓存，注意力每层最多2048选中latent，indexer按ceil(78/4)个完整key扫描近似。DeepSeek：4个共享压缩KV/index缓存及各层128窗口，index访问用保守全扫描近似，未精确复现候选筛选流水。三者都不是实测访存trace，注意力计算未完整建模。

公共/专家计算和HBM资源以roofline式重叠近似；没有指令、缓存、DRAM bank、实际kernel并发或调度开销。代表块用全模型公共工作量的1/L与一个MoE层组合，不代表所有层相同。解析roofline忽略不均衡与关键路径，只能作为资源下界；有效效率也不能据这些数据自证。

## 来源与复现

[DeepSeek 1017官方模型](https://huggingface.co/deepseek-ai/DeepSeek-V4.1-Flash-1017)、冻结的官方inference/model.py与engram.py保存在inputs目录；[Kimi官方模型](https://huggingface.co/moonshotai/Kimi-K3)、[GLM官方模型](https://huggingface.co/zai-org/GLM-5.3)。主配置沿用此前冻结版本，DeepSeek按commit冻结，URL与SHA见各source.json。公开激活参数量用于简化资源预算，不是逐张量测得的有效FLOPs。

[主实验计划](plan.md) · [KV恢复计划](restore-plan.md) · [Prefill参数块计划](prefill-plan.md) · [全部指标](summary.csv) · [容量预算](capacity-budget.csv) · [prefill预算](prefill-budget.csv) · [校验记录](verification.json)。327项不包含5项资源验收和2项旧案例回归。
'''
 (root/'report.md').write_text(text)
 md=MarkdownIt('commonmark').enable('table');template=(HERE/'inference_template.html').read_text().replace('@@DETAILS@@',md.render(text)).replace('@@SCRIPT@@',(HERE/'inference_visuals.js').read_text()).replace('@@DATA@@',json.dumps(data,ensure_ascii=False,separators=(',',':')).replace('<','\\u003c'))
 (root/'report.html').write_text(template);print(json.dumps(v,ensure_ascii=False,indent=2))
if __name__=='__main__':main(sys.argv[1])
