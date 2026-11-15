"""Execute every frozen workload through original SUE packets, with new network settings.
Only compute service inputs are reused; no old elapsed times or performance points.
Each actual condition runs independently, including otherwise equivalent controls.
"""
import argparse, concurrent.futures, csv, json, pathlib, shutil, subprocess, sys, time
import run

def main():
 p=argparse.ArgumentParser();p.add_argument('--config',type=pathlib.Path,required=True);p.add_argument('--source-run',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
 config=a.config.resolve();source=a.source_run.resolve();out=a.output.resolve()
 c,model=run.load(config);assert c['modeling']['backend']=='sue'
 oldc=json.loads((source/'experiment.json').read_text());oldrows=json.loads((source/'results.json').read_text())
 assert not (source/'INVALID.json').exists()
 # Frozen operator values must match all non-network inputs exactly.
 for key in ('workload','sensitivity'):assert c[key]==oldc[key],key
 for key in c['hardware']:
  if key not in ('scaleup_bytes_per_s','scaleup_hop_ns'):assert c['hardware'][key]==oldc['hardware'][key],key
 for key in c['modeling']:
  if key not in ('backend','network','sue_profile','scaleup_duplex_allocation'):assert c['modeling'][key]==oldc['modeling'][key],key
 assert c['model_config']==oldc['model_config'] and c['weight_inventory']==oldc['weight_inventory']
 assert all(r['status'] in ('completed','capacity_rejected') for r in oldrows)
 out.mkdir(parents=True,exist_ok=False);(out/'conditions').mkdir();(out/'source_snapshot').mkdir();(out/'bin').mkdir()
 run.dump(out/'experiment.json',c);run.dump(out/'validation.json',run.validation(c,model))
 for f in run.HERE.glob('*'):
  if f.is_file():shutil.copy2(f,out/'source_snapshot'/f.name)
 shutil.copytree(source/'routes',out/'routes')
 run.build('sue');shutil.copy2(run.BIN,out/'bin/hm-emu-sue-engine');run.BIN=out/'bin/hm-emu-sue-engine'
 impl={'files':{str(f.relative_to(run.ROOT)):run.sha(f) for f in run.HERE.glob('*') if f.is_file()},'input_files':{str(f):run.sha(f) for f in (config,config.parent/c['model_config'],config.parent/c['weight_inventory'])},'source_run':str(source),'source_results_sha256':run.sha(source/'results.json'),'engine_sha256':run.sha(run.BIN),'libraries':{str(f):run.sha(f) for f in (run.SIM/'build/lib').glob('libns3.36.1-*-default.so') if any(k in f.name for k in ('sue-','core-','network-','point-to-point-'))},'source_service':'byte-identical, verified; no prior performance results reused','command':sys.argv}
 run.dump(out/'implementation.json',impl)
 rows=[];jobs=[]
 for old in oldrows:
  d=out/'conditions'/old['id'];d.mkdir();origin=pathlib.Path(old['path'])
  case={k:old[k] for k in ('id','mode','chips','batch','microbatch','duplex','variant','steps','capacity','partition')}
  case.update(path=str(d),backend='sue',topology='single_switch',sue_profile=c['modeling']['sue_profile'],timeout=c['execution']['case_timeout_s'])
  if old['status']=='capacity_rejected':
   r={**case,'status':'capacity_rejected'};run.dump(d/'result.json',r);rows.append(r);continue
  for key in ('resource_totals','input_sha256','service_shape','stage_mean_service_ms'):case[key]=old[key]
  shutil.copy2(origin/'service.bin',d/'service.bin');assert run.sha(d/'service.bin')==case['input_sha256']
  lines=(origin/'input.txt').read_text().splitlines();header=lines[0].split();header[7]=str(c['hardware']['scaleup_bytes_per_s']*(2 if case['variant']=='bandwidth_x2' else 1));header[8]=str(round(c['hardware']['scaleup_hop_ns']*(.5 if case['variant']=='latency_half' else 1)*1000));header[10]=str(d/'service.bin');lines[0]=' '.join(header)
  (d/'input.txt').write_text('\n'.join(lines)+'\n');run.dump(d/'case.json',case)
  # Longest packet jobs first prevents a single huge TP condition starting last.
  payload=list(map(float,lines[1].split()));n=case['chips'];m=case['microbatch'];B=case['batch'];S=case['steps'];packet=c['modeling']['sue_profile']['payload']
  import math
  estimate=(sum(4*math.ceil(x/n/4/packet) for x in payload if x)*n*2*(n-1)*S if case['mode']=='TP' else sum(4*math.ceil(x/4/packet) for x in payload)*B//m*S)
  jobs.append((estimate,case))
 jobs.sort(key=lambda x:-x[0]);start=time.time()
 def checkpoint():
  counts={s:sum(r['status']==s for r in rows) for s in ('completed','capacity_rejected','failed')}
  run.dump(out/'progress.json',{'planned':len(oldrows),**counts,'pending':len(oldrows)-len(rows),'elapsed_s':time.time()-start})
 checkpoint();print(f'Prepared {len(jobs)} packet conditions; {len(rows)} capacity rejections; {c["execution"]["workers"]} workers',flush=True)
 with concurrent.futures.ThreadPoolExecutor(max_workers=c['execution']['workers']) as pool:
  futures=[pool.submit(run.execute,case) for estimate,case in jobs]
  for f in concurrent.futures.as_completed(futures):
   r=f.result();rows.append(r)
   with (out/'ledger.jsonl').open('a') as ledger:ledger.write(json.dumps(r)+'\n')
   checkpoint();print(f'{len(rows)}/{len(oldrows)} {r["id"]}: {r["status"]} ({r["wall_s"]:.1f}s)',flush=True)
 rows.sort(key=lambda x:x['id']);run.dump(out/'results.json',rows)
 fields=['id','status','mode','chips','batch','microbatch','duplex','variant','backend','tpot_ms','throughput','network_util','wall_s']
 with (out/'summary.csv').open('w') as f:
  w=csv.DictWriter(f,fields,extrasaction='ignore');w.writeheader();w.writerows(rows)
 progress=json.loads((out/'progress.json').read_text());run.dump(out/'verification.json',{**progress,'backend':'sue','all_requested_points_accounted':True,'frozen_service_sha_matched':True,'token_conservation':progress['failed']==0,'network_budget_and_bytes':progress['failed']==0,'resolution':'1ps','notification_interval':'1ns','sources':impl['input_files']})
 if progress['failed']:raise SystemExit(1)
 print('All packet conditions completed.',flush=True)
if __name__=='__main__':main()
