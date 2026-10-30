"""SUE case communication observability: raw-evidence checks, static plots, offline dashboard."""
import csv,json,sys,math,hashlib,html,statistics,zipfile
from pathlib import Path
from collections import defaultdict,Counter
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties
from plotly.offline import get_plotlyjs
from run import dump,csvread,csvwrite,sha
FONT=FontProperties(fname='/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
plt.rcParams.update({'font.family':FONT.get_name(),'axes.unicode_minus':False,'svg.fonttype':'none','font.size':10})
COLORS=['#167da5','#db8633','#7758b8','#299a7b','#c14c67']
GUIDES=[
 ('card','01-card-matrix','卡间通信矩阵','目的芯片 / MPU','源芯片 / MPU','KiB','逐任务网络有效载荷按源/目的芯片汇总；包含MoE及Engram请求/返回，不含本地UCIe及协议控制帧。颜色是字节数，不是带宽。','检查MoE是否分布到多张卡，以及MPU的集中读服务方向。矩阵不包含等待时间，不能单凭颜色判断拥塞。'),
 ('die','02-die-dispatch','Die间MoE dispatch矩阵','目的rank','源rank','KiB','只选dispatch任务；跨卡任务取bytes，同卡UCIe取resource.units与peer_die；同Die直接使用不计搬运字节。','64个发送Die分散工作，矩阵每4行/列对应一张卡。专家热点和TP特征分片共同决定矩阵；不能把一格当一条物理链路。'),
 ('port','03-port-busy','端点发送端口忙碌率','轨 / 端口号','Die rank；最后两行为MPU','%','host侧tx_busy_ps / 仿真结束时间，统计从0开始，含首次注入前1μs及控制帧；不含IFG等空闲时间。','看同一Die的8轨是否均匀。平均忙碌率低不代表没有短突发，也不是有效应用吞吐率。'),
 ('rail','04-rail-time','交换机出口随时间的平均忙碌率','仿真时间（μs）','轨 / 交换机','%','相邻500ns快照累计tx_busy_ps之差除以真实区间长度，再平均同轨66个交换机出口；最后区间可能短于500ns。','时间轴能看出Engram、公共处理和MoE产生的通信波段。66口平均会稀释单端口热点，须结合队列图。'),
 ('queue','05-egress-queue','交换机出口队列热点','仿真时间（μs）','目的rank；最后两行为MPU','KiB','500ns快照读取设备发送队列8个VC的字节总和；每目的Die在8个轨中取最大队列。不是交换机全部缓存，不含正在发送帧/重传控制队列或上游pending帧。','可以定位微突发排队出现在哪个目的Die与时间。采样峰值是实际峰值下界，可能漏过短于500ns的尖峰。'),
 ('credit','06-credit-pressure','CBFC信用占用快照','仿真时间（μs）','目的rank；最后两行为MPU','%','交换机发送侧CU/CL；对每设备8个VC取最大，再对目的Die的8轨取最大。读取getter，不调用会更新计数的CanSend。','占用高表示在途信用多，并不自动等于阻塞。只有结合credit_exhausted等证据才判断信用门控；本例该失败检查计数为0。'),
 ('cdf','07-latency-cdf','网络任务完成时延分布','任务时延（μs）','累计比例','', '各网络任务(end_ps-start_ps)/1e6，分dispatch、combine、Engram请求和回传；AW+W任务包含B返回；R聚合任务还包含等待后续R就绪的时间。不是单包传播时延。','比较方向与阶段的尾部；不同阶段消息大小不同，不能单凭CDF给协议排名。零字节计算/资源任务不纳入。'),
 ('scatter','08-size-latency','消息大小与完成时延','任务有效载荷（KiB）','任务时延（μs）','', '每点一个网络任务，横轴逻辑有效载荷，纵轴事务完成时延；颜色按四个阶段区分。','相近大小但时延不同，提示排队、路径与同步背景不同；同一任务可能分片，不能把散点数当包数。'),
 ('expert','09-expert-load','专家owner负载分布','owner rank','次数 / 个数','', 'token_routes.csv逐条(token,expert)分配计数，合并两步；同时展示两步内不同专家ID数量。专家复用概率0.75，不是独立同分布两步。','检查合成路由负载是否均匀；不是所有Die计算量必然一样，Expert数相同也不保证token命中均匀。'),
 ('ring','10-ring-bytes','UCIe有向边流量','有向环边','芯片','MiB','ring_statistics.csv的bytes；每次经过一条边计一次，含TP集合通信及卡内MoE搬运。两跳会被计两次。','方向不对称来自ring算法及任务路径，不等同于链路缺失。不能与应用端到端字节直接求和。'),
 ('resources','11-resource-busy','计算与HBM资源忙碌率','Die rank','时间占比（%）','', '计算使用服务时间之和；HBM使用hbm_dies.csv的busy_ps（多个源并发时不重复计时），各自除以仿真结束时间。','用于把算力空闲、HBM服务与网络活动区分。当前这是行为模型的资源占用，不是硬件GPU profiler实测。'),
 ('mpu','12-mpu-waiting','MPU内存事务的等待数量','仿真时间（μs）','等待事务数','', 'AXI模式按io_mpu_requests.csv的[submitted_ps,bandwidth_start_ps)统计：含等槽位、200ns访问延迟及等待带宽，500ns采样；已经开始带宽服务的事务不计入。','这是MPU内存服务队列，不是SUE交换机队列。两者对照可解释网络并不拥塞但远程读取仍慢。')]
PHASES=['dispatch','combine','engram:request','engram:data-response']

def extract(r):
 run=Path(r['run']);tasks=json.loads((run/'tasks.json').read_text());stats=csvread(run/'task_statistics.csv');resources=csvread(run/'resource_statistics.csv');ports=csvread(run/'port_statistics.csv');rings=csvread(run/'ring_statistics.csv');routes=csvread(run/'token_routes.csv');T=r['sim_end_us'];end=T*1e6
 assert len(stats)==len(tasks) and all(x['status']=='complete' and x['bytes']==x['received_bytes'] for x in stats)
 manifest=json.loads((run/'command-manifest.json').read_text())
 for f,h in manifest['inputs'].items():assert sha(run/f)==h,(f,'hash')
 card=np.zeros((18,18));die=np.zeros((64,64));port=np.zeros((66,8));ring=np.zeros((16,8));compute=np.zeros(64);hbm=np.zeros(64);counts=np.zeros(64);distinct=[set() for _ in range(64)];phase=defaultdict(list)
 from io_die_config import managed_edges
 streaming_edges=managed_edges(run)
 stat={int(x['id']):x for x in stats}
 for t in tasks:
  s=stat[t['id']];label=t['label'].split(':',1)[1]
  for parent in t['deps']:
   if (t['id'],parent) in streaming_edges:continue
   assert int(s['start_ps'])>=int(stat[parent]['data_end_ps' if parent in t.get('receive_deps',[]) else 'end_ps'])
  if t['bytes']:
   card[t['src_chip'],t['dst_chip']]+=t['bytes'];phase[label].append([t['bytes']/1024,(int(s['end_ps'])-int(s['start_ps']))/1e6])
   if label=='dispatch':die[t['src_chip']*4+t['src_die'],t['dst_chip']*4+t['dst_die']]+=t['bytes']
 for row in resources:
  k=int(row['kind']);chip=int(row['chip']);d=int(row['die']);task=tasks[int(row['id'])]
  if k in (0,1):(compute if k==0 else hbm).__setitem__(chip*4+d,(compute if k==0 else hbm)[chip*4+d]+int(row['service_ns'])/1000/T*100)
  if k==3 and task['label'].endswith(':dispatch'):
   original=next(x for x in json_resources_cache(run) if x['id']==task['id']);die[chip*4+d,chip*4+original['peer_die']]+=int(row['units'])
 if (run/'hbm_dies.csv').exists():
  for row in csvread(run/'hbm_dies.csv'):hbm[int(row['chip'])*4+int(row['die'])]=int(row['busy_ps'])/end*100
 for row in ports:
  if row['side']=='0':
   rank=int(row['chip'])*4+int(row['die']) if int(row['chip'])<16 else 64+int(row['chip'])-16
   port[rank,int(row['port'])]=int(row['tx_busy_ps'])/end*100
 for row in rings:
  a,b=int(row['from_die']),int(row['to_die']);idx=a*2+(0 if b==(a+1)%4 else 1);ring[int(row['chip']),idx]=int(row['bytes'])/2**20
 for row in routes:
  rank=int(row['owner_rank']);counts[rank]+=1;distinct[rank].add(int(row['expert']))
 # Cumulative wire busy time is clipped to the current sample boundary by C++.
 previous={};last_sample={};times=[];rail=[];queue=[];credit=[];host_q=[];switch_q=[];current=None
 with (run/'port_timeseries.csv').open() as f:
  for row in csv.DictReader(f):
   now=int(row['time_ps'])
   if now!=current:
    current=now;times.append(now/1e6);rail.append([0.0]*8);queue.append([0]*66);credit.append([0.0]*66);host_q.append(0);switch_q.append(0)
   chip,d,p,side=map(int,(row['chip'],row['die'],row['port'],row['side']));key=chip,d,p,side;busy=int(row['tx_busy_ps']);q=int(row['egress_bytes']);rank=chip*4+d if chip<16 else 64+chip-16
   if key in previous:
    then,old=previous[key];fraction=(busy-old)/(now-then)
    assert -1e-9<=fraction<=1+1e-9,(key,fraction)
    if side==1:rail[-1][int(row['switch'])]+=fraction*100/66
   previous[key]=(now,busy);last_sample[key]=busy
   if side==1:
    queue[-1][rank]=max(queue[-1][rank],q);credit[-1][rank]=max(credit[-1][rank],float(row['credit_used_fraction_max'])*100);switch_q[-1]+=q
   else:host_q[-1]+=q
 for row in ports:
  key=tuple(int(row[k]) for k in ('chip','die','port','side'));assert last_sample[key]==int(row['tx_busy_ps'])
 assert abs(times[-1]-T)<1e-8
 rail=np.array(rail).T;queue=np.array(queue).T/1024;credit=np.array(credit).T;times=np.array(times);waiting=np.zeros((2,len(times)))
 for row in resources:
  if int(row['kind'])==2:
   taskstat=stat[int(row['id'])];start=int(taskstat['start_ps'])/1e6;finish=int(row['service_start_ps'])/1e6
   waiting[int(row['chip'])-16]+=((times>=start)&(times<finish))
 if (run/'io_mpu_requests.csv').exists():
  waiting[:]=0
  for row in csvread(run/'io_mpu_requests.csv'):
   start=int(row['submitted_ps'])/1e6;finish=int(row['bandwidth_start_ps'])/1e6
   waiting[int(row['chip'])-16]+=((times>=start)&(times<finish))
 # Independent integral check for all switch directions, including final partial bin.
 expected=sum(int(p['tx_busy_ps']) for p in ports if p['side']=='1')
 actual=float(np.sum(rail[:,1:]*np.diff(times)[None,:]*1e6/100*66));assert math.isclose(actual,expected,rel_tol=1e-10)
 assert int(card.sum())==r['logical_bytes']==r['received_bytes']
 assert counts.sum()==len(routes)==64*6*2
 evidence={p.name:sha(p) for p in run.iterdir() if p.name in ['port_timeseries.csv','task_statistics.csv','port_statistics.csv','ring_statistics.csv','resource_statistics.csv','token_routes.csv','tasks.json','resources.json','command-manifest.json']}
 metrics=dict(seed=r['seed'],window_us=r['makespan_us'],observed_end_us=T,network_tasks=sum(int(x['bytes'])>0 for x in stats),app_bytes=int(card.sum()),max_sampled_switch_queue_KiB=float(queue.max()),max_sampled_switch_credit_pct=float(credit.max()),max_rail_average_busy_pct=float(rail.max()),max_host_port_average_busy_pct=float(port.max()),credit_exhausted_checks=r['credit_exhausted_checks'],replay_frames=r['replay_frames'],discard=r['discard'],queue_blocked_polls=r['queue_blocked_polls'],max_mpu_waiting_tasks=float(waiting.max()),max_hbm_busy_pct=float(hbm.max()),max_compute_busy_pct=float(compute.max()),expert_assignments_min=int(counts.min()),expert_assignments_max=int(counts.max()),source_run=str(run),evidence=evidence)
 arrays=dict(card=card/1024,die=die/1024,port=port,rail=rail[:,1:],queue=queue,credit=credit,ring=ring,times=times,phase=dict(phase),expert=counts,distinct=np.array([len(x) for x in distinct]),compute=compute,hbm=hbm,waiting=waiting)
 return arrays,metrics
_RESOURCE_CACHE={}
def json_resources_cache(run):
 if str(run) not in _RESOURCE_CACHE:_RESOURCE_CACHE[str(run)]=json.loads((run/'resources.json').read_text())
 return _RESOURCE_CACHE[str(run)]

def specs(a):
 out=[]
 for key,slug,title,xlab,ylab,unit,method,meaning in GUIDES:
  traces=[];layout=dict(title=dict(text=title,font=dict(size=19)),xaxis=dict(title=dict(text=xlab)),yaxis=dict(title=dict(text=ylab)),margin=dict(l=90,r=35,t=65,b=65),height=440,paper_bgcolor='white',plot_bgcolor='white',font=dict(family='Noto Sans CJK SC, sans-serif',color='#18364a'),legend=dict(orientation='h',y=-.2),hovermode='closest')
  if key in ('card','die','port','rail','queue','credit','ring'):
   z=a[key];x=list(range(z.shape[1]));y=list(range(z.shape[0]));scale='YlGnBu'
   if key in ('rail','queue','credit'):x=a['times'][1:].tolist() if key=='rail' else a['times'].tolist()
   if key=='card':x=y=[f'XPU{i}' for i in range(16)]+['MPU0','MPU1']
   if key=='ring':x=['0→1','0→3','1→2','1→0','2→3','2→1','3→0','3→2']
   if key in ('queue','credit'):scale='YlOrRd'
   traces=[dict(type='heatmap',z=np.round(z,5).tolist(),x=x,y=y,colorscale=[[i/8,matplotlib.colors.to_hex(plt.get_cmap(scale)(i/8))] for i in range(9)],zmin=0,colorbar=dict(title=dict(text=unit)),hovertemplate='x=%{x}<br>y=%{y}<br>值=%{z:.3f} '+unit+'<extra></extra>')]
   layout['yaxis']['autorange']='reversed'
  elif key in ('cdf','scatter'):
   for i,name in enumerate(PHASES):
    values=a['phase'].get(name,[])
    if not values:continue
    values=np.array(values);dur=np.sort(values[:,1]);traces.append(dict(type='scatter',mode='lines' if key=='cdf' else 'markers',name=name,x=(dur if key=='cdf' else values[:,0]).tolist(),y=(np.arange(1,len(dur)+1)/len(dur) if key=='cdf' else values[:,1]).tolist(),marker=dict(color=COLORS[i],size=5,opacity=.55),line=dict(color=COLORS[i],width=2)))
  elif key in ('expert','resources'):
   for i,(name,values) in enumerate([('token-expert分配',a['expert']),('不同专家ID',a['distinct'])] if key=='expert' else [('计算',a['compute']),('HBM',a['hbm'])]):
    traces.append(dict(type='bar',name=name,x=list(range(64)),y=np.round(values,4).tolist(),marker=dict(color=COLORS[i])))
   layout['barmode']='group'
  else:
   for i in range(2):traces.append(dict(type='scatter',mode='lines',name=f'MPU{i}',x=a['times'].tolist(),y=a['waiting'][i].tolist(),line=dict(color=COLORS[i],shape='hv')))
  out.append(dict(id=slug,data=traces,layout=layout,method=method,meaning=meaning,key=key))
 return out

def static_plot(spec,path):
 fig,ax=plt.subplots(figsize=(11.5,4.7));fig.patch.set_facecolor('white');unit=next(x[5] for x in GUIDES if x[1]==spec['id'])
 for t in spec['data']:
  if t['type']=='heatmap':
   z=np.array(t['z']);im=ax.imshow(z,aspect='auto',interpolation='nearest',cmap='YlOrRd' if spec['key'] in ('queue','credit') else 'YlGnBu',vmin=0);fig.colorbar(im,ax=ax,label=unit,pad=.02)
   for axis,vals in [('x',t['x']),('y',t['y'])]:
    n=len(vals);idx=np.unique(np.linspace(0,n-1,min(n,18 if spec['key']=='card' else 10),dtype=int));labels=[f'{vals[j]:.1f}' if isinstance(vals[j],float) else str(vals[j]) for j in idx]
    getattr(ax,'set_'+axis+'ticks')(idx,labels,rotation=35 if axis=='x' and spec['key']=='card' else 0)
  elif t['type']=='bar':
   i=spec['data'].index(t);ax.bar(np.array(t['x'])+(i-.5)*.4,t['y'],width=.4,label=t['name'],color=t['marker']['color'])
  elif t['mode']=='markers':ax.scatter(t['x'],t['y'],s=7,alpha=.35,label=t['name'],color=t['marker']['color'],rasterized=True)
  else:ax.plot(t['x'],t['y'],label=t['name'],color=t['line']['color'],linewidth=1.4,drawstyle='steps-post' if spec['key']=='mpu' else 'default')
 if spec['data'][0]['type']!='heatmap':ax.legend(fontsize=9,frameon=False);ax.grid(alpha=.15)
 ax.set(title=spec['layout']['title']['text']+' · seed 1',xlabel=spec['layout']['xaxis']['title']['text'],ylabel=spec['layout']['yaxis']['title']['text']);fig.tight_layout();fig.savefig(path.with_suffix('.svg'));fig.savefig(path.with_suffix('.png'),dpi=160);plt.close(fig)

def main():
 root=Path(sys.argv[1]).resolve();out=root/'report';out.mkdir(exist_ok=True);figures=out/'figures';figures.mkdir(exist_ok=True)
 rs=[r for r in json.loads((root/'results.json').read_text()) if r['sample_ns']];payload={};metrics=[]
 for r in rs:
  a,m=extract(r);s=specs(a);payload[str(r['seed'])]=s;metrics.append(m)
  if r['seed']==1:
   for spec in s:static_plot(spec,figures/spec['id'])
   dump(out/'figure-data-seed1.json',s)
 dump(out/'metrics.json',metrics);dump(out/'plot-data.json',payload)
 csvwrite(out/'metrics.csv',[{k:v for k,v in m.items() if k not in ('evidence','source_run')} for m in metrics])
 cards=[]
 for key,slug,title,xlab,ylab,unit,method,meaning in GUIDES:
  cards.append(f'<section id="section-{slug}"><h2>{html.escape(title)}</h2><div class="plot" id="{slug}"></div><p><b>怎么测：</b>{html.escape(method)}</p><p><b>怎么看：</b>{html.escape(meaning)}</p><a href="figures/{slug}.svg" download>下载静态SVG（seed1）</a> · <a href="figures/{slug}.png" download>下载静态PNG（seed1）</a></section>')
 body='''<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DeepSeek 16卡通信与拥塞观测</title><style>*{box-sizing:border-box}body{margin:0;background:#f1f5f9;color:#19354b;font:16px/1.8 system-ui,sans-serif}header{padding:30px max(20px,calc((100vw - 1200px)/2));background:#17354b;color:white}main{max-width:1240px;margin:auto;padding:20px}section{background:white;border:1px solid #dde5ed;border-radius:12px;padding:20px;margin:22px 0}.plot{width:100%;min-height:440px}h1{font-size:28px}h2{font-size:21px}a{color:#14769d}select,button{padding:8px;border-radius:5px}.controls{position:sticky;top:0;background:#e5eef4;padding:14px;z-index:5}.metric{display:inline-block;margin:6px 20px 6px 0}p{margin:10px 0}.note{background:#fff4dc;padding:14px;border-radius:8px}nav a{display:inline-block;margin:3px 12px 3px 0}@media(max-width:600px){main{padding:8px}section{padding:8px}.plot{min-height:440px}}</style><header><h1>DeepSeek 16卡：通信、队列与资源观测</h1><div>TP4 × DP16 · EP64 · 2 MPU · Batch64 · Context32K · 两步代表块</div></header><main><div class="controls">种子 <select id="seed"><option>1</option><option>2</option><option>3</option></select>　<button id="csv">导出指标CSV</button>　<a href="communication-evidence.zip">下载通信证据包</a><div id="metrics"></div></div><p class="note">采样500ns；队列为瞬时快照，可能漏过更短尖峰。链路负载含控制帧，通信矩阵只计有效载荷。0→仿真结束为统计区间，含首次注入前1μs。网络队列与MPU服务队列分开解释；本例不是完整模型测速。</p><nav>'''+''.join(f'<a href="#section-{g[1]}">{html.escape(g[2])}</a>' for g in GUIDES)+'</nav>'+''.join(cards)+'</main>'
 js='''const DATA=__DATA__,METRICS=__METRICS__;function render(){const seed=document.getElementById('seed').value;const m=METRICS.find(x=>x.seed==seed);document.getElementById('metrics').innerHTML=[['窗口',m.window_us.toFixed(3)+' μs'],['最大出口队列快照',m.max_sampled_switch_queue_KiB.toFixed(2)+' KiB'],['最大信用占用',m.max_sampled_switch_credit_pct.toFixed(2)+'%'],['信用不足检查',m.credit_exhausted_checks],['重传帧',m.replay_frames]].map(x=>'<span class="metric"><b>'+x[0]+'</b> '+x[1]+'</span>').join('');for(const f of DATA[seed])Plotly.react(f.id,f.data,f.layout,{responsive:true,displaylogo:false,toImageButtonOptions:{format:'svg',filename:f.id}});}document.getElementById('seed').onchange=render;document.getElementById('csv').onclick=()=>{const keys=Object.keys(METRICS[0]).filter(k=>!['evidence','source_run'].includes(k));const text=[keys.join(','),...METRICS.map(m=>keys.map(k=>m[k]).join(','))].join('\\n');const u=URL.createObjectURL(new Blob([text],{type:'text/csv'})),a=document.createElement('a');a.href=u;a.download='communication-metrics.csv';a.click();setTimeout(()=>URL.revokeObjectURL(u),1000);};render();'''.replace('__DATA__',json.dumps(payload,ensure_ascii=False).replace('</','<\\/')).replace('__METRICS__',json.dumps(metrics,ensure_ascii=False))
 (out/'report.html').write_text(body+'<script>'+get_plotlyjs()+'</script><script>'+js+'</script></html>')
 dump(out/'verification.json',{'status':'passed','seeds':3,'figures':len(GUIDES),'checks':['complete payload delivery and DAG dependencies','input hashes','matrix bytes equal delivered payload','wire busy deltas within [0,1]','last sample matches all port totals','integrated rail busy equals cumulative switch tx busy','expert assignments equal 64*6*2','observer effect control in parent verification.json']})
 with zipfile.ZipFile(out/'communication-evidence.zip','w',zipfile.ZIP_DEFLATED) as z:
  for p in [root/'plan.json',root/'verification.json',out/'metrics.json',out/'metrics.csv',out/'verification.json',out/'figure-data-seed1.json']:
   z.write(p,'analysis/'+('observation-'+p.name if p.parent==root else p.name))
  for r in rs:
   run=Path(r['run'])
   for p in run.iterdir():
    if p.is_file() and p.suffix in ('.csv','.json','.log','.conf','.tsv'):z.write(p,f'seed-{r["seed"]}/'+p.name)
 print(json.dumps({'figures':len(GUIDES),'report':str(out/'report.html'),'metrics':metrics},ensure_ascii=False))
if __name__=='__main__':main()
