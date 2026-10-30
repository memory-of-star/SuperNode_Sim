"""Chapter 8: partial local-HBM/MPU KV placement and paired IO packing study.

Use frozen chapter-8 hardware/model/settings. No PCIe backend or model-wide
latency extrapolation. Every run is validated by run_case before analysis.
"""
import argparse
import copy
import json
import math
import os
import shutil
from collections import Counter
from pathlib import Path

from inference_experiments import DAG, model_block
from inference_profile import profile, kv_budget
from run import create, run_case, dump, csvread, sha
import hbm_config
from trace_storage import compress_completed


class PartialKv:
    kv = True
    engram = False  # Preserve the chapter-8 Engram read/stage DAG exactly.

    def __init__(self, definition, hardware, fraction):
        self.d = definition
        self.dag = DAG(hardware)
        self.m = profile(definition['model'], definition['model_input'])
        self.fraction = fraction
        self.requests = []

    def read_kv(self, sa, batch_group, tp, context, deps, label):
        _, traffic, _ = kv_budget(self.m, context)
        full = traffic * batch_group / self.m['layers'] / tp
        # Cache residency and accesses use the same uniform 25% share.
        # Whole 256-B AXI units avoid fabricating a partially useful final unit.
        remote = int(full * self.fraction // 256) * 256
        ready = []
        for index, offset in enumerate(range(0, remote, 262144)):
            size = min(262144, remote-offset)
            bank = (sa[0]*4+sa[1]+index) % self.d['mpus']
            tag = f'{label}:mpu:chunk{index}'
            done = self.dag.remote_read(sa, self.d['chips']+bank, size, deps, tag)
            ready.append(self.dag.local(*sa, 1, size, done, tag+':hbm-consume'))
        self.requests.append(dict(label=label, chip=sa[0], die=sa[1], total_read_bytes=full,
                                  mpu_read_bytes=remote, local_read_bytes=full-remote))
        return full-remote, ready


def business(task):
    label=task['label']
    if ':kv:' in label:return 'KV'
    if ':engram' in label:return 'Engram'
    if label.endswith((':dispatch', ':combine')):return 'MoE'
    if task['bytes']:raise ValueError('Unclassified network task: '+label)
    return 'local'


def make_case(root, src, fraction, mode, sample_ns, name, time_resolution_ns=0):
    spec=json.loads((src/'experiment.json').read_text())
    definition=copy.deepcopy(spec['workload']['definition'])
    hw=copy.deepcopy(spec['workload']['hardware'])
    definition['hardware_overrides']=hw
    offload=PartialKv(definition, hw, fraction)
    dag,routes,meta=model_block(definition, offload=offload if fraction else None)
    if not fraction:
        assert dag.tasks==json.loads((src/'tasks.json').read_text()),'baseline DAG drift'
        assert dag.resources==json.loads((src/'resources.json').read_text()),'baseline resource drift'
    else:
        for t in dag.tasks:
            if t['bytes']:t['vc']={'MoE':0,'KV':1,'Engram':2}[business(t)]
    cap=copy.deepcopy(spec['workload']['capacity'])
    m=profile(definition['model'], definition['model_input'])
    kv_capacity,_,_=kv_budget(m,definition['context'])
    moved=kv_capacity*definition['batch']*fraction
    # Report whole-model capacity separately from two-block traffic.
    placement=dict(kv_mpu_fraction=fraction, kv_local_fraction=1-fraction,
                   kv_capacity_moved_bytes=moved, baseline_capacity=cap,
                   extra_mpu_bytes_per_node=moved/definition['mpus'],
                   hbm_freed_bytes_per_die=moved/(definition['chips']*4),
                   assumption='uniform static KV residency and access share; no KV writeback/prefetch/cache misses',
                   requests=offload.requests)
    assert m['engram_bytes']+moved <= definition['mpus']*hw['mpu_capacity_bytes']
    meta.update(study='chapter8-vc-packing', placement=placement,
                vc_map={'MoE':0,'KV':1,'Engram':2} if fraction else {'all':0})
    io=json.loads((src/'io-die.json').read_text())['profile'];io['mode']=mode
    io_path=root/f'io-{mode}.json';dump(io_path,io)
    os.environ['SUE_IO_DIE_PROFILE']=str(io_path)
    # Frozen source inputs can explicitly request NS; override for a PS rerun.
    settings={**spec['settings'],'port_sample_ns':sample_ns,'timeout_s':300,
              'time_resolution_ns':time_resolution_ns}
    case=create(root/'cases'/name,spec['topology']['name'],dag.tasks,meta,settings,
                routes=routes,resources=dag.resources)
    hbm_config.write(case,json.loads((src/'hbm.json').read_text()),dag.tasks,dag.resources)
    for filename in ('model-config.json','model-simulation.json','model-source.json','parallel.json','parallel-layout.json'):
        if (src/filename).exists():shutil.copy2(src/filename,case/filename)
    dump(case/'placement.json',placement)
    dump(case/'replay-source.json',dict(run=str(src),task_sha256=sha(src/'tasks.json'),resource_sha256=sha(src/'resources.json')))
    return case


def main():
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--time-resolution-ns',type=int,choices=(0,1),default=0)
    p.add_argument('--compare-study',type=Path,help='Validate unchanged workload against a prior VC study')
    a=p.parse_args();root=a.output.resolve();root.mkdir(parents=True,exist_ok=False)
    old=json.loads((a.source/'results.json').read_text())
    sources={r['seed']:Path(r['run']) for r in old if r.get('sample_ns')==500}
    assert set(sources)=={1,2,3}
    previous={r['arm']:Path(r['run']) for r in json.loads((a.compare_study/'results.json').read_text())} if a.compare_study else {}
    plan=dict(kv_mpu_fraction=.25,vc_map={'MoE':0,'KV':1,'Engram':2},seeds=[1,2,3],
              modes=['best_effort','none'],sample_ns=500,credits_each_vc=512,credit_bytes=256,
              time_resolution_ns=a.time_resolution_ns,
              controls=['frozen baseline input equality','seed1 partial-KV telemetry off/on exact match'],
              prediction='KV introduces AR/R on VC1; packing reduces frame count and credit rounding overhead. Peak per-port CU, rather than aggregate byte share alone, constrains credit allocation.',
              rate_definition='switch egress only; data frames separate from local control; 500ns full bins anchored at time zero',
              source=str(a.source.resolve()),compare_study=str(a.compare_study.resolve()) if a.compare_study else None)
    dump(root/'registered-plan.json',plan)
    shutil.copy2(__file__,root/'runner.py')
    results=[]
    def execute(seed,fraction,mode,sample,name):
        case=make_case(root,sources[seed],fraction,mode,sample,name,a.time_resolution_ns)
        if name in previous:
            oldrun=previous[name]
            for f in ('tasks.json','resources.json','token_routes.csv','hbm.json','hbm-requests.json',
                      'io-die.json','io-die.tsv','io-mpu.tsv','sue.conf','ports.tsv','path_plan.csv'):
                assert sha(case/f)==sha(oldrun/f),f+' changed relative to prior study'
            before=json.loads((oldrun/'experiment.json').read_text())['settings']
            after=json.loads((case/'experiment.json').read_text())['settings']
            assert {k:v for k,v in before.items() if k!='time_resolution_ns'}=={k:v for k,v in after.items() if k!='time_resolution_ns'}
        r=run_case(case);r.update(seed=seed,kv_fraction=fraction,mode=mode,sample_ns=sample,arm=name)
        results.append(r);dump(root/'results.json',results)
        print(json.dumps({k:r.get(k) for k in ('arm','status','makespan_us','wall_s','failures')}),flush=True)
        assert r['status']=='passed',r
        compress_completed(Path(r['run']))
        return Path(r['run'])
    baseline=execute(1,0,'best_effort',0,'baseline-replay')
    comparison=['task_statistics.csv','port_statistics.csv','ring_statistics.csv','resource_statistics.csv']
    baseline_same_grid=a.time_resolution_ns==json.loads((sources[1]/'experiment.json').read_text())['settings'].get('time_resolution_ns',0)
    if baseline_same_grid:
        for f in comparison:assert sha(baseline/f)==sha(sources[1]/f),f+' baseline changed'
    control=execute(1,.25,'best_effort',0,'partial-kv-control-off')
    for seed in (1,2,3):
        pair=[]
        for mode in ('best_effort','none'):
            run=execute(seed,.25,mode,500,f'seed{seed}-{mode}')
            pair.append(run)
            if seed==1 and mode=='best_effort':
                for f in comparison:assert sha(control/f)==sha(run/f),f+' observer effect'
        for f in ('tasks.json','resources.json','token_routes.csv','hbm.json','hbm-requests.json'):
            assert sha(pair[0]/f)==sha(pair[1]/f),f+' unfair packing comparison'
        profiles=[json.loads((r/'io-die.json').read_text())['profile'] for r in pair]
        assert {k:v for k,v in profiles[0].items() if k!='mode'}=={k:v for k,v in profiles[1].items() if k!='mode'}
    assert len({json.loads((Path(r['run'])/'command-manifest.json').read_text())['binary_sha256'] for r in results})==1
    dump(root/'verification.json',dict(status='passed',cases=len(results),baseline_input_exact=True,
        baseline_output_exact=True if baseline_same_grid else None,
        time_resolution_ns=a.time_resolution_ns,prior_study_inputs_identical=bool(previous),
        telemetry_no_observer_effect=True,paired_inputs_identical_except_packing=True,
        task_bytes_dependencies_axi_hbm_mpu_validated=True))

if __name__=='__main__':main()
