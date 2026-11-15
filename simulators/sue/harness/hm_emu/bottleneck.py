"""Critical path accounting from measured second-step events and frozen service inputs."""
import csv,json,pathlib,numpy as np
from run import load,plan_costs

def analyze(rows):
 out=[]
 for mode,n in [('TP',8)]:
  r=next(x for x in rows if x['mode']==mode and x['chips']==n and x['batch']==1 and x['duplex']=='sum' and x['variant']=='base')
  d=pathlib.Path(r['path']);root=d.parent.parent
  if mode=='TP':
   impl=json.loads((root/'implementation.json').read_text());config=next(pathlib.Path(k) for k in impl['input_files'] if pathlib.Path(k).suffix=='.json' and json.loads(pathlib.Path(k).read_text()).get('schema')=='hm-emu-1')
   c,m=load(config);tiles=np.load(root/'routes/expert-tiles-m1.npy')
   cost,metrics,_=plan_costs(m,1,8,'TP',tiles,r['partition'])
   frozen=np.fromfile(d/'service.bin',dtype='<f8').reshape(r['service_shape']);assert np.allclose(cost*1e12,frozen,rtol=1e-14,atol=1e-4)
   local=float(np.ceil(frozen[1,0]).sum()/1e9);v=metrics[1,0,0];fixed=278*14*.002
  else:
   phases=[p for p in json.loads((d/'phases.json').read_text()) if p['step']==1]
   tasks={int(t['id']):t for t in csv.DictReader((d/'engine-ep-tasks.csv').open())}
   dur=lambda p:(int(tasks[p['end_id']]['end_ps'])-(int(tasks[p['start_id']]['end_ps']) if p['start_id'] is not None else 0))/1e9
   local=sum(dur(p) for p in phases if p['kind']=='local');network=sum(dur(p) for p in phases if p['kind']=='network')
   assert abs(local+network-r['tpot_ms'])<1e-6
   v=np.sum([p['critical_resources'] for p in phases if p['kind']=='local'],axis=0);fixed=sum(p.get('fixed_steps',0) for p in phases)*.002
  variants={x['variant']:x['tpot_ms'] for x in rows if x['mode']==mode and x['chips']==n and x['batch']==1 and x['duplex']=='sum'}
  out.append(dict(strategy='TP8（8卡）' if mode=='TP' else 'TP8＋EP2（16卡）',tpot_ms=r['tpot_ms'],local_ms=local,network_ms=r['tpot_ms']-local,fixed_network_ms=fixed,hbm_ms=float(v[2]*1000),d2d_ms=float(v[3]*1000),compute_busy_ms=float(v[1]*1000),hbm_GB=float(v[4]/1e9),variants=variants))
 return out

def section(rows,table):
 a=analyze(rows)
 content='<section id="bottleneck"><h2>3. 单请求瓶颈：B=1、TP8</h2><p>固定B=1、主实验TX64/RX64 GB/s、2 μs固定网络路径，统计第二步完整93层decode。以下拆解来自服务输入和逐包事件完成时间，并非用理论带宽倒推整体耗时。</p>'
 content+=table(['策略','TPOT / ms','本地依赖段 / ms','网络依赖段 / ms','网络固定预算 / ms'],[[x['strategy']]+[f'{x[k]:.4f}' for k in ('tpot_ms','local_ms','network_ms','fixed_network_ms')] for x in a])
 content+='<p>本地依赖段与网络依赖段相加等于TPOT。</p>'
 content+='<h3>为什么TP8仍需要约37 ms？</h3><p>首先是本地HBM读取。每Die有效带宽为562.5 GB/s，TP8第二步每Die约读取16.35 GB，HBM服务预算约29.07 ms。当前布局把latent down/up和路由器在各Die复制：92层latent down/up合计约9.45 GB/Die/token，路由器约1.18 GB。它们不会随TP卡数增加而继续均分。这里反映的是本次参数布局、权重tile复用和缓存假设，不是Kimi K3在真实芯片上的不可突破下限。</p><p>其次是多轮小消息同步。每token有278次卡间Ring AllReduce，TP8每次14步；仅固定路径预算即278×14×2 μs＝7.784 ms。实际网络依赖段还包含序列化、通知及控制帧竞争。网络等待不等于网络拥塞，更不等于链路带宽已满。</p>'
 content+=table(['策略','HBM服务预算 / ms','片内D2D预算 / ms','计算忙碌预算 / ms'],[[x['strategy']]+[f'{x[k]:.4f}' for k in ('hbm_ms','d2d_ms','compute_busy_ms')] for x in a])
 content+='<p>这些是一个Die的资源预算，不能直接相加：算子内部计算和HBM读取存在双缓冲重叠。</p><h3>通过配对实验区分带宽与延时</h3>'
 content+=table(['策略','原配置 ms','网络带宽×2 ms','固定路径2→1 μs ms'],[[x['strategy'],f'{x["tpot_ms"]:.4f}',f'{x["variants"]["bandwidth_x2"]:.4f}',f'{x["variants"]["latency_half"]:.4f}'] for x in a])
 content+='<p>带宽翻倍的收益较小，而缩短每一步固定路径更有效，支持“小batch更受重复同步延时和本地HBM读取影响”的判断。敏感性对照仅改变对应网络参数，计算和路由保持一致；结果是行为模型预测，尚未做芯片实测校准。</p></section>'
 return content
