"""Topology schematics derived from executable port maps, not hand-written wiring."""
import html
import zipfile
from collections import defaultdict
from itertools import combinations
from topologies import BASE_TOPOLOGY_NAMES, TOPOLOGIES, ports, validate, proxy, ring_distance

COLORS = ['#2675bd','#168b90','#3c9261','#9a8624','#da8032','#c45c80','#8a64b8','#586cc0']
TITLES = {'xpu8':'8 XPU · 单台多VRF','xpu16':'16 XPU · 两台交换机','xpu32':'32 XPU · 四台交换机',
          'xpu64':'64 XPU · 一轨一台','xpu128-parity':'128 XPU · 奇偶分面 parity',
          'xpu128-rotate':'128 XPU · 分法轮转 rotate','xpu256':'256 XPU · 按Die拆轨',
          'mixed6-8':'6 XPU + 8 MPU · 单台满配','mixed8-8':'8 XPU + 8 MPU · 两台多VRF',
          'mixed32-32':'32 XPU + 32 MPU · 一轨一台','mixed64-64':'64 XPU + 64 MPU · MPU好口成组',
          'mixed64-64-pool':'64 XPU + 64 MPU · 奇偶双池','xpu18':'18 XPU · 576口单台',
          'mixed16-8':'16 XPU + 8 MPU · 576口单台','xpu144':'144 XPU · 576口完整轨',
          'xpu576':'576 XPU · 576口按Die拆轨'}
NOTES = {
 'small':'同口号全体Die同域；每个源Die的8个口分属8个转发域。',
 'single':'同口号共用VRF，8条轨逻辑隔离；所有Die对均有8条直接轨。',
 'two':'偶数口接SW0、奇数口接SW1；每台4个VRF，8条轨全部直达。',
 'rail':'交换机号等于口号；每条轨容纳全体SA，一台交换机一个VRF。',
 'parity':'所有轨均为 {D0,D2} | {D1,D3}；同奇偶直接，跨奇偶经目的侧1跳UCIe。',
 'rotate':'轨0/3/6用A，轨1/4/7用B，轨2/5用C；跨Die每对只有2–3条直接轨。',
 'pure':'每台交换机承载一个(Die,口号)；同Die直达，跨Die经目的侧1–2跳UCIe。',
 'mixed':'XPU奇偶分面；MPU口0–3接偶面、4–7接奇面。偶Die选组0，奇Die选组1。',
 'pool':'偶MPU的8口全部接偶面，奇MPU接奇面；跨池无直接路径，R5仅部分满足。'}


def describe(t):
    rows=ports(t); validation=validate(t,rows)
    idx={(r['chip'],r['die'],r['port']):r for r in rows}
    reps=[dict(label='XPU0',chip=0,multiplicity=t.xpu,kind='xpu')]
    if t.mpu:
        if t.family=='pool':
            reps += [dict(label=f'MPU{t.xpu+j} · '+('偶池' if j==0 else '奇池'),chip=t.xpu+j,multiplicity=t.mpu//2,kind='mpu') for j in (0,1)]
        else: reps += [dict(label=f'MPU{t.xpu}',chip=t.xpu,multiplicity=t.mpu,kind='mpu')]
    switch_rows=[]
    for s in range(t.switches):
        rr=[r for r in rows if r['switch']==s]
        switch_rows.append(dict(switch=s,used=len(rr),radix=t.radix,vrfs=sorted({r['vrf'] for r in rr}),
            dies=sorted({r['die'] for r in rr if r['chip']<t.xpu}),mpu_ports=sum(r['chip']>=t.xpu for r in rr)))
    pairs=[]
    for a,b in combinations(range(4),2):
        direct=[];hops=[]
        for i in range(8):
            sr,dr=idx[0,a,i],idx[1,b,i]
            if (sr['switch'],sr['vrf'])==(dr['switch'],dr['vrf']):direct.append(i)
            pd=proxy(t,idx,0,a,1,b,i);hops.append(None if pd is None else ring_distance(pd,b))
        pairs.append(dict(src_die=a,dst_die=b,direct_ports=direct,ucie_hops=hops,
            guaranteed_groups=[g for g in range(2) if all(i in direct for i in range(4*g,4*g+4))]))
    links=[dict(r) for r in rows if any(r['chip']==rep['chip'] for rep in reps)]
    return dict(name=t.name,title=TITLES[t.name],xpu=t.xpu,mpu=t.mpu,switch_count=t.switches,
                radix=t.radix,family=t.family,note=NOTES[t.family],links=len(rows),domains=validation['domains'],
                representatives=reps,representative_ports=links,switches=switch_rows,die_pairs=pairs)


def draw(meta):
    # Rail slices deliberately repeat shared physical switches, distinguished by VRF.
    m=meta; name=m['name']; esc=html.escape; out=[]
    invrows=(m['switch_count']+7)//8
    bottom=714+invrows*64
    height=bottom+300
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1240 {height}" role="img" aria-labelledby="{name}-title {name}-desc">')
    out.append(f'<title id="{name}-title">{esc(m["title"])}接线示意图</title><desc id="{name}-desc">按8条轨展开一张代表XPU及代表MPU；箭头取自实际端口映射。同编号SW是同一物理交换机，VRF区分转发域。下方列出全部物理交换机和跨Die直接轨。</desc>')
    out.append('<style>.toposvg text{font-family:system-ui,Microsoft YaHei,sans-serif;fill:#263b52}.toposvg .ttl{font-size:25px;font-weight:700}.toposvg .head{font-size:16px;font-weight:700}.toposvg .txt{font-size:14px}.toposvg .tiny{font-size:12px}.toposvg .muted{fill:#607286}</style>')
    out.append('<g class="toposvg">')
    def box(x,y,w,h,color,stroke='#dce5ef',rx=9,extra=''):
        out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{color}" stroke="{stroke}" {extra}/>')
    def text(x,y,s,cl='txt',extra=''):
        out.append(f'<text x="{x}" y="{y}" class="{cl}" {extra}>{esc(str(s))}</text>')
    def arrow(x,y,xx,yy,color,tip=''):
        out.append(f'<g><title>{esc(tip)}</title><path d="M{x} {y} L{xx} {yy}" stroke="{color}" stroke-width="1.8" fill="none"/><circle cx="{xx}" cy="{yy}" r="2.8" fill="{color}"/></g>')
    box(0,0,1240,height,'#f3f6fb',rx=16)
    text(28,39,m['title'],'ttl')
    text(28,68,f"{m['xpu']} XPU × 32口"+(f" + {m['mpu']} MPU × 8口" if m['mpu'] else '')+f"   ｜   {m['switch_count']}台 × {m['radix']}口   ｜   {m['links']:,}条200G链路   ｜   {m['domains']}个转发域")
    text(28,95,m['note'],'txt')
    text(28,121,'接线展开：每列为同一张代表XPU的第i号口；D0–D3是其4个Die。点线表示直连交换机，无ISL。','tiny')
    for g in range(2):
        box(25+g*598,139,589,36,'#e5ecf6',rx=7)
        text(40+g*598,163,f'端口组 {g}：口 {4*g}–{4*g+3} · 软件选组，硬件哈希选组内口','head')
    idx={(r['chip'],r['die'],r['port']):r for r in m['representative_ports']}
    for i in range(8):
        x=26+i*149;col=COLORS[i]
        box(x,187,140,395,'white',col)
        text(x+12,210,f'轨 {i} / 口 {i}','head')
        rr=[idx[0,d,i] for d in range(4)]
        domains=sorted({(r['switch'],r['vrf']) for r in rr})
        centers={dom:246+(j+.5)*260/len(domains) for j,dom in enumerate(domains)}
        for d,r in enumerate(rr):
            y=259+d*65;dom=(r['switch'],r['vrf']);cy=centers[dom]
            arrow(x+32,y,x+52,cy,col,f"XPU0.Die{d} P{i} → SW{r['switch']} VRF{r['vrf']} 物理口{r['physical']}")
            out.append(f'<circle cx="{x+19}" cy="{y}" r="14" fill="#edf3fa" stroke="{col}"/>')
            text(x+19,y+4,f'D{d}','tiny','text-anchor="middle"')
        for (s,v),cy in centers.items():
            ds=[r['die'] for r in rr if (r['switch'],r['vrf'])==(s,v)]
            box(x+53,cy-25,77,50,'#e9f3f4',col,6)
            text(x+91,cy-6,f'SW{s}','head','text-anchor="middle"')
            text(x+91,cy+13,f'VRF{v} · '+''.join(map(str,ds)),'tiny','text-anchor="middle"')
        for j,rep in enumerate(m['representatives'][1:]):
            r=idx[rep['chip'],0,i];y=529+j*29
            s,v=r['switch'],r['vrf'];cy=centers[s,v]
            arrow(x+135+j*3,y,x+135+j*3,cy,'#bf8928',f"{rep['label']} P{i} → SW{s} VRF{v} 物理口{r['physical']}")
            arrow(x+135+j*3,cy,x+130,cy,'#bf8928')
            box(x+5,y-13,127,25,'#fff3dc','#dfb665',5)
            text(x+9,y+4,('偶MPU' if j==0 else '奇MPU')+f' → SW{s}' if m['family']=='pool' else f'MPU P{i} → SW{s}','tiny')
    text(28,606,'同编号SW在多列出现时，表示同一台物理交换机的不同VRF；没有增加交换机。VRF采用运行配置编号1起。','tiny')
    reps='；'.join(f"{r['label']}代表{r['multiplicity']}张同类卡" for r in m['representatives'])
    text(28,630,reps+'。全部卡的接线目标按同一规则复制，物理端口各不相同。','tiny')
    text(28,659,'颜色仅区分轨；同色不代表同域。SW框中02等表示承接的Die组合；直达必须同时满足同SW、同VRF。','tiny')
    text(28,689,'全部物理交换机 · 已用口 / 容量 · VRF数','head')
    for s in m['switches']:
        k=s['switch'];x=26+(k%8)*149;y=704+(k//8)*64
        box(x,y,140,54,'#fff')
        text(x+10,y+21,f"SW{k}   {s['used']}/{s['radix']}",'head')
        text(x+10,y+42,f"{len(s['vrfs'])} VRF · MPU口 {s['mpu_ports']}",'tiny')
    text(28,bottom+20,'跨卡不同Die对：8条轨中哪些能直接一跳？','head')
    for i in range(8):text(218+i*37,bottom+46,str(i),'tiny','text-anchor="middle"')
    for j,p in enumerate(m['die_pairs']):
        y=bottom+58+j*28
        text(30,y+16,f"D{p['src_die']} ↔ D{p['dst_die']}")
        for i in range(8):box(205+i*37,y,26,20,COLORS[i] if i in p['direct_ports'] else '#e2e7ef',rx=4)
        groups=','.join(map(str,p['guaranteed_groups'])) or '无'
        text(520,y+16,f"{len(p['direct_ports'])}/8直接；保证全组直接：{groups}",'tiny')
    text(28,bottom+257,'有色=直接一跳；灰色=需目的侧UCIe转交。同Die对始终8/8直接；本表不表示MPU跨池可达。','tiny')
    box(816,bottom+15,395,212,'#fff8ec','#dfc48f')
    text(834,bottom+41,'每张XPU内部 · 双向UCIe环','head')
    coords={0:(862,bottom+79),1:(960,bottom+79),2:(960,bottom+160),3:(862,bottom+160)}
    for a,b in [(0,1),(1,2),(2,3),(3,0)]:
        x,y=coords[a];xx,yy=coords[b];out.append(f'<path d="M{x} {y} L{xx} {yy}" stroke="#c19345" stroke-width="3"/>')
    for d,(x,y) in coords.items():
        out.append(f'<circle cx="{x}" cy="{y}" r="19" fill="#fff" stroke="#c19345"/>');text(x,y+5,'D'+str(d),'txt','text-anchor="middle"')
    text(1000,bottom+83,'相邻Die：1跳','tiny');text(1000,bottom+109,'对角Die：2跳','tiny')
    text(1000,bottom+141,'先过交换机','tiny');text(1000,bottom+166,'再在目的卡内转交','tiny')
    text(834,bottom+206,'MPU没有此环；环为链路/缓冲抽象。','tiny')
    out.append('</g></svg>')
    return '\n'.join(out)


def generate(dest):
    folder=dest/'topologies';folder.mkdir(exist_ok=True)
    diagrams={}
    for name in BASE_TOPOLOGY_NAMES:
        t=TOPOLOGIES[name]
        meta=describe(t);meta['svg']=draw(meta)
        (folder/(name+'.svg')).write_text(meta['svg'])
        diagrams[name]=meta
    with zipfile.ZipFile(dest/'topology-diagrams.zip','w',zipfile.ZIP_DEFLATED) as z:
        for name in diagrams:z.write(folder/(name+'.svg'),name+'.svg')
    return diagrams


def comparison(results):
    lookup={(r['topology'],r['model'],r['window']):r for r in results}
    heading='''## parity 与 rotate：接线不同，为什么有些数字一样？

两者都是128张XPU、16台256口交换机、4096条200G链路，没有交换机间级联。它们改变的是每条轨上4个Die如何分到两台交换机。

| 对比 | xpu128-parity：奇偶分面 | xpu128-rotate：分法轮转 |
|---|---|---|
| 每条轨的分组 | 始终为 {0,2} / {1,3} | 轨0/3/6：A={0,1}/{2,3}；轨1/4/7：B={0,2}/{1,3}；轨2/5：C={0,3}/{1,2} |
| 同Die跨卡 | 8条轨全部直接一跳 | 8条轨全部直接一跳 |
| 不同Die跨卡 | 0↔2、1↔3有8条直接轨；其余4对没有直接轨 | 所有6对均有直接轨，但每对仅2或3条 |
| 软件只能选4口组时 | 同奇偶对任一组都保证直接一跳；跨奇偶固定走1次UCIe | 没有一个4口组能为不同Die对保证全组直接；T1/T2随哈希选口变化 |
| 目的侧转交 | 跨奇偶由目的卡代理Die接收，再走1次UCIe | 选中不直达的轨时，由目的卡同侧代理Die接收，再走1次UCIe |

**示例：源D0→目的D1。** parity在8条轨上都分面，全部经目的侧1次UCIe；rotate在A分法的轨0、3、6上直接，其他5条轨需1次UCIe。反过来，D0→D2在parity上8条轨全直接，在rotate上仅轨1、4、7直接。因此rotate并非在所有通信对上更快。

**相同的是实验输入。** 对每个模型与窗口，已逐文件核对：两种拓扑的tasks.json和token_routes.csv完全一致，ports.csv和path_plan.csv不同。相同卡数、相同任务、相同专家放置与路由抽样，使任务数、请求字节数、源Die/源卡→目的Die/目的卡通信量矩阵，以及专家分配图相同；这些图是输入需求，不是实际网络路径。任务并发、端口占用、UCIe路径比例和时延才可能体现接线差异。

**显示精度也会隐藏差异。** Kimi prefill的9.41828与9.41908µs此前都四舍五入为9.42µs，相差0.00080µs，即0.8ns；本报告完成窗口现显示5位小数，避免把显示相同误解为测量相同。其他少数分位数仍可能确实相同，例如本次Kimi train的P99均为2.40000µs，并不说明全部任务时序相同。显示精度不等于硬件预测精度。

| 模型 / 窗口 | parity完成 µs | rotate完成 µs | rotate−parity µs | parity经UCIe请求% | rotate经UCIe请求% |
|---|---:|---:|---:|---:|---:|
'''
    for model in ['kimi-k3','glm-5.3']:
        for window in ['prefill','decode','train']:
            a=lookup['xpu128-parity',model,window];b=lookup['xpu128-rotate',model,window]
            def share(r):return 100*sum(v for k,v in r['task_path_bytes'].items() if k!='0')/r['logical_bytes']
            heading+=f"| {model} / {window} | {a['makespan_us']:.5f} | {b['makespan_us']:.5f} | {b['makespan_us']-a['makespan_us']:+.5f} | {share(a):.3f} | {share(b):.3f} |\n"
    heading+='''
这些是同一种子、固定5µs计算等待的代表层窗口；两种接法的差别在当前输入下可能较小。结构上，每条轨的两种分法都覆盖4个同Die有序对和4个跨Die有序对，所以近似均匀的Die流量可具有相近的总体直达比例，但具体哪些通信对直达不同；这不能解释为两种拓扑等价，也不能据此保证一种始终优于另一种。
'''
    return heading
