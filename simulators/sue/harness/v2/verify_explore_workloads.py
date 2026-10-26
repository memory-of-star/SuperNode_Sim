#!/usr/bin/env python3
"""Default compatibility and experiment workload invariants, without simulation."""
import sys,json
from pathlib import Path
from workloads import build
from topologies import TOPOLOGIES
from run import check_tasks,DEFAULT,dump

def verify(baseline,dest):
    old=json.loads((Path(baseline)/'results.json').read_text());n=0
    for r in old:
        if r['topology'] in ('xpu8','xpu128-parity'):
            tasks,_,_=build(TOPOLOGIES[r['topology']],r['model'],r['window'])
            assert tasks==json.loads((Path(r['run'])/'tasks.json').read_text());n+=1
    for model in ('kimi-k3','glm-5.3'):
        for profile in ('uniform','hotspot80','community80'):
            batches=[]
            for placement in ('spread','affinity'):
                tasks,routes,meta=build(TOPOLOGIES['xpu128-parity'],model,'decode',2,tokens=256,compute_ns=0,profile=profile,placement=placement)
                check_tasks(TOPOLOGIES['xpu128-parity'],tasks,DEFAULT)
                assert len(routes)==256*4*meta['top_k']
                assert all(t['delay_ns']==0 for t in tasks if not t['bytes'])
                bins={}
                for row in routes:bins.setdefault((row['step'],row['token']),[]).append(row['expert'])
                assert all(len(v)==len(set(v))==meta['top_k'] for v in bins.values())
                batches.append([(r['step'],r['token'],r['expert'],r['dst_chip']) for r in routes])
            assert batches[0]==batches[1]
    result=dict(status='passed',unchanged_legacy_workloads=n,custom_workloads=12,checks=['DAG and payload validity','token/top-k totals and expert uniqueness','zero computation waits','placement preserves expert choices and chip'])
    dump(Path(dest)/'workload-verification.json',result);print(json.dumps(result))

if __name__=='__main__':verify(sys.argv[1],sys.argv[2])
