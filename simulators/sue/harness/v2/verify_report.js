// Offline DOM-logic checks, not a browser layout/screenshot test.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const file=process.argv[2],html=fs.readFileSync(file,'utf8');
const data=html.match(/<script id="data" type="application\/json">([\s\S]*?)<\/script>/)[1];
const code=html.match(/<script>\s*([\s\S]*?)<\/script>/)[1],D=JSON.parse(data),nodes=new Map();
function get(id){if(!nodes.has(id))nodes.set(id,{value:id==='model'?'kimi-k3':id==='window'?'decode':'',textContent:'',innerHTML:'',click(){}});return nodes.get(id);}
get('data').textContent=data;let downloaded=false;
vm.runInNewContext(code,{document:{getElementById:get,createElement:()=>({click(){downloaded=true;}})},console,Blob,URL:{createObjectURL:()=>'',revokeObjectURL(){}},setTimeout:f=>f()});
assert(!/@@GUIDE_|@@GUIDE_INTRO@@/.test(html));
for(const id of ['architecture','topology','makespan','paths','cdf','heatmap','switches','timeline','cardmatrix','activity','portheat','experts'])assert(html.includes('id="guide-'+id+'"'));
assert(html.includes('5000 ns')&&html.includes('不随端点token量'));
assert.equal(D.results.length,96);assert.equal(D.results.filter(r=>r.status==='passed').length,96);
let checks=0;
for(const model of ['kimi-k3','glm-5.3'])for(const window of ['prefill','decode','train']){
 get('model').value=model;get('window').value=window;get('window').onchange();
 assert.equal((get('table').innerHTML.match(/<tr>/g)||[]).length,16);
 for(const topo of D.topologies){get('case').value=topo;get('case').onchange();
   for(const id of ['makespan','paths','cdf','heatmap','switches','timeline','cardmatrix','activity','portheat','experts']){
     const s=get(id).innerHTML;assert(s.includes('<svg'),id);assert(!/NaN|undefined|Infinity/.test(s),id+': nonfinite value');
   }
   assert(get('topologydiagram').innerHTML.includes(D.topology_diagrams[topo].title));assert.equal(get('topologyselect').value,topo);
   assert(get('metrics').innerHTML.includes('µs'));assert(get('stages').innerHTML.includes('<tr>'));checks++;
 }
}
get('export').onclick();assert(downloaded);downloaded=false;get('chartpick').value='cardmatrix';get('exportchart').onclick();assert(downloaded);
assert.equal(Object.keys(D.topology_diagrams).length,16);
for(const topo of D.topologies){get('topologyselect').value=topo;get('topologyselect').onchange();assert(get('topologydiagram').innerHTML.includes(D.topology_diagrams[topo].title));downloaded=false;get('exporttopology').onclick();assert(downloaded);}
assert(html.includes('9.41828')&&html.includes('9.41908'));
assert(!/<script[^>]+src=/.test(html),'external runtime dependency');
console.log(JSON.stringify({status:'passed',case_views:checks,checks:['12 figure measurement/interpretation guides','fixed-compute scaling boundary','96-case filters','ten SVG charts','16 topology views and SVG downloads','parity/rotate explanation and five-decimal makespan','finite numeric output','stage table','CSV and SVG export handlers','standalone embedded data'],boundary:'DOM logic only, not browser layout'},null,2));
