"""Render speculative-decoding assumptions separately from measured packet cycles."""
import json,html,numpy as np

def section(path,table):
 cfg=json.loads((path/'experiment.json').read_text());v=json.loads((path/'verification.json').read_text());assert v['status']=='passed' and v['failed']==0
 rows=json.loads((path/'results.json').read_text());est=json.loads((path/'estimates.json').read_text());base=rows[0]['cycle_ms'];main=[r for r in est if r['draft_multiplier']==1]
 s='<section id="mtp"><h2>4. 加入MTP：B=1、TP8的性能评估</h2><p><b>这是一条请求的推测解码情景评估。</b>卡数仍为8，单卡四Die做TP，总计32个TP rank。每轮先由草拟模型提出d个候选，再由主模型一次验证d＋1个位置；验证位置数不是用户请求batch size。所有阶段的卡间消息均执行SUE逐包仿真。</p>'
 s+='<p><a href="https://arxiv.org/html/2607.24653v1">Kimi K3技术报告§4.1.4、§5.4.2</a>描述了由一层MTP微调的EAGLE-3草拟模型及KDA状态恢复。当前保存的<a href="https://huggingface.co/moonshotai/Kimi-K3/blob/main/config.json">主模型config</a>中num_nextn_predict_layers=0，未提供草拟权重。本节采用一个MLA＋MoE主干块、3H→H特征融合投影和输出头作为草拟成本代理，不能称为实际草拟模型测速。</p><h3>如何测：一次草拟—验证周期</h3><ol><li>串行草拟1、2或4个候选；复用同一个假设草拟层，每次承担本地读取、计算、片内归约及3次卡间Ring归约。</li><li>主模型93层验证2、3或5个位置。普通矩阵允许位置间权重复用，路由专家按各位置的实际合成选择计读取量；各位置的KV和KDA状态访问保守地分别计费，没有把同序列KV复用当作免费收益。</li><li>计入一次KDA状态快照及最多d个位置的恢复重放预算。用最坏接受前缀预算统一各接受率，不模拟真实KDA内核、树状分支和动态回滚；延时可能偏保守。</li><li>每个配置执行两轮，取第二轮周期时间。容量按一条请求加草拟层、特征融合和临时状态预算检查。原SUE协议核心不变，固定网络路径仍为2 μs。</li></ol>'
 s+=table(['每轮草拟d','验证位置数','草拟本地 / ms','主模型本地 / ms','状态恢复 / ms','逐包网络 / ms','完整周期 / ms'],[[r['depth'],r['verification_positions']]+[f'{r[k]:.3f}' for k in ('draft_local_ms','verify_local_ms','recovery_ms','network_ms','cycle_ms')] for r in rows if r['draft_multiplier']==1])
 if cfg['mtp'].get('public_acceptance'):
  pub=cfg['mtp']['public_acceptance'];r=next(x for x in rows if x['depth']==pub['draft_depth'] and x['draft_multiplier']==1)
  s+='<h3>公开数据：以EAGLE3实际接受长度作参照</h3><p><a href="'+pub['source_url']+'">LightSeek EAGLE3-MLA模型卡</a>给出草拟3个、验证4个位置的接受长度，测量条件是NVFP4 Kimi-K3、GB300 TP8、TokenSpeed，temperature=1.0、top_p=0.95、reasoning effort=max。以下直接使用发布的接受长度，不倒推出一个固定α，也不将4理解成4个额外候选。</p>'
  s+=table(['公开工作负载','平均接受长度L','本仿真周期/L ms/token','相对无MTP'],[[x['benchmark'],f"{x['accept_length']:.3f}",f"{r['cycle_ms']/x['accept_length']:.3f}",f"{base*x['accept_length']/r['cycle_ms']:.2f}×"] for x in pub['data']])
  s+=f'<p>本仿真草拟3个的周期为{r["cycle_ms"]:.3f} ms；表中是“仿真周期÷外部接受长度”的条件估计，并非GB300实测速度。源数据按请求统计接受长度，不是本案例的逐轮轨迹；工作负载、目标精度与硬件也不同。草拟成本仍沿用代理模型，没有把LightSeek草拟权重实际部署到模拟芯片。此前60%/80%/90%仅保留为敏感性假设。</p>'
  s+='<p>另有<a href="https://huggingface.co/Inferact/Kimi-K3-DSpark">Inferact DSpark公开模型卡</a>：7个候选、temperature=1.0时，SPEED-Bench coding接受长度4.22，MT-Bench为3.06。DSpark采用5层dense并行草拟，与本节串行单层EAGLE代理不同，因此不混用其接受长度计算本节加速比。</p>'
 routes=np.load(path/'routes.npy');expert_rows=[]
 for depth in sorted(set(r['depth'] for r in rows)):
  counts=[len(np.unique(routes[1,l,:depth+1])) for l in range(1,93)]
  by=sum(counts)*17547264/32/1e9
  expert_rows.append([depth,depth+1,f'{np.mean(counts):.2f}',f'{by:.3f}',f'{by/.807174144:.2f}×'])
 s+='<h3>额外候选会增加专家权重读取，已计入周期</h3><p>每个验证位置每层选16个专家；实现先统计各专家被多少位置使用，再按ceil(次数/32)计算权重读取tile。相同专家在一次验证内可复用，未被接受的候选也已经完成计算和读取，不能在事后扣除其开销。草拟层自身的专家读取另计。</p>'
 s+=table(['草拟d','验证位置','每层不同专家数（平均）','主模型路由权重GB/Die/轮','相对单token'],expert_rows)
 s+='<p>统计来自第二轮92个MoE层的实际合成路由；只计路由专家权重，不含公共层、共享专家、KV或草拟层。当前候选位置独立均匀路由，未使用真实同一序列的专家相关性，真实重复率可能不同。读取字节按32个TP Die分摊；没有把所有候选假设成访问同一组专家。</p><p>因此MTP不是免费增加输出：公共矩阵与同步可被多个有效token摊薄，但路由专家工作量近似随验证宽度增加。接受长度增长慢于验证成本时，继续加深草拟可能降低收益。LightSeek的<a href="https://lightseek.org/blog/kimi-k3-draft-collection.html">公开评测分析</a>也指出额外验证位置会增加MoE计算、dispatch及KDA状态处理。</p>'
 s+='<h3>接受率决定摊到每个有效token的成本</h3><p>α表示“此前候选均已通过时，下一个候选也通过”的条件概率，假设各位置相同。每轮平均提交token数 E＝1＋α＋α²＋…＋αᵈ，其中1包含首次拒绝时的纠正token或全部通过时的bonus token。摊销时间＝周期时间/E，加速比＝无MTP的37.344 ms/摊销时间。这是解析期望，未运行文本模型或测量真实接受率。</p>'
 s+=table(['草拟d','假设α','平均提交token/轮','摊销ms/token','期望token/s','相对无MTP'],[[r['depth'],f'{r["alpha"]:.0%}',f'{r["expected_committed_tokens"]:.3f}',f'{r["amortized_ms_per_token"]:.3f}',f'{r["expected_token_s"]:.2f}',f'{r["speedup"]:.2f}×'] for r in main])
 # Small standalone vector chart; native tooltips retain numeric values offline.
 colors=['#2675b7','#2e967e','#df873e'];width=900;height=310;maxy=max(r['speedup'] for r in main)*1.12
 svg=f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="假设接受率与MTP加速比"><text x="65" y="22" font-size="14">相对无MTP加速比（越高越好）</text>'
 for j in range(5):
  val=j*maxy/4;y=250-val/maxy*205;svg+=f'<path d="M65 {y} H840" stroke="#e3eaf0"/><text x="50" y="{y+5}" text-anchor="end" font-size="12">{val:.1f}×</text>'
 for di,depth in enumerate([1,2,4]):
  rr=[r for r in main if r['depth']==depth];pts=[(120+i*320,250-r['speedup']/maxy*205) for i,r in enumerate(rr)];svg+=f'<polyline points="'+ ' '.join(f'{x},{y}' for x,y in pts)+f'" fill="none" stroke="{colors[di]}" stroke-width="3"/>'
  for (x,y),r in zip(pts,rr):svg+=f'<circle cx="{x}" cy="{y}" r="6" fill="{colors[di]}"><title>d={depth}, α={r["alpha"]:.0%}：{r["speedup"]:.3f}×，{r["amortized_ms_per_token"]:.3f} ms/token</title></circle>'
  svg+=f'<text x="{100+di*245}" y="305" fill="{colors[di]}" font-size="13">● 草拟{depth}个token</text>'
 for i,a in enumerate([.6,.8,.9]):svg+=f'<text x="{120+i*320}" y="275" text-anchor="middle" font-size="13">α={a:.0%}</text>'
 s+=svg+'</svg><p>图中只有接受率是假设扫描；每个d的完整周期来自实际执行的逐包网络和行为计算模型。输出token会成批提交，所以摊销ms/token不是用户连续两次收到token的均匀时间间隔；用户可能先等待整个周期，再收到多个token。</p>'
 s+='<h3>草拟成本不确定时，收益是否还成立？</h3><p>保持相同逐包消息，将草拟层本地服务成本乘0.5、1、2，固定α=80%。这是成本敏感性，不代表三种已实现的草拟模型，也不改变主模型验证成本。</p>'
 s+=table(['草拟d','草拟本地成本倍率','摊销ms/token（α=80%）','加速比'],[[r['depth'],r['draft_multiplier'],f'{r["amortized_ms_per_token"]:.3f}',f'{r["speedup"]:.2f}×'] for r in est if r['alpha']==.8])
 r=next(r for r in main if r['depth']==2 and r['alpha']==.8)
 s+=f'<p><b>一个便于理解的例子：</b>每轮草拟2个、α=80%时，平均每轮提交2.44个token，完整周期约{r["cycle_ms"]:.3f} ms，摊销约{r["amortized_ms_per_token"]:.3f} ms/token，为无MTP的{r["speedup"]:.2f}倍。收益来自多位置验证摊薄权重读取和同步轮次；不是让单轮计算更快。</p>'
 s+='<p>不能据此断言草拟越深越好：候选接受收益逐渐饱和，而草拟和验证成本继续增加。尚缺真实草拟权重、接受率分布、kernel效率和缓存恢复轨迹；本节用于判断潜在收益范围。真实EAGLE树状验证、动态草拟长度、训练质量及额外采样调度不在本次模型内。</p>'
 s+='<h3>复现</h3><pre>python3 simulators/sue/harness/hm_emu/mtp_run.py --config configs/experiments/hm-emu/study-mtp-public.json --output results/sue/hm-emu-mtp-new</pre><p>可调参数：草拟深度、条件接受率、草拟本地成本倍率。原始记录：<code>'+html.escape(str(path))+'</code>；离线包的mtp/目录包含输入、估计表、服务输入及逐包证据。</p></section>'
 return s
