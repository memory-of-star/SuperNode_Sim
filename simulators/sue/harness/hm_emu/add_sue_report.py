"""Add clearly separated SUE packet results before the full message-level matrix."""
import argparse,csv,json,pathlib,html
p=argparse.ArgumentParser();p.add_argument('--report',type=pathlib.Path,required=True);p.add_argument('--runs',type=pathlib.Path,nargs='+',required=True);p.add_argument('--reference',type=pathlib.Path,required=True);p.add_argument('--protocol-tests',type=pathlib.Path,required=True);a=p.parse_args()
refs={r['id']:r for r in json.loads((a.reference/'results.json').read_text())};rows=[]
for run in a.runs:
 v=json.loads((run/'verification.json').read_text());assert not v.get('failed',0)
 rows+=json.loads((run/'results.json').read_text())
assert all(r['status']=='completed' and r['backend']=='sue' and r['steps']==256 for r in rows)
def table(head,body):return '<div class="scroll"><table><thead><tr>'+''.join('<th>'+html.escape(str(x))+'</th>' for x in head)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in row)+'</tr>' for row in body)+'</tbody></table></div>'
body=[];packet_total=0;exports=[]
for r in sorted(rows,key=lambda x:(x['batch'],x['chips'],x['mode'],x['microbatch'],x['duplex'])):
 d=pathlib.Path(r['path']);ports=list(csv.DictReader((d/'engine-sue-ports.csv').open()));wire=list(csv.DictReader((d/'engine-sue-wire.csv').open()))
 frames=sum(int(x['tx_ok']) for x in ports);packet_total+=frames;replay=sum(int(x['replay_frames']) for x in ports);block=sum(int(x['credit_exhausted']) for x in ports);assert all(int(x['discard'])==0 for x in ports)
 ref=refs[r['id']];row=[r['mode']+str(r['chips']),r['batch'],r['microbatch'],'64/64' if r['duplex']=='sum' else '128/128',f'{r["tpot_ms"]:.4f}',f'{r["throughput"]:.2f}',f'{ref["tpot_ms"]:.4f}',f'{100*(r["tpot_ms"]/ref["tpot_ms"]-1):+.2f}%',f'{frames:,}',replay,block];body.append(row)
 exports.append({**{k:r[k] for k in ('id','chips','mode','batch','microbatch','duplex','tpot_ms','throughput','wall_s')},'data_link_transmissions':frames,'replay_frames':replay,'credit_exhausted_checks':block,'tx_rx_wire_bytes':sum(int(x['tx_wire_bytes'])+int(x['rx_wire_bytes']) for x in wire)})
s=f'''<section id="sue-results"><h2>SUE 逐包链路：已接入并执行</h2><div class="cards"><div class="card"><b>{len(rows)}</b>完整256步逐包案例</div><div class="card"><b>4 / 卡</b>独立 Die 链路端点</div><div class="card"><b>AFH · LLR · CBFC</b>调用原 SUE 协议实现</div><div class="card"><b>0</b>本轮运行失败 / 丢弃</div></div><p class="note">当前物理拓扑：每张卡的四个 Die 各有一个聚合接入口，全部连接同一台 SUE 交换机；没有芯片间物理环。卡内四 Die 环保留。Ring AllReduce 只是上层集合通信算法，每个逻辑步骤的报文都经过交换机。</p><p>本节是真实 SUE 逐包事件结果；后面的522点全矩阵仍是明确标注的<strong>消息级参考</strong>，没有将其重新命名为逐包结果。已完成范围为：B=1的TP2/4/8、PP2/4/8，两种收发口径；以及B=8的PP2/4/8全部四种microbatch划分、两种收发口径。B&gt;8及其他TP并发点尚未逐包重跑。</p><h3>带宽与时延实现</h3><p>主逐包配置采用固定全双工线速：每Die每方向16 GB/s，即整卡TX 64 GB/s、RX 64 GB/s，双向合计128 GB/s；对照为每Die每方向32 GB/s，即整卡各128 GB/s。这不是消息级模型中“单向可以独占128 GB/s”的动态共享仲裁。因此PP两者的差异同时包含带宽分配口径变化与协议开销，不能全归因于SUE协议。</p><p>路径为源Die→交换机→目的Die。两根接入链路各设置500 ns单程传播；交换机转发预算1000 ns，沿用原SUE实现的“处理＋出口线时预算”语义，组成约2 μs的固定基准。真实包序列化、队列等待、控制帧竞争、信用与重传另外由事件推进。不是每根线各加2 μs，也没有再叠加旧消息级传播延时。</p><h3>逐包执行过程</h3><ol><li>每个跨卡消息分成四个Die stripe，再按最多2048 B有效载荷分片。用SUE header和shim构造报文，当前业务统一VC0。</li><li>报文进入原SueNetDevice发送队列，依次经过LLR窗口、CBFC信用与物理序列化；交换机用AFH目的路由转发。</li><li>每VC信用大小256 B、窗口512 credit（128 KiB）；主机出口队列64 KiB、交换机出口每VC128 KiB。队列满时挂起生产者，通过出口释放钩子唤醒，保持1 ns通知间隔。</li><li>接收端收齐消息全部有效字节后，通知下一归约步或下一计算阶段。LLR ACK在链路上独立返回，不把ACK返回额外串接到计算依赖。</li><li>原SUE协议核心未修改。为避免长计算空隙里生成大量空闲周期事件，本profile使用原引擎支持的逐帧ACK、逐帧CF退还，关闭周期CC/CF刷新；CtlOS仍受400 B间隔门约束。它不是原默认周期ACK配置，不能直接当成真实硬件已校准的参数。</li></ol><h3>完整decode结果</h3><p>TX/RX列为整卡每方向GB/s。消息参考列使用同一模型、同一专家路由、同一microbatch和相同服务输入。数据链路发送次数统计源→交换机与交换机→目的两段；不等于应用消息数。credit计数是失败检查次数，不是拥塞时间。以下运行累计观察到 {packet_total:,} 次数据帧链路发送。</p>'''
# A real packet-backend microbatch chart, separate from reference plots below.
pp=[r for r in rows if r['mode']=='PP' and r['batch']==8 and r['duplex']=='sum']
if pp:
 top=max(r['throughput'] for r in pp)*1.15;svg='<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1080 330">'
 for i in range(5):
  y=270-i*55;svg+=f'<path d="M85,{y}H1040" stroke="#e2e8f0"/><text x="75" y="{y+4}" text-anchor="end" font-size="12">{top*i/4:.0f}</text>'
 for i,micro in enumerate((1,2,4,8)):svg+=f'<text x="{130+i*290}" y="296" text-anchor="middle">m={micro}</text>'
 for n,color in ((2,'#2675b7'),(4,'#df873e'),(8,'#2e967e')):
  ps=sorted([r for r in pp if r['chips']==n],key=lambda r:r['microbatch']);coords=[]
  for i,r in enumerate(ps):
   x=130+i*290;y=270-r['throughput']/top*220;coords.append(f'{x},{y}');svg+=f'<circle cx="{x}" cy="{y}" r="5" fill="{color}"><title>PP{n}, m={r["microbatch"]}: {r["throughput"]:.2f} token/s</title></circle>'
  svg+=f'<polyline points="{" ".join(coords)}" fill="none" stroke="{color}" stroke-width="2.5"/><text x="{300+(n//2-1)*170}" y="322" fill="{color}">PP{n}</text>'
 svg+='<text x="85" y="20" font-size="13">SUE逐包 · B=8 · 实例输出token/s</text></svg>'
 s+='<h3>SUE微批扫描：B=8的PP吞吐</h3>'+svg+'<p class="small">全部点均执行93层、256步decode，固定每卡TX64/RX64 GB/s；没有用消息模型补点。横轴为每microbatch请求数，纵轴为完整窗口吞吐。</p>'
s+='<p><a href="sue-summary.csv" download>下载SUE逐包结果CSV</a> · <a href="sue-results.json" download>下载SUE结果JSON</a></p>'
s+=table(['配置','B','每微批请求','TX/RX GB/s','SUE TPOT/ms','SUE token/s','消息参考TPOT/ms','耗时差异','数据链路发送次数','重传帧','信用阻塞检查'],body)
t=json.loads((a.protocol_tests/'verification.json').read_text());assert t['status']=='passed'
s+='<h3>机制验证：信用与错误真的改变了时序</h3><p>同一4 MiB累计有效载荷测试，改变信用窗口或对单个入口注入每17个数据帧一次错误，均完整交付且没有LLR丢弃。</p>'+table(['场景','总时间/μs','重传帧','信用阻塞检查'],[[k,f'{v["makespan_ps"]/1e6:.3f}',v['replay_frames'],v['credit_exhausted']] for k,v in t['cases'].items()])
s+='''<p>另有四卡SUE测试直接从卡0发送到卡2：数据经交换机到达，卡1和卡3没有中继有效载荷。卡端线速、SUE控制帧与报文头由实际设备计时；片内D2D与算子仍沿用已说明的资源模型，没有升级为GPU指令级仿真。</p><h3>复现SUE结果</h3><pre>bin/sue hm-emu build --config configs/experiments/hm-emu/sue-full-decode.json
bin/sue hm-emu run --config configs/experiments/hm-emu/sue-full-decode.json --output results/sue/sue-full-new
python3 simulators/sue/harness/hm_emu/test_sue_backend.py results/sue/sue-protocol-test-new
# PP B=8 uses frozen, byte-identical service inputs from the paired message run:
python3 simulators/sue/harness/hm_emu/run_selected_sue.py --config configs/experiments/hm-emu/sue-pp-b8.json --output results/sue/sue-pp-b8-new</pre><p>study.json现在选择SUE逐包后端；study-message.json保留消息级对照。完整大batch矩阵的逐包成本远高于消息级，不能把上面的36点范围误认为整个矩阵已完成。协议统计保存在各条件的engine-sue-ports.csv和engine-sue-wire.csv。</p></section>'''
text=a.report.read_text();text=text.replace('<main>','<main>'+s,1).replace('<h2>1. 先看结果与适用范围</h2>','<h2>1. 消息级全矩阵参考：结果与适用范围</h2><p class="note">以下第1–8节的曲线和对照表来自消息级后端，不是SUE逐包结果。当前已完成的SUE性能数据仅见上方独立结果区。</p>').replace('<nav><a href="#conclusions">结论</a>','<nav><a href="#sue-results">SUE逐包结果</a><a href="#conclusions">消息级参考</a>').replace('<h1>Kimi K3：2 / 4 / 8 卡的 TP 与 PP 对照</h1>','<h1>Kimi K3：单层交换机 · SUE逐包结果与消息级对照</h1>')
# The reference section must reproduce its own backend, not the new default SUE config.
start=text.index('<section id="conclusions">');text=text[:start]+text[start:].replace('--config configs/experiments/hm-emu/study.json','--config configs/experiments/hm-emu/study-message.json')
a.report.write_text(text)
out=a.report.parent/'sue-summary.csv'
with out.open('w') as f:w=csv.DictWriter(f,fieldnames=list(exports[0]));w.writeheader();w.writerows(exports)
(a.report.parent/'sue-results.json').write_text(json.dumps(exports,indent=2)+'\n');print('SUE section cases:',len(rows))
