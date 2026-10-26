"""Check chip-wide PCIe sharing, paired workloads, and historical DAG compatibility."""
import importlib.util
import json
from pathlib import Path
import sys
from collections import Counter

from inference_experiments import model_block
from offload_experiments import Offload, definition, MODELS, DS
from run import create, run_case, csvread, dump, check_resources
from topologies import TOPOLOGIES
from workloads import task


def main(root):
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=False)
    tasks = [task(c,d,c,d,0,delay=1000,label='pcie-sharing') for c,d in [(0,0),(0,1),(1,0),(0,2)]]
    resources = [dict(id=i,kind=4 if i<3 else 1,service_ns=1000,units=128000,peer_die=-1) for i in range(4)]
    case = create(root/'pcie-sharing', 'hw-4-0', tasks, {'window':'pcie-resource-validation'}, resources=resources)
    r = run_case(case)
    assert r['status']=='passed',r
    stats = csvread(Path(r['run'])/'resource_statistics.csv')
    assert [int(x['end_ps']) for x in stats] == [2000000,3000000,2000000,2000000], stats
    assert [int(x['queued_ps']) for x in stats] == [0,1000000,0,0],stats
    rejected = False
    bad = [task(4,0,4,0,0,delay=1000)]
    try:
        check_resources(TOPOLOGIES['hw-4-1'], bad,
                        [dict(id=0,kind=4,service_ns=1000,units=128000,peer_die=-1)],1000000)
    except AssertionError:
        rejected=True
    assert rejected,'PCIe service must be on XPU, not MPU'
    checks=['PCIe shared across Die on one chip', 'PCIe independent across chips',
            'PCIe overlaps HBM', 'PCIe kind rejected on MPU']
    for model,target in [(m,'kv') for m in MODELS]+[(DS,'engram'),(DS,'both')]:
        reference = None
        for backend,mpus in [('pcie',0),('mpu',1),('mpu',16),('hybrid',8),('tiered',8)]:
            if backend=='tiered' and target!='both':
                continue
            d = definition(model,target,backend,mpus)
            offload = Offload(d)
            dag,routes,meta = model_block(d,offload=offload)
            volume = {target:sum(v for k,v in offload.bytes.items() if k.startswith(target+'_')) for target in ('kv','engram')}
            compute = Counter()
            for q in dag.resources:
                if q['kind']==0:
                    t=dag.tasks[q['id']]
                    compute[t['label'],t['src_chip'],t['src_die']]+=q['units']
            logical=(routes,volume,compute)
            if reference is None:reference=logical
            assert logical==reference,(model,target,backend,'workload changed')
            assert offload.capacity()['admitted'],(model,target,backend,offload.capacity())
            for req in offload.requests:
                t=dag.tasks[req['consumer_task']]
                assert t['deps'] and t['label'].endswith('hbm-consume')
        checks.append(model+':'+target+' same routes, compute and requested bytes across backends')
    # Exact no-offload compatibility against the saved implementation.
    original=root.parent/'source-before/inference_experiments.py'
    spec=importlib.util.spec_from_file_location('offload_original_inference',original)
    old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
    for model in MODELS:
        d=definition(model,'kv','pcie',0)
        a,ra,_=old.model_block(d)
        b,rb,_=model_block(d)
        assert a.tasks==b.tasks and a.resources==b.resources and ra==rb,model
    checks.append('existing three-model no-offload tasks/resources/routes unchanged')
    dump(root/'verification.json',dict(status='passed',checks=checks,pcie_probe=r))
    print(json.dumps({'status':'passed','checks':checks},ensure_ascii=False))


if __name__=='__main__':main(sys.argv[1])
