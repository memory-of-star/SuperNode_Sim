#!/usr/bin/env python3
"""Acceptance tests exercise real SUE packets; preserve negative-case outputs."""
import json
from pathlib import Path
from run import *
from topologies import *

def main():
    dest=HERE/'validation'/stamp();dest.mkdir(parents=True);checks=[]
    def execute(name,topo,ts,settings=None,expected=True):
        c=create(dest/name,topo,ts,dict(model='acceptance',window=name),settings)
        r=run_case(c);assert (r['status']=='passed')==expected,(name,r)
        checks.append(dict(name=name,assertion_passed=True,run=r['run'],simulation_status=r['status']))
        print(name,r['status'],round(r['wall_s'],3),flush=True);return r
    for name,t in TOPOLOGIES.items():
        # Every XPU/Die originates a same-Die cross-card transaction; both
        # groups are exercised by the different source/message hash inputs.
        ts=[task(c,d,(c+1)%t.xpu,d,513,1000,group=(c+d)%2) for c in range(t.xpu) for d in range(4)]
        ts += [task(0,a,t.xpu-1,b,5001,1000,group=g) for a in range(4) for b in range(4) for g in range(2)]
        if t.mpu:
            for d in range(4):
                for j in range(t.mpu):
                    m=t.xpu+j
                    if t.family=='pool' and d%2!=m%2:continue
                    ts += [task(0,d,m,0,513,1000),task(m,0,0,d,513,1000)]
            for j in range(t.mpu):
                dst=t.xpu+((j+2 if t.family=='pool' else j+1)%t.mpu)
                ts.append(task(t.xpu+j,0,dst,0,513,1000))
        r=execute(name,name,ts)
        observed={int(k) for k,v in r['task_path_counts'].items() if v}
        expected={0,1,2} if t.family=='pure' else {0,1} if t.family in ('parity','rotate','mixed','pool') else {0}
        assert observed==expected,(name,observed)
        rows=csvread(Path(r['run'])/'task_statistics.csv')
        assert {int(x['port']) for x in rows}==set(range(8)),(name,'not every rail exercised')
    r=execute('wrong-mpu-group','mixed64-64',[task(0,0,64,0,513,1000,group=1)],{'stop_ns':10000},False)
    assert r['completed']==0 and r['received_bytes']==0
    r=execute('cross-pool','mixed64-64-pool',[task(0,0,65,0,513,1000),task(64,0,65,0,513,1000)],{'stop_ns':10000},False)
    assert r['completed']==0
    r=execute('llr-errors','xpu8',[task(0,0,7,3,100005,1000)],{'bad_every':10})
    assert r['replay_frames']>0 and r['discard']==0
    stress=[task(0,0,255,2,9001,1000) for _ in range(24)]
    fast=execute('ring-fast','xpu256',stress,{'ring_gbps':400,'ring_bytes':8192})
    slow=execute('ring-slow','xpu256',stress,{'ring_gbps':100,'ring_bytes':8192})
    assert slow['makespan_us']>fast['makespan_us'] and slow['ucie_queue_ps']>0
    assert fast['ring_peak_bytes']<=8192 and slow['ring_peak_bytes']<=8192
    for r in (fast,slow):
        # 2 hops for all request bytes and 64B response per transaction.
        assert r['ucie_byte_hops']==2*(24*9001+24*64)
    r=execute('dag-fragments','xpu128-parity',[
        task(0,0,127,1,5001,1000),task(1,2,127,1,7003,1000),
        task(127,1,127,1,0,333,[0,1]),task(127,1,0,0,9007,0,[2])])
    assert r['received_bytes']==21011
    # Deterministic seed: the repeated simulation must have identical timing,
    # bytes and path selections; process wall time is deliberately excluded.
    r2=run_case(Path(r['run']).parents[1])
    assert (Path(r['run'])/'task_statistics.csv').read_bytes()==(Path(r2['run'])/'task_statistics.csv').read_bytes()
    checks.append(dict(name='repeat-determinism',assertion_passed=True,run=r2['run'],simulation_status=r2['status']))
    # Validate malformed inputs before execution, not by a successful ns-3 run.
    malformed=[dict(task(0,0,1,0,513),id=0,deps=[0])]
    try:check_tasks(TOPOLOGIES['xpu8'],malformed,DEFAULT)
    except AssertionError:pass
    else:raise AssertionError('cycle accepted')
    checks.append(dict(name='cyclic-input-rejected',assertion_passed=True))
    for topology_name in BASE_TOPOLOGY_NAMES:
        topo=TOPOLOGIES[topology_name]
        for model in ('kimi-k3','glm-5.3'):
            for window in ('prefill','decode','train'):
                ts,rs,meta=build(topo,model,window);check_tasks(topo,ts,DEFAULT)
                assert len(rs)==meta['global_tokens_per_step']*meta['steps']*meta['top_k']
    checks.append(dict(name='96-workload-DAG-byte-conservation',assertion_passed=True))
    dump(dest/'verification.json',dict(status='passed',checks=checks))
    dump(HERE/'validation'/'latest.json',dict(directory=str(dest)))
    print('ALL V2 ACCEPTANCE CHECKS PASSED',dest,flush=True)
if __name__=='__main__':main()
