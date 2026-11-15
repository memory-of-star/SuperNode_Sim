"""Replay frozen operator service inputs through SUE; preserve trace pairing."""
import argparse,concurrent.futures,json,pathlib,shutil,sys
import run
p=argparse.ArgumentParser();p.add_argument('--config',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args();c=json.loads(a.config.read_text());source=(a.config.parent/c['source_run']).resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);(out/'conditions').mkdir();run.BIN=run.ROOT/'results/build/hm-emu-sue-engine'
run.dump(out/'experiment.json',c);source_rows=json.loads((source/'results.json').read_text());jobs=[]
for old in source_rows:
 if old['status']!='completed' or old['mode'] not in c['modes'] or old['chips'] not in c['chips'] or old['batch'] not in c['batches'] or old['variant']!='base':continue
 path=out/'conditions'/old['id'];path.mkdir();origin=pathlib.Path(old['path']);case=json.loads((origin/'case.json').read_text());case.update(path=str(path),backend='sue',sue_profile=c['sue_profile'],timeout=c['timeout_s'])
 shutil.copy2(origin/'service.bin',path/'service.bin');text=(origin/'input.txt').read_text().replace(str(origin/'service.bin'),str(path/'service.bin'));(path/'input.txt').write_text(text)
 run.dump(path/'case.json',case);jobs.append(case)
run.dump(out/'implementation.json',{'files':{str(f.relative_to(run.ROOT)):run.sha(f) for f in run.HERE.glob('*') if f.is_file()},'source_run':str(source),'service_inputs':'byte-identical copied from source conditions; SHA checked','config_sha256':run.sha(a.config)})
with concurrent.futures.ThreadPoolExecutor(max_workers=c['workers']) as pool:rows=list(pool.map(run.execute,jobs))
run.dump(out/'results.json',rows);counts={s:sum(r['status']==s for r in rows) for s in ('completed','failed')};run.dump(out/'verification.json',{'planned':len(rows),**counts,'backend':'sue_packets','source_service_sha_matched':all(run.sha(pathlib.Path(r['path'])/'service.bin')==r['input_sha256'] for r in rows)})
print(counts);sys.exit(bool(counts['failed']))
