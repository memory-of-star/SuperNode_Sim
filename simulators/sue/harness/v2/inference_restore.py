#!/usr/bin/env python3
"""Separately registered cold-KV page restoration transport controls."""
import sys,json,math
from pathlib import Path
from inference_experiments import DAG,SETTINGS
from inference_profile import HW
from hardware_config import HARDWARE_SOURCE,network_settings,save_hardware
from run import create,run_case,dump,csvread,sha

def main(root):
 root=Path(root);save_hardware(root);defs=[dict(study='kv_restore_pages',model='shared-transport',chips=4,mpus=M,batch=B,bytes_per_session=n,seed=s) for M in (2,4) for B in (1,8) for n in (4*1024**2,32*1024**2) for s in (1,2,3)]
 assert not (root/'restore-design.json').exists();dump(root/'restore-design.json',defs)
 (root/'restore-plan.md').write_text('# 冷KV恢复传输补充对照\n\n独立于267项主矩阵登记24项：2/4MPU × 1/8并发会话 × 每会话4/32MiB页集合 × seed1/2/3。每个页集合均匀条带到所有MPU；请求事务完成→MPU服务→数据回传→本地HBM写入。只测恢复阶段，不把4/32MiB当成完整模型的KV容量，也不将带宽线性外推声称为完整会话恢复实测。预测：增加MPU仅在对应端口与客户端带宽允许时改善；并发会话共享190GB/s节点服务。\n')
 out=[]
 for i,d in enumerate(defs):
  dag=DAG(HW)
  for client in range(d['batch']):
   rank=client%16;sa=(rank//4,rank%4)
   for bank in range(d['mpus']):dag.remote_read(sa,4+bank,d['bytes_per_session']//d['mpus'],[],f'kv-session{client}')
  c=create(root/'restore-cases'/f'{i:03d}',f'hw-4-{d["mpus"]}',dag.tasks,{'model':'shared-transport','window':'cold-kv-page-restore','definition':d,'hardware':HW,'hardware_source':HARDWARE_SOURCE,'scope':'selected KV pages only; not a whole model'},settings={**SETTINGS,**network_settings(HW),'seed':d['seed']},resources=dag.resources)
  r=run_case(c);r.update(d);r['scope']='selected KV-page restoration phase';r['tasks_sha256']=sha(c/'tasks.json');r['resources_sha256']=sha(c/'resources.json')
  if r['status']=='passed':
   rows=csvread(Path(r['run'])/'task_statistics.csv');r['window_us']=(max(int(x['end_ps']) for x in rows)-1000000)/1e6;r['effective_GB_s']=d['batch']*d['bytes_per_session']/r['window_us']/1000
  out.append(r);dump(root/'restore-results.json',out);print(json.dumps({'done':len(out),'total':len(defs),'status':r['status'],'us':r.get('window_us')},ensure_ascii=False),flush=True)
if __name__=='__main__':main(sys.argv[1])
