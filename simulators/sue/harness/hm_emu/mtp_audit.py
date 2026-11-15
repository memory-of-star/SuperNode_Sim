"""Recompute cycle and expectation metrics independently from packet-run evidence."""
import csv,json,hashlib,pathlib,sys,numpy as np
p=pathlib.Path(sys.argv[1]);impl=json.loads((p/'implementation.json').read_text());sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
for f,digest in impl['inputs'].items():assert sha(pathlib.Path(f))==digest
for f,digest in impl['sources'].items():assert sha(p/'source_snapshot'/f)==digest
assert sha(p/'bin/engine')==impl['binary_sha256']
rows=json.loads((p/'results.json').read_text());lookup={r['id']:r for r in rows}
for r in rows:
 d=pathlib.Path(r['path']);assert sha(d/'service.bin')==r['service_sha256']
 token=list(csv.DictReader((d/'engine-tokens.csv').open()));assert len(token)==2
 interval=int(token[1]['completed_ps'])-int(token[0]['completed_ps']);assert abs(interval/1e9-r['cycle_ms'])<1e-9
 assert r['request_batch']==1 and r['verification_positions']==r['depth']+1
 inp=(d/'input.txt').read_text().splitlines();header=inp[0].split();assert header[:6]==['TP','8','1','1','2',str(len(inp[1].split()))]
 a=np.fromfile(d/'service.bin',dtype='<f8').reshape(2,-1);assert np.isfinite(a).all() and (a>=0).all()
 local=np.ceil(a[1]).sum()/1e9;assert abs(r['cycle_ms']-local-r['network_ms'])<1e-6
 e=json.loads((d/'engine.json').read_text());assert e['ar_steps']==(278+3*r['depth'])*14*2
 assert sum(e['chip_tx_bytes'])==sum(e['chip_rx_bytes'])>0
 assert all(int(x['discard'])==0 for x in csv.DictReader((d/'engine-sue-ports.csv').open()))
 assert r['capacity_max_GB']<=1000
base=lookup['d0-draftx1']['cycle_ms'];assert abs(base-37.343877701)<1e-8
for r in json.loads((p/'estimates.json').read_text()):
 a=r['alpha'];depth=r['depth'];expected=(1-a**(depth+1))/(1-a)
 assert abs(expected-r['expected_committed_tokens'])<1e-12
 assert abs(r['cycle_ms']/expected-r['amortized_ms_per_token'])<1e-10
 assert abs(base/r['amortized_ms_per_token']-r['speedup'])<1e-10
result={'status':'passed','measured_cycles':len(rows),'baseline_matches_report_tp8_b1':True,'input_binary_source_hashes':True,'cycle_intervals_and_local_network_accounting':True,'ar_rounds_payload_and_zero_discard':True,'acceptance_expectations_recomputed':True}
(p/'audit.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
