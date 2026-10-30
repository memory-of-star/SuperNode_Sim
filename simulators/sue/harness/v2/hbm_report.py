"""Audit paired HBM runs and write plots plus a chapter-ready report."""
import json,sys,statistics,html,base64,shutil
from pathlib import Path
from collections import defaultdict
from run import csvread,csvwrite,dump,sha

def main(root):
 root=Path(root).resolve();shutil.copy2(Path(__file__).resolve().parents[4]/'results/sue/hbm-fair-validation-v2-20261028/verification.json',root/'mechanism-verification.json');results=json.loads((root/'results.json').read_text());design=json.loads((root/'design.json').read_text())
 assert len(results)==len(design) and all(r['status']=='passed' for r in results)
 key=lambda r:(r['backend'],r['mpus'],r['seed'],r.get('mpu_fraction',-1))
 pairs=defaultdict(dict)
 old=json.loads((Path(__file__).resolve().parents[4]/'results/sue/offload-mpu-pcie-transaction-20261027/analysis/results.json').read_text())
 for r in results:
  p=Path(r['run']);manifest=json.loads((p/'command-manifest.json').read_text())
  assert all(sha(p/n)==v for n,v in manifest['inputs'].items())
  assert json.loads((p/'summary.json').read_text())['status']=='passed'
  pairs[key(r)][r['hbm_policy']]=r
 for k,pair in pairs.items():
  a,b=pair['fifo'],pair['fair'];assert a['offload_bytes']==b['offload_bytes']
  for name in ('tasks.json','resources.json','token_routes.csv','offload-requests.json','pcie.json','pcie-requests.json'):
   assert sha(Path(a['run'])/name)==sha(Path(b['run'])/name),(k,name)
  hist=next(r for r in old if r['model']==a['model'] and r['target']=='both' and key(r)==k)
  for name in ('task_statistics.csv','resource_statistics.csv','port_statistics.csv','pcie_descriptors.csv','pcie_resources.csv'):
   assert sha(Path(a['run'])/name)==sha(Path(hist['run'])/name),(k,name,'FIFO regression')
 groups=defaultdict(list)
 for r in results:groups[(r['hbm_policy'],r['backend'],r['mpus'],r.get('mpu_fraction',-1))].append(r)
 rows=[]
 for (mode,backend,m,f),rs in groups.items():
  assert {r['seed'] for r in rs}=={1,2,3}
  baseline=statistics.mean(r['block_us'] for r in groups[(mode,'pcie',0,-1)])
  times=[r['block_us'] for r in rs]
  rows.append(dict(policy=mode,backend=backend,mpus=m,fraction=f,mean_us=statistics.mean(times),min_us=min(times),max_us=max(times),pcie_us=baseline,speedup=baseline/statistics.mean(times)))
 csvwrite(root/'summary.csv',rows);dump(root/'summary.json',rows)
 import matplotlib;matplotlib.use('Agg')
 import matplotlib.pyplot as plt
 fig,axs=plt.subplots(1,2,figsize=(13,4.5),layout='constrained')
 for mode,color in [('fifo','#b5453c'),('fair','#137c95')]:
  rr=sorted([r for r in rows if r['policy']==mode and r['backend']=='hybrid' and r['fraction']==-1],key=lambda r:r['mpus'])
  xx=[r['mpus'] for r in rr];yy=[r['mean_us'] for r in rr]
  axs[0].plot(xx,yy,'o-',label=mode,color=color);axs[0].fill_between(xx,[r['min_us'] for r in rr],[r['max_us'] for r in rr],alpha=.12,color=color)
  axs[1].plot(xx,[r['speedup'] for r in rr],'o-',label=mode,color=color)
 for ax in axs:
  ax.set_xscale('log',base=2);ax.set_xticks([1,2,4,8,16],labels=['1','2','4','8','16']);ax.set_xlabel('MPU count');ax.grid(alpha=.2);ax.legend()
 axs[0].set_ylabel('Microseconds / representative block');axs[0].set_title('Absolute time; 3 seeds, min-max shade')
 axs[1].axhline(1,color='#555',ls='--');axs[1].set_ylabel('PCIe baseline / hybrid time');axs[1].set_title('Each curve uses its own HBM-matched PCIe baseline')
 fig.suptitle('DeepSeek KV + Engram: FIFO vs capped max-min HBM | nominal share')
 fig.savefig(root/'hbm-comparison.svg');fig.savefig(root/'hbm-comparison.png',dpi=160);plt.close(fig)
 def table(rr,cols):
  def val(v):return f'{v:.3f}' if isinstance(v,float) else str(v)
  return '\n'.join(['| '+' | '.join(b for a,b in cols)+' |','| '+' | '.join('---' for _ in cols)+' |']+['| '+' | '.join(val(r[a]) for a,b in cols)+' |' for r in rr])+'\n\n'
 compare=[]
 for m in (1,2,4,8,16):
  a=next(r for r in rows if (r['policy'],r['mpus'],r['fraction'])==('fifo',m,-1));b=next(r for r in rows if (r['policy'],r['mpus'],r['fraction'])==('fair',m,-1))
  compare.append(dict(mpus=m,fifo_us=a['mean_us'],fair_us=b['mean_us'],change_pct=(b['mean_us']/a['mean_us']-1)*100,fifo_speedup=a['speedup'],fair_speedup=b['speedup']))
 csvwrite(root/'comparison.csv',compare)
 text='''## 9.10 HBM公平共享：DeepSeek KV＋Engram重跑对照

本节使用新的HBM共享模型。§9.3–9.6保留上一轮HBM FIFO实验，不应将两轮PCIe基线混用。本轮只重跑DeepSeek KV＋Engram，未声称其他模型也已更新。

### 共享机制与配置

每Die有效HBM带宽仍为1.875 TB/s。活跃访问源先均分；达到自身速率上限的源只取所需份额，其余带宽继续分给未达上限的源。请求到达和完成时更新剩余字节与速率，不等整项专家读取结束。没有需求时不分配带宽，所有源都受限时允许带宽闲置。

源按业务类别划分：专家读取、公共层读取、KV消费、Engram消费、SUE写入、PCIe写入及其他。同类源内FIFO，各源平权；拆分更多请求不会获得更多权重。KV与Engram的SUE写入共享200 GB/s/Die上限；PCIe写入上限128 GB/s/Die，真实可用速率还受四Die共享的全芯片PCIe、DDR及到达时刻限制，不能将四个上限相加当卡带宽。接口限速源用不满的HBM份额会被专家读取等其他源使用。

配置见[HBM JSON](examples/hbm-fair-share.json)，带宽不能超过硬件有效预算。未传入该JSON的历史入口仍采用FIFO。

PCIe保持有限接收缓冲：收到Completion后才提交写入，实际HBM写完后才释放缓冲，慢HBM会反向限制PCIe。SUE仍保留整消息收齐和确认后再写HBM的模型，写入速率受接口上限约束；没有新增逐SUE包到HBM的流式信用耦合。这次改动也没有复刻真实DRAM bank、读写切换、缓存或GPU仲裁优先级。

### 实验口径

42组：FIFO与公平共享分别运行纯PCIe基线、1/2/4/8/16 MPU nominal share、8 MPU固定25%分流，全部seed1/2/3。固定16卡、TP4×DP16、EP64、B64、初始上下文32768、两步代表块、原256 KiB分块、同一事务级PCIe。25%来自先前实验，仅作为固定比例控制，不重新选择，不标为新的confirmed share。结果为µs/代表块，不是完整模型TPOT。

两种HBM模式的任务、资源输入、专家路由、访问字节及PCIe地址均逐字节配对；21个FIFO条件复现历史统计。旧新差异同时包含带宽共享与写入速率上限，不能只归因于某一项。

![HBM公平共享与FIFO对照](hbm/hbm-comparison.svg)

左图直接比较耗时，阴影为三个种子范围。右图分别用各自HBM模式的纯PCIe均值作分子，避免用旧基线夸大新模式收益。

'''
 text+=table([r for r in rows if r['backend']=='pcie'],[('policy','HBM模式'),('mean_us','纯PCIe基线µs')])
 text+=table(compare,[('mpus','MPU'),('fifo_us','FIFO µs'),('fair_us','公平共享µs'),('change_pct','耗时变化%'),('fifo_speedup','FIFO速度比'),('fair_speedup','共享速度比')])
 m4=next(r for r in compare if r['mpus']==4)
 baselines={r['policy']:r['mean_us'] for r in rows if r['backend']=='pcie'}
 worst=max(compare,key=lambda r:r['fair_us'])
 text+=f"**结果解读：**4 MPU的均值从{m4['fifo_us']:.3f}降到{m4['fair_us']:.3f} µs（变化{m4['change_pct']:+.2f}%）。本轮共享模型的nominal share最慢点仍是{worst['mpus']} MPU，因此取消整项专家读取的独占阻塞并没有保证曲线单调。不能再将剩余拐点全部归因于旧HBM FIFO，仍需用固定分流/时序消融区分网络返回与DAG调度的贡献。纯PCIe也从{baselines['fifo']:.3f}降到{baselines['fair']:.3f} µs，绝对耗时下降不等于相对PCIe收益增加。\n\n"
 text+='### 8 MPU固定25%分流控制\n\n'
 text+=table([r for r in rows if r['fraction']==.25],[('policy','模式'),('mean_us','µs/代表块'),('speedup','相对本模式PCIe速度比')])
 text+='### 原始排队反例的复查\n\n以下是4 MPU、seed2、第一步、XPU13 Die0原先受阻的256 KiB KV写入。就绪到首次服务是源内FIFO等待；首次服务到完成包含共享导致的服务拉长，二者不能混淆。\n\n'
 details=[]
 for mode in ('fifo','fair'):
  r=next(r for r in results if r['hbm_policy']==mode and r['mpus']==4 and r['seed']==2)
  p=Path(r['run']);tasks=json.loads((p/'tasks.json').read_text());stats={int(x['id']):x for x in csvread(p/'task_statistics.csv')};rss={int(x['id']):x for x in csvread(p/'resource_statistics.csv')}
  t=next(t for t in tasks if t['src_chip']==13 and t['src_die']==0 and t['label']=='0:kv:mpu:chunk0:hbm-write');q=rss[t['id']]
  details.append(dict(mode=mode,ready=int(stats[t['id']]['start_ps'])/1e6,start=int(q['service_start_ps'])/1e6,end=int(q['end_ps'])/1e6,wait=int(q['queued_ps'])/1e6))
 text+=table(details,[('mode','模式'),('ready','就绪µs'),('start','首次服务µs'),('end','完成µs'),('wait','源内等待µs')])
 a,b=details
 text+=f"该请求源内等待从{a['wait']:.3f} µs变为{b['wait']:.3f} µs；整个任务图的变化仍应以上面的配对均值为准，单个请求不能解释全部性能。\n\n"
 text+='''### 验证、统计与复现

机制测试覆盖均分、限速水位分配、源加入/退出、小请求不增加权重、Die独立、旧FIFO精确回归、PCIe接收缓冲填满与专家减速。全量实验核验字节守恒、DAG依赖、源速率上限、Die总带宽积分和PCIe信用/缓冲限制。

共享模式下`resource_statistics.csv`的输入`service_ns`是历史独占预算，不再等于实际服务历时；使用真实`end_ps`。`queued_ps`只统计同类源内部排队。`hbm_sources.csv`的各源活跃时间相互重叠，不能相加当HBM利用率；字节积分和`hbm_dies.csv`总量用于核验带宽守恒。PCIe的HBM busy同样表示经历时间，不是独占时间。

[配对汇总CSV](hbm/comparison.csv) · [全部统计](hbm/summary.csv) · [每例结果](hbm/results.json) · [验证记录](hbm/verification.json) · [运行计划](hbm/plan.json) · [机制验证](hbm/mechanism-verification.json)

```bash
bin/sue build
python3 simulators/sue/harness/v2/verify_hbm.py results/sue/FRESH-hbm-tests
python3 simulators/sue/harness/v2/run_hbm_study.py results/sue/FRESH-hbm-study
PYTHONPATH=/tmp/sue-communication-deps:/tmp/sue-report-deps python3 simulators/sue/harness/v2/hbm_report.py results/sue/FRESH-hbm-study
```

独立运行offload时添加`--hbm-profile configs/hardware/hbm/fair-share.json`；省略则使用旧FIFO。绘图需要matplotlib，HTML需要markdown-it-py；这些依赖不参与仿真。
'''
 (root/'chapter.md').write_text(text)
 from markdown_it import MarkdownIt
 body=MarkdownIt('commonmark').enable('table').render(text)
 body=body.replace('src="hbm/hbm-comparison.svg"','src="data:image/svg+xml;base64,'+base64.b64encode((root/'hbm-comparison.svg').read_bytes()).decode()+'"').replace('href="hbm/','href="').replace('href="examples/hbm-fair-share.json"','href="hbm-fair-share.json"')
 (root/'hbm-fair-share.json').write_text(json.dumps(results[-1]['hbm_profile'],indent=2))
 (root/'report.html').write_text('<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>HBM公平共享对照</title><style>body{max-width:1200px;margin:30px auto;padding:0 20px;font:16px/1.8 system-ui;color:#19334b}img{max-width:100%}table{border-collapse:collapse;display:block;overflow:auto}td,th{padding:8px;border:1px solid #cbd5df}pre{overflow:auto;background:#eef3f7;padding:16px}</style>'+body+'</html>')
 dump(root/'verification.json',dict(status='passed',cases=len(results),pairs=len(pairs),historical_fifo_exact=len(pairs),checks=['input manifest SHA','same tasks/resources/routes/offload bytes/PCIe addresses across HBM policies','historical FIFO task/resource/port/PCIe stats exact','run-level DAG, delivery, PCIe and HBM capacity/byte-integral audits passed']))
 print(json.dumps(compare,indent=2),flush=True)
if __name__=='__main__':main(sys.argv[1])
