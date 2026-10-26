// DOM-logic smoke test; no browser/layout claim. Usage: node harness/verify_report.js <report.html>
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(process.argv[2],'utf8');
const data=html.match(/<script id="data" type="application\/json">([\s\S]*?)<\/script>/)[1];
const code=html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
const nodes=new Map();
function elem(id){if(!nodes.has(id))nodes.set(id,{value:id==='source'||id==='label'?'all':'',textContent:'',innerHTML:'',children:[],attrs:{},append(x){this.children.push(x)},setAttribute(k,v){this.attrs[k]=v}});return nodes.get(id)}
elem('data').textContent=data;
vm.runInNewContext(code,{document:{getElementById:elem,createElement:()=>({})},console});
assert(elem('timeline').innerHTML.includes('<line'));
assert(elem('bytes').innerHTML.includes('<rect'));
assert(elem('tasks').innerHTML.includes('<tr>'));
const D=JSON.parse(data);
elem('source').value=String(D.tasks[0].src);elem('source').onchange();
assert(elem('tasks').innerHTML.includes('<tr>'));
elem('label').value=D.tasks[0].label;elem('label').onchange();
assert(elem('timeline').innerHTML.includes('<line'));
elem('source').value='all';elem('label').value='all';elem('label').onchange();
elem('next').onclick();elem('prev').onclick();
assert(elem('page').textContent.startsWith('1 /'));
console.log(JSON.stringify({report:process.argv[2],tasks:D.tasks.length,checks:['embedded-json','timeline','byte-bars','task-table','source-filter','phase-filter','pagination'],status:'passed'}));
