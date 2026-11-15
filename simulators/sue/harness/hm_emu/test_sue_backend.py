"""Protocol evidence: delivery, real CBFC throttling, real LLR retransmission."""
import csv,json,os,pathlib,subprocess,sys
import numpy as np
from run import ROOT
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=False)
results={}
for name,credit,bad in [('clean',512,0),('credit_limited',16,0),('llr_errors',512,17)]:
 p=out/name;p.mkdir();np.zeros((2,1,1),dtype='<f8').tofile(p/'service.bin')
 (p/'input.txt').write_text(f'TP 2 1 1 2 1 0 128000000000 2000000 1000 {p.resolve()}/service.bin\n1048576\nsingle_switch\n')
 with (p/'stdout.log').open('w') as stdout,(p/'stderr.log').open('w') as stderr:
  subprocess.run([str(ROOT/'results/build/hm-emu-sue-engine'),str(p/'input.txt'),str(p/'engine')],check=True,timeout=90,stdout=stdout,stderr=stderr,env={**os.environ,'HM_SUE_PAYLOAD':'2048','HM_SUE_CREDITS':str(credit),'HM_SUE_BAD_EVERY':str(bad)})
 r=json.loads((p/'engine.json').read_text())
 with (p/'engine-sue-ports.csv').open() as f:rows=list(csv.DictReader(f))
 assert r['finished_tokens']==2 and sum(r['chip_rx_bytes'])==4194304 and r['chip_tx_bytes']==r['chip_rx_bytes']
 results[name]={'makespan_ps':r['makespan_ps'],'replay_frames':sum(int(x['replay_frames']) for x in rows),'credit_exhausted':sum(int(x['credit_exhausted']) for x in rows),'discard':sum(int(x['discard']) for x in rows)}
 assert not results[name]['discard']
assert results['credit_limited']['makespan_ps']>results['clean']['makespan_ps']*2
assert results['credit_limited']['credit_exhausted']>0
assert results['llr_errors']['replay_frames']>0
(out/'verification.json').write_text(json.dumps({'status':'passed','cases':results},indent=2)+'\n');print(json.dumps(results,indent=2))
