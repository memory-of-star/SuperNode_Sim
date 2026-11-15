<script>
const $=id=>document.getElementById(id), esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const good=DATA.filter(x=>x.status==='completed'),colors=['#2675b7','#df873e','#2e967e','#aa65ad','#6d84bd','#ac7855'];
function cases(mode,n,b,duplex='sum',variant='base'){return good.filter(r=>r.mode===mode&&r.chips===n&&r.batch===b&&r.duplex===duplex&&r.variant===variant)}
function best(mode,n,b,duplex='sum',policy='best'){let a=cases(mode,n,b,duplex);if(mode==='PP'&&policy==='one')a=a.filter(r=>r.microbatch===1);if(mode==='PP'&&policy==='whole')a=a.filter(r=>r.microbatch===b);return a.sort((x,y)=>y.throughput-x.throughput)[0]}
function fmt(x){return x===undefined?'—':Math.abs(x)>=1000?x.toLocaleString('en-US',{maximumFractionDigits:0}):x.toFixed(2)}
function svgStart(w=1080,h=380){return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${w} ${h}">`}
let plotTooltip;
function hidePlotTooltip(){if(plotTooltip)plotTooltip.hidden=true;}
function bindPointTooltips(root){
 const show=(event)=>{
  const point=event.target.closest('[data-tip]');
  if(!point){hidePlotTooltip();return;}
  if(!plotTooltip){plotTooltip=document.createElement('div');plotTooltip.setAttribute('role','tooltip');plotTooltip.style.cssText='position:fixed;z-index:10000;pointer-events:none;white-space:pre-line;max-width:330px;padding:10px 14px;border-radius:7px;background:#142d46;color:#fff;font:13px/1.7 system-ui,sans-serif;box-shadow:0 4px 18px #0003';document.body.appendChild(plotTooltip);}
  plotTooltip.textContent=point.getAttribute('data-tip').split(' | ').join('\n');plotTooltip.hidden=false;
  const box=point.getBoundingClientRect(),x=event.clientX??box.right,y=event.clientY??box.top;
  plotTooltip.style.left=Math.max(8,Math.min(x+14,window.innerWidth-plotTooltip.offsetWidth-8))+'px';
  plotTooltip.style.top=Math.max(8,Math.min(y+14,window.innerHeight-plotTooltip.offsetHeight-8))+'px';
 };
 root.onpointermove=show;root.onfocusin=show;root.onpointerleave=hidePlotTooltip;root.onfocusout=hidePlotTooltip;
}
function line(id,series,{ylabel='',xlabel='',logy=false,xs=BATCHES}={}){const W=1080,H=390,L=90,T=25,R=22,D=65;let all=series.flatMap(s=>s.points).filter(p=>Number.isFinite(p.y)&&(!logy||p.y>0));if(!all.length){$(id).innerHTML='<p>该配置无可运行结果（检查容量或测试范围）。</p>';return}
const trans=v=>logy?Math.log10(v):v;let ymin=logy?Math.min(...all.map(p=>trans(p.y))):Math.min(0,...all.map(p=>p.y)),ymax=Math.max(...all.map(p=>trans(p.y)));if(ymax===ymin)ymax=ymin+1;let pad=(ymax-ymin)*.08;ymax+=pad;if(logy)ymin-=pad;
let X=x=>L+xs.indexOf(x)*(W-L-R)/Math.max(1,xs.length-1),Y=y=>H-D-(trans(y)-ymin)/(ymax-ymin)*(H-D-T),s=svgStart(W,H);
for(let i=0;i<6;i++){let v=ymin+(ymax-ymin)*i/5,y=H-D-i*(H-D-T)/5;s+=`<path d="M${L},${y}H${W-R}" stroke="#e3eaf0"/><text x="${L-12}" y="${y+4}" text-anchor="end" font-size="12" fill="#64748b">${fmt(logy?10**v:v)}</text>`}
for(const x of xs)s+=`<text x="${X(x)}" y="${H-D+25}" text-anchor="middle" font-size="12">${x}</text>`;
s+=`<text x="${L}" y="14" font-size="12" fill="#526781">${esc(ylabel)}${logy?'（对数）':''}</text>`;
if(xlabel)s+=`<text x="${L+(W-L-R)/2}" y="${H-8}" text-anchor="middle" font-size="13" fill="#526781">${esc(xlabel)}</text>`;
series.forEach((a,i)=>{const c=a.color||colors[i%colors.length];let pts=a.points.filter(p=>Number.isFinite(p.y)&&(!logy||p.y>0));s+=`<polyline points="${pts.map(p=>X(p.x)+','+Y(p.y)).join(' ')}" fill="none" stroke="${c}" stroke-width="2.5" ${a.dash?'stroke-dasharray="7 4"':''}/>`;for(const p of pts){const tip=a.name+' | '+(id==='scaling'?'XPU 卡数':id==='capacity'?'Batch Size B':'全局 Batch Size B')+'：'+p.x+' | '+ylabel+'：'+p.y.toLocaleString('en-US',{minimumFractionDigits:2,maximumFractionDigits:4})+(p.tip?' | '+p.tip:'');s+=`<g><circle cx="${X(p.x)}" cy="${Y(p.y)}" r="4.5" fill="white" stroke="${c}" stroke-width="2"/><circle cx="${X(p.x)}" cy="${Y(p.y)}" r="12" fill="transparent" style="cursor:crosshair" tabindex="0" aria-label="${esc(tip)}" data-tip="${esc(tip)}"/></g>`}});s+='</svg><div class="legend">'+series.map((a,i)=>`<span style="color:${a.color||colors[i%colors.length]}">● ${esc(a.name)}</span>`).join('')+'</div>';$(id).innerHTML=s;hidePlotTooltip();bindPointTooltips($(id))}
function refresh(){const d=$('duplex').value,p=$('policy').value;let seq=[];for(const n of [2,4,8])for(const mode of ['TP','PP'])seq.push({name:mode+n,dash:mode==='PP',rows:BATCHES.map(b=>best(mode,n,b,d,p)).filter(Boolean)});
line('latency',seq.map(s=>({...s,points:s.rows.map(r=>({x:r.batch,y:r.tpot_ms,tip:`m=${r.microbatch}; P95 token间隔=${fmt(r.p95_ms)} ms`}))})),{ylabel:'平均 TPOT / ms',xlabel:'全局 Batch Size B（并发请求数，对数刻度）',logy:true});
line('throughput',seq.map(s=>({...s,points:s.rows.map(r=>({x:r.batch,y:r.throughput,tip:`m=${r.microbatch}`}))})),{ylabel:'实例 output token/s',xlabel:'全局 Batch Size B（并发请求数，对数刻度）',logy:true});
const sb=+$('scaleB').value;line('scaling',['TP','PP'].map(mode=>({name:mode+`，B=${sb}`,points:[2,4,8].map(n=>best(mode,n,sb,d,p)).filter(Boolean).map(r=>({x:r.chips,y:r.per_chip_throughput,tip:`总吞吐 ${fmt(r.throughput)} token/s`}))})),{ylabel:'每卡 output token/s',xlabel:'XPU 芯片数（卡数，2 / 4 / 8 等距排列）',xs:[2,4,8]});
}

function inspect(){let r=DATA.find(x=>x.id===$('caseSelect').value);$('caseDetails').textContent=JSON.stringify(r,null,2)}
function download(name,text,type){let a=document.createElement('a');a.href=URL.createObjectURL(new Blob([text],{type}));a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}
function downloadAll(){let cols=['id','status','mode','chips','batch','microbatch','duplex','variant','backend','tpot_ms','p50_ms','p95_ms','request_mean_p50_ms','request_mean_p95_ms','first_decode_ms','throughput','per_chip_throughput','capacity_GB','compute_util','hbm_util','d2d_util','network_util'];download('hm-emu-results.csv','\ufeff'+cols.join(',')+'\n'+DATA.map(r=>cols.map(k=>r[k]??'').join(',')).join('\n'),'text/csv;charset=utf-8')}
function saveChart(){download('hm-emu-tpot.svg',$('latency').querySelector('svg').outerHTML,'image/svg+xml')}
for(const id of ['scaleB']){$(id).innerHTML=BATCHES.map(b=>`<option>${b}</option>`).join('');$(id).value='32'}
$('caseSelect').innerHTML=DATA.map(r=>`<option>${esc(r.id)}</option>`).join('');for(const id of ['duplex','policy','scaleB'])$(id).addEventListener('change',refresh);$('caseSelect').addEventListener('change',inspect);refresh();inspect();
</script></html>
