#!/usr/bin/env python3
"""Prefill-style parameter chunk microbenchmarks, deliberately not TTFT."""
import sys,json,traceback
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
from inference_profile import profile,NAMES,best_tp
from inference_experiments import execute
from run import dump
from hardware_config import save_hardware

def main(root):
 root=Path(root);save_hardware(root);defs=[]
 for name in NAMES:
  m=profile(name)
  for n in (m['base_chips'],m['base_chips']*2):
   M=2 if name==NAMES[2] and n==4 else 0;remote=bool(M);tp=best_tp(m,n,1,8192,remote,M)
   for tokens in (128,512):
    for seed in (1,2,3):defs.append(dict(study='prefill_parameter_chunk',model=name,chips=n,tp=tp,batch=tokens,tokens_per_chunk=tokens,capacity_sequences=1,context=8192,mpus=M,engram_remote=remote,include_engram=name==NAMES[2],parameter_chunk_only=True,seed=seed,steps=1))
 assert not (root/'prefill-design.json').exists();dump(root/'prefill-design.json',defs)
 (root/'prefill-plan.md').write_text('# Prefill风格参数块补充实验\n\n独立登记36项：3模型×两档卡数×128/512 token chunk×3seed。公共参数矩阵与MoE计算/权重访问、TP/EP通信使用资源队列和逐包SUE；为隔离参数复用，明确不含注意力QK/AV和KV读写，不称完整prefill或TTFT。容量按1条8K输入序列检查，chunk token数量不是并发会话数。DeepSeek4芯片使用2MPU存Engram，8芯片本地；其他模型全本地。不同卡数按本场景容量选择TP，因此同时改变部署分组。预测：扩大chunk提高专家权重复用但可能增加网络和矩阵时间，不能由此直接推出完整TTFT。\n')
 out=[]
 with ThreadPoolExecutor(max_workers=3) as pool:
  fs={pool.submit(execute,root/'prefill',d,i):d for i,d in enumerate(defs)}
  for f in as_completed(fs):
   try:r=f.result()
   except Exception as e:r={**fs[f],'status':'runner_error','error':str(e),'traceback':traceback.format_exc()}
   out.append(r);dump(root/'prefill-results.json',out);print(json.dumps({'done':len(out),'total':len(defs),'status':r['status'],'model':r['model'],'us':r.get('window_us'),'error':r.get('error')},ensure_ascii=False),flush=True)
if __name__=='__main__':main(sys.argv[1])
