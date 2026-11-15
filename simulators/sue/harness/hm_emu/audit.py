"""Independent evidence audit for a completed run; no simulations are launched."""
import csv, hashlib, json, pathlib, sys
import numpy as np
from run import load, ROOT
p=pathlib.Path(sys.argv[1]);assert not (p/'INVALID.json').exists()
rows=json.loads((p/'results.json').read_text())
impl=json.loads((p/'implementation.json').read_text())
if 'source_runs' in impl:
 import subprocess
 source_rows=[];audits=[]
 for name in impl['source_runs']:
  q=pathlib.Path(name);assert hashlib.sha256((q/'results.json').read_bytes()).hexdigest()==impl['source_results_sha256'][name]
  subprocess.run([sys.executable,__file__,name],check=True)
  source_rows.extend(json.loads((q/'results.json').read_text()));audits.append(json.loads((q/'audit.json').read_text()))
 assert sorted(rows,key=lambda x:x['id'])==sorted(source_rows,key=lambda x:x['id'])
 assert len({r['id'] for r in rows})==len(rows)
 result={'status':'passed','source_runs':impl['source_runs'],'source_audits':audits,'disjoint_measured_cases_unchanged':True}
 (p/'audit.json').write_text(json.dumps(result,indent=2)+'\n');print('Composed-run audit passed');sys.exit(0)
config=next(pathlib.Path(k) for k in impl['input_files'] if pathlib.Path(k).suffix=='.json' and json.loads(pathlib.Path(k).read_text()).get('schema')=='hm-emu-1')
c,m=load(config)
assert c==json.loads((p/'experiment.json').read_text()),'experiment changed since execution'
for name,digest in impl['input_files'].items():assert hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()==digest
for name,digest in impl.get('libraries',{}).items():assert hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()==digest
if 'engine_sha256' in impl:assert hashlib.sha256((p/impl.get('engine_relative_path','bin/hm-emu-sue-engine')).read_bytes()).hexdigest()==impl['engine_sha256']
reconstructed=sum(m.weight(l,1) for l in range(m.L))+sum(v['bytes'] for v in m.tail.values())+m.other
assert reconstructed==m.inv['checkpoint_bytes']
counts={};token_rows=0
for r in rows:
 counts[r['status']]=counts.get(r['status'],0)+1
 if r['status']=='capacity_rejected':assert max(x['total_bytes'] for x in r['capacity'])>c['hardware']['memory_bytes'];continue
 assert r['status']=='completed'
 assert r['backend']=='sue'
 d=pathlib.Path(r['path'])
 with (d/'engine-sue-ports.csv').open() as f:ports=list(csv.DictReader(f))
 assert len(ports)==8*r['chips'] and sum(int(x['discard']) for x in ports)==0
 assert sum(int(x['tx_ok']) for x in ports)==r['sue_counters']['tx_ok']>0
 timing=json.loads((d/'engine-sue-profile.json').read_text());assert timing==r['sue_timing']
 assert timing['fixed_path_ps']==(1000000 if r['variant']=='latency_half' else 2000000)
 assert timing['switch_forwarding_budget_ps']==300000 and timing['link_propagation_ps']==20000
 assert timing['access_channel_ps']==timing['link_propagation_ps']+timing['xpu_extra_access_ps']
 assert 2*timing['access_channel_ps']+timing['switch_forwarding_budget_ps']==timing['fixed_path_ps']
 rate=128000000000*(2 if r['duplex']=='independent' else 1)*(2 if r['variant']=='bandwidth_x2' else 1)
 assert timing['per_die_direction_bps']==rate
 with (d/'engine-sue-wire.csv').open() as f:wire=list(csv.DictReader(f))
 actual=(sum(int(x['tx_wire_bytes'])+int(x['rx_wire_bytes']) for x in wire)/(rate*r['chips']*r['makespan_s']))
 assert abs(actual-r['network_util'])<1e-6  # billions of double-precision time additions

 d=pathlib.Path(r['path']);assert hashlib.sha256((d/'service.bin').read_bytes()).hexdigest()==r['input_sha256']
 a=np.fromfile(d/'service.bin',dtype='<f8');assert list(r['service_shape'])==[r['steps'],r['batch']//r['microbatch'],279 if r['mode']=='TP' else 1 if r['mode']=='TP_EP' else r['chips']]
 assert a.size==np.prod(r['service_shape']) and np.isfinite(a).all() and (a>=0).all()
 prev={};sums={};intervals=[]
 with (d/'engine-tokens.csv').open() as f:
  for x in csv.DictReader(f):
   g,st,t,dt=(int(x[k]) for k in ('group','step','completed_ps','interval_ps'));ps,pt=prev.get(g,(-1,0));assert st==ps+1 and t>pt and dt==t-pt;prev[g]=(st,t)
   if st:intervals.append(dt)
   token_rows+=1
 assert len(prev)==r['batch']//r['microbatch'] and all(st==r['steps']-1 for st,t in prev.values())
 assert abs(np.mean(intervals)/1e9-r['tpot_ms'])<1e-8
 assert abs(max(t for st,t in prev.values())/1e12-r['makespan_s'])<1e-8
 assert all(x['total_bytes']<=c['hardware']['memory_bytes'] for x in r['capacity'])
 assert abs(r['throughput']-r['batch']*r['steps']/r['makespan_s'])<1e-7
# The EP DAG adds dispatch/combine and ownership, not a post-hoc timing factor.
for r in rows:
 if r.get('mode')!='TP_EP' or r['status']!='completed':continue
 d=pathlib.Path(r['path']);assert hashlib.sha256((d/'dag.tsv').read_bytes()).hexdigest()==r['dag_sha256']
 task_rows=list(csv.DictReader((d/'engine-ep-tasks.csv').open()));assert len(task_rows)==sum(1 for _ in (d/'dag.tsv').open())
 byid={int(t['id']):t for t in task_rows};expected_bytes=0
 for line in (d/'dag.tsv').read_text().splitlines():
  ident,kind,src,dst,st,ps,by,label,deps=line.split();t=byid[int(ident)];assert t['kind']==kind
  start,end=int(t['start_ps']),int(t['end_ps']);assert end>=start
  if deps!='-':assert all(start>=int(byid[int(dep)]['end_ps']) for dep in deps.split(','))
  if kind=='C':assert end-start==int(ps)
  if kind=='N':expected_bytes+=int(by)
 assert sum(r['tx_bytes'])==sum(r['rx_bytes'])==expected_bytes
 alloc=json.loads((d/'expert-assignment-counts.json').read_text());assert len(alloc)==r['steps']*92
 assert all(v['group0_assignments']+v['group1_assignments']==16*r['batch'] for v in alloc)
 if r['batch']==1:assert all(v['remote_tokens_1']==0 for v in alloc)
# Weight tile bounds are checked independently of the elapsed-time results.
for f in (p/'routes').glob('expert-tiles-m*.npy'):
 msize=int(f.stem.split('-m')[-1]);a=np.load(f,mmap_mode='r');assert (a[:,0,:]==0).all()
 assert np.all(a[:,1:,:]>=16) and np.all(a[:,1:,:]<=16*msize)
 if msize==1:assert np.all(a[:,1:,:]==16)
impl=json.loads((p/'implementation.json').read_text())
for name in ('model.py','engine.cc','sue_backend.h','run.py'):
 f=ROOT/'simulators/sue/harness/hm_emu'/name;expected=impl['files'][str(f.relative_to(ROOT))]
 if hashlib.sha256(f.read_bytes()).hexdigest()!=expected:f=p/'source_snapshot'/name
 assert hashlib.sha256(f.read_bytes()).hexdigest()==expected
result={'status':'passed','condition_counts':counts,'token_completion_rows_checked':token_rows,'checkpoint_reconstructed_bytes':int(reconstructed),'checkpoint_matches_official_index':True,'service_sha_shapes_nonnegative_finite':True,'per_request_autoregressive_completion_order':True,'tpot_and_throughput_recomputed':True,'capacity_admission_checked':True,'routing_tile_bounds':True,'executed_physics_sources_or_frozen_snapshot_verified':True,'sue_protocol_counters_and_latency_profiles':True,'network_wire_utilization_recomputed':True}
(p/'audit.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
