import sys,json,statistics,shutil,csv,hashlib
from pathlib import Path
sys.path.insert(0,str(Path('simulators/sue/harness/v2').resolve()))
from offload_report import aggregate
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path('/data/scale_up_evaluation-mpu128-20261030'); study=root/'offload'; here=Path('docs/manual');dest=here/'offload128';dest.mkdir(exist_ok=True)
assert not (study/'analysis/results.json').exists(), 'Full study exists: use build_manual.py; do not overwrite it with the meeting snapshot'
rs=json.loads((study/'matrix/results.json').read_text());assert len(rs)==275 and all(r['status']=='passed' for r in rs)
rows=aggregate(rs);old=json.loads(Path('/data/scale_up_evaluation-rerun-20261030/offload/analysis/results.json').read_text());oi={r['case_index']:r for r in old}
for r in rs:
 p,q=Path(r['run']),Path(oi[r['case_index']]['run'])
 for n in ['tasks.json','resources.json','token_routes.csv','sue.conf','pcie.json','hbm.json']:
  assert hashlib.sha256((p/n).read_bytes()).digest()==hashlib.sha256((q/n).read_bytes()).digest()
 if r['backend']=='pcie':assert r['block_us']==oi[r['case_index']]['block_us']
order=[('glm-5.3','kv','GLM 5.3 · KV'),('kimi-k3','kv','Kimi K3 · KV'),('deepseek-v4.1-flash-1017','kv','DeepSeek · KV'),('deepseek-v4.1-flash-1017','engram','DeepSeek · Engram'),('deepseek-v4.1-flash-1017','both','DeepSeek · KV + Engram')];counts=[1,2,4,8,16]
plt.rcParams.update({'font.size':11,'axes.spines.top':False,'axes.spines.right':False,'svg.fonttype':'none'})
fig,axs=plt.subplots(2,3,figsize=(17,9));table=[];selected=[];pairs=[]
for ax,(model,target,title) in zip(axs.flat,order):
 base=statistics.mean(r['block_us'] for r in rs if (r['model'],r['target'],r['backend'])==(model,target,'pcie'))
 data={k:[] for k in ['mpu','hybrid','best']}
 for m in counts:
  group=[r for r in rows if (r['model'],r['target'],r['mpus'],r['variant'])==(model,target,m,'main')]
  a=next(r for r in group if r['backend']=='mpu');b=next(r for r in group if r['backend']=='hybrid')
  candidates=[r for r in rs if (r['model'],r['target'],r['mpus'],r['backend'],r['seed'])==(model,target,m,'hybrid',1)]
  best=min(candidates,key=lambda r:r['block_us']);fraction=best.get('mpu_fraction',190*m/(2048+190*m))
  b1=next(r['block_us'] for r in rs if (r['model'],r['target'],r['backend'],r['seed'])==(model,target,'pcie',1))
  data['mpu'].append(a['speedup_vs_PCIe']);data['hybrid'].append(b['speedup_vs_PCIe']);data['best'].append(b1/best['block_us'])
  table.append(dict(workload=title,mpus=m,pcie_us=base,mpu_us=a['mean_block_us'],nominal_us=b['mean_block_us'],best_measured_us=best['block_us'],selected_share=fraction,mpu_seeds=a['seeds'],nominal_seeds=b['seeds']))
  selected.append(dict(model=model,target=target,mpus=m,case_index=best['case_index'],share=fraction,seed=1))
 ax.axhline(1,color='#64748b',ls='--',lw=1.3,label='PCIe baseline')
 for key,label,color in [('mpu','MPU only','#d97706'),('hybrid','Nominal share','#2563eb'),('best','Best measured share','#059669')]:ax.plot(range(5),data[key],'-o',label=label,color=color,lw=2.3,ms=5)
 if target=='both':
  tier=[next(r for r in rows if (r['model'],r['target'],r['mpus'],r['backend'],r['variant'])==(model,target,m,'tiered','main'))['speedup_vs_PCIe'] for m in counts]
  ax.plot(range(5),tier,'-s',color='#9333ea',lw=2,label='Engram MPU + KV PCIe')
 ax.set_xticks(range(5),counts);ax.set_xlabel('MPU count');ax.set_ylabel('Speedup vs PCIe');ax.set_title(title,fontweight='bold',loc='left');ax.grid(axis='y',alpha=.18)
 new=table[-1]['mpu_us'];oldmean=statistics.mean(r['block_us'] for r in old if (r['model'],r['target'],r['backend'],r['mpus'])==(model,target,'mpu',16))
 pairs.append((title,oldmean,new,base,(new/oldmean-1)*100))
axs.flat[5].axis('off');handles,labels=axs.flat[4].get_legend_handles_labels();axs.flat[5].legend(handles,labels,loc='center',frameon=False,fontsize=15)
axs.flat[5].text(.5,.10,'128 in-flight AXI requests / MPU\n16 XPU · 256 B / AXI · 1 ns clock\nHigher is better',ha='center',va='center',transform=axs.flat[5].transAxes,color='#475569',linespacing=1.8)
fig.suptitle('MPU offload performance | 128-request concurrency',fontsize=22,fontweight='bold',x=.055,ha='left');fig.tight_layout(rect=[0,0,1,.94]);fig.savefig(dest/'offload-speedup.svg',bbox_inches='tight');fig.savefig(dest/'offload-speedup.png',dpi=170,bbox_inches='tight');plt.close(fig)
for name,obj in [('results.json',rs),('summary.json',rows),('selection.json',selected),('delivery.json',json.loads((root/'expedited-delivery.json').read_text()))]: (dest/name).write_text(json.dumps(obj,ensure_ascii=False,indent=2))
with (dest/'comparison.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(table[0]));w.writeheader();w.writerows(table)
shutil.copy2(study/'hardware-profile.json',dest/'hardware-profile.json')
chapter=here/'09-mpu-pcie-offload.md';previous=chapter.read_text();tail='## 9.9'+previous.split('## 9.9',1)[1]
if not (root/'chapter9-before-expedited.md').exists():shutil.copy2(chapter,root/'chapter9-before-expedited.md')
text='''# 9. MPU与PCIe卸载对比

## 9.1 实验配置

本轮将每MPU的AXI在途上限改为**128条**。每请求256 B，200 ns内存访问延迟允许重叠，内部数据带宽仍以FIFO服务；每MPU单向190 GB/s。其余硬件、PCIe、HBM、网络、模型及路由配置与64条在途对照保持一致。

固定16颗XPU、每卡4 Die、八轨八交换机，Batch=64、上下文32768、EP64。GLM和DeepSeek使用TP4，Kimi使用TP32。PCIe每卡128 GB/s，每8卡共享双路Host；每路12通道DDR5-6400。HBM使用公平带宽共享；IO Die尽力拼包，全局及端点请求间隔1 ns。XPU/MPU单向接入延迟各400 ns，交换机转发预算300 ns。

## 9.2 测量口径

耗时为两步代表块窗口除以2，单位µs/代表块，并非完整模型TPOT或TTFT。本轮采用已完成的275组结果；主曲线使用可用种子均值，Kimi纯MPU的1/2节点分别为1/2个种子，其余主对比为3个种子。分流曲线使用seed1已完成候选中最快的比例，本轮不追加独立种子重复确认。Kimi的1 MPU扫描候选为12.5%、25%及nominal，其余节点包含12.5%、25%、50%、75%及nominal。不同曲线按各自参与种子对应的PCIe基线归一化。

## 9.3 MPU数量与分流策略

![128条在途：五种工作负载的完整MPU数量对比](offload128/offload-speedup.svg)

- **MPU only**：全部卸载数据由MPU提供。
- **Nominal share**：MPU比例为190×M/(2048+190×M)，两后端并行服务互不重复的数据。
- **Best measured share（已测最佳分流）**：各节点数分别从已完成扫描中选取最快比例，不代表全局最优。
- **Engram MPU + KV PCIe**：Engram放MPU，KV通过PCIe读取。
- **纵轴**：对应PCIe耗时/方案耗时，大于1表示优于PCIe；连线连接1、2、4、8、16 MPU实测点。

[下载高清PNG](offload128/offload-speedup.png) · [下载SVG](offload128/offload-speedup.svg) · [结果CSV](offload128/comparison.csv)

## 9.4 128条与64条在途：16 MPU对照

以下五组均使用相同三个种子，单位µs/代表块。

|负载|64条|128条|PCIe|耗时变化|
|---|---:|---:|---:|---:|
'''
for label,a,b,c,d in pairs:text+=f'|{label}|{a:.3f}|{b:.3f}|{c:.3f}|{d:+.2f}%|\n'
text+='''
16 MPU下，五种负载中四种纯MPU方案已经快于PCIe；只有Engram单独卸载仍略慢。扩大并发主要改善KV大流量路径，Engram小请求的往返、收齐与消费依赖仍限制收益。

仅按延迟估算，128×256 B/200 ns=163.84 GB/s，仍低于单MPU的190 GB/s标称带宽；再加数据服务及网络开销，不能把标称总带宽直接当作应用有效带宽。增加并发不会使整个代表块耗时减半，因为计算、HBM、集合通信和数据依赖仍存在。

## 9.5 全部实测点与分流比例

|负载|MPU数|PCIe µs|纯MPU µs|Nominal µs|已测最佳 µs|选定MPU比例|
|---|---:|---:|---:|---:|---:|---:|
'''
for r in table:text+=f"|{r['workload']}|{r['mpus']}|{r['pcie_us']:.3f}|{r['mpu_us']:.3f}|{r['nominal_us']:.3f}|{r['best_measured_us']:.3f}|{r['selected_share']:.1%}|\n"
text+='''
## 9.6 如何解释曲线

横轴增加MPU同时改变总服务能力、数据分片和到达时序。Nominal比例随节点数增加；已测最佳分流也可能在各点选择不同的比例。因此曲线并不是固定流量划分下的单一带宽扩容实验，出现非单调变化不等于交换机拥塞，需要结合具体请求时序判断。

## 9.7 建模边界

MPU槽位仅限制已准入内存请求，等待准入队列尚未与SUE信用容量耦合；结果进入网络发送后不继续占用内存槽位。SUE收齐消息后再写HBM，尚不是逐包流式接收背压。KV按256 KiB逻辑消息拆为256 B AXI；Engram按目标节点聚合后拆分，不是真实逐地址随机访问trace。

## 9.8 结果与复现入口

275组执行结果均通过各自运行校验；本次另核对275组与64条对照的任务、资源、路由、网络、PCIe和HBM输入相同，15个PCIe基线耗时完全一致。未把未完成运行计入数据，也未用旧64条结果填补新曲线。

[每例结果及运行目录](offload128/results.json) · [汇总](offload128/summary.json) · [选定比例](offload128/selection.json) · [128条硬件JSON](offload128/hardware-profile.json)。原始证据保存在`results/sue/mpu128-20261030/offload/matrix/`。第8章仍是独立冻结案例，不使用本章数字替换。

'''+tail
chapter.write_text(text)
(dest/'chapter.md').write_text(text)
# Standalone report uses a portable local image and table; manual embeds the same SVG.
from markdown_it import MarkdownIt
body=MarkdownIt('commonmark').enable('table').render(text.split('## 9.9')[0].replace('offload128/',''))
(dest/'report.html').write_text('<!doctype html><meta charset="utf-8"><title>MPU 128条在途实验</title><style>body{max-width:1200px;margin:35px auto;padding:20px;font:16px/1.7 system-ui;color:#172b4d}img{width:100%}table{border-collapse:collapse;display:block;overflow:auto}td,th{padding:8px;border-bottom:1px solid #ddd}a{color:#2563eb}</style>'+body)
print('275 results; full 5-workload x 5-count plot and chapter generated')
