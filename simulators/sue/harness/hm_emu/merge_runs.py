"""Compose disjoint, fully verified SUE runs without changing measured cases."""
import argparse,json,pathlib,shutil,csv,time
import run

def main():
 p=argparse.ArgumentParser();p.add_argument('--runs',nargs='+',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--wait',action='store_true');a=p.parse_args();sources=[x.resolve() for x in a.runs]
 while any(not (x/'verification.json').exists() for x in sources):
  if not a.wait:raise SystemExit('source run incomplete')
  time.sleep(10)
 configs=[json.loads((x/'experiment.json').read_text()) for x in sources];configs0=configs[0]
 for x,c in zip(sources,configs):
  assert not (x/'INVALID.json').exists() and not (x/'CANCELLED.json').exists()
  v=json.loads((x/'verification.json').read_text());assert v['failed']==0 and v['backend']=='sue'
  for key in ('hardware','modeling','sensitivity'):assert c[key]==configs0[key],key
  for key in ('steps','seed','batches','initial_context'):assert c['workload'][key]==configs0['workload'][key]
  assert json.loads((x/'routes/routing.json').read_text())['route_sha256']==json.loads((sources[0]/'routes/routing.json').read_text())['route_sha256']
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);(out/'conditions').mkdir();(out/'source_snapshot').mkdir();rows=[];seen=set();val={'backend':'sue','partitions':{},'capacity':[]}
 for source in sources:
  for r in json.loads((source/'results.json').read_text()):
   assert r['id'] not in seen;seen.add(r['id']);rows.append(r);(out/'conditions'/r['id']).symlink_to(pathlib.Path(r['path']))
  v=json.loads((source/'validation.json').read_text());val['partitions'].update(v['partitions']);val['capacity'].extend(v['capacity'])
 c=json.loads(json.dumps(configs0));c['name']='kimi-k3-sue-2-4-8-and-16chip-tp8-ep2';c['workload']['chips']=[2,4,8,16];c['parallel_16chip']={'tp_chips':8,'ep_groups':2,'expert_tp_dies':32,'attention_dp_groups':2,'pp_alternative':16}
 run.dump(out/'experiment.json',c);run.dump(out/'validation.json',val);run.dump(out/'results.json',sorted(rows,key=lambda x:x['id']))
 shutil.copytree(sources[0]/'routes',out/'routes')
 for f in run.HERE.glob('*'):
  if f.is_file():shutil.copy2(f,out/'source_snapshot'/f.name)
 run.dump(out/'implementation.json',{'source_runs':[str(x) for x in sources],'source_results_sha256':{str(x):run.sha(x/'results.json') for x in sources},'files':{str(f.relative_to(run.ROOT)):run.sha(f) for f in run.HERE.glob('*') if f.is_file()},'composition':'disjoint conditions; no timings synthesized or recomputed'})
 counts={k:sum(r['status']==k for r in rows) for k in ('completed','capacity_rejected','failed')};run.dump(out/'verification.json',{'planned':len(rows),**counts,'backend':'sue','all_requested_points_accounted':True,'source_runs':[str(x) for x in sources]})
 with (out/'summary.csv').open('w') as f:
  w=csv.DictWriter(f,['id','mode','chips','batch','microbatch','duplex','variant','status','tpot_ms','throughput'],extrasaction='ignore');w.writeheader();w.writerows(rows)
 print(counts,flush=True)
if __name__=='__main__':main()
