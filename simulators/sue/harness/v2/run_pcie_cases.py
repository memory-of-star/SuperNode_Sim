"""Reproducible integration matrix: full DeepSeek case, small Kimi/GLM probes."""
import argparse,json,traceback,copy,shutil
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
from offload_experiments import execute,definition,DS
from run import dump,sha,SOURCE,BINARY
import pcie_config

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('output',type=Path)
    ap.add_argument('--pcie-profile',type=Path,default=pcie_config.DEFAULT);ap.add_argument('--workers',type=int,default=2)
    a=ap.parse_args();root=a.output.resolve();root.mkdir(parents=True,exist_ok=False);p=pcie_config.load(a.pcie_profile)
    defs=[]
    for backend,mpus in [('pcie',0),('hybrid',8),('mpu',8)]:
        for detailed in (False,True):
            d=definition(DS,'both',backend,mpus);d['study_variant']='transaction' if detailed else 'legacy'
            if detailed:d['pcie_profile']=p
            defs.append(d)
    for seed in (2,3):
        d=definition(DS,'both','pcie',0,seed);d.update(study_variant='transaction',pcie_profile=p);defs.append(d)
    for model in ('kimi-k3','glm-5.3'):
        for backend,mpus in [('pcie',0),('hybrid',8)]:
            d=definition(model,'kv',backend,mpus);d.update(batch=8,context=2048,steps=1,study_variant='small-integration',pcie_profile=p);defs.append(d)
    dump(root/'design.json',defs)
    dump(root/'plan.json',dict(prediction='Finite transaction windows and queues change PCIe time. MPU-only task timings must be unchanged. No guaranteed MPU speedup. Kimi/GLM probes are smaller and cannot be compared as model rankings.',cases=len(defs),binary_sha256=sha(BINARY),adapter_sha256=sha(SOURCE),pcie_header_sha256=sha(SOURCE.parent/'sue-pcie-model.h')))
    source=root/'source';source.mkdir()
    for f in [SOURCE,SOURCE.parent/'sue-pcie-model.h',Path(__file__),Path(__file__).with_name('pcie_config.py'),Path(__file__).with_name('offload_experiments.py')]:shutil.copy2(f,source/f.name)
    results=[]
    with ThreadPoolExecutor(max_workers=a.workers) as pool:
        futures={pool.submit(execute,root,d,i):i for i,d in enumerate(defs)}
        for f in as_completed(futures):
            i=futures[f]
            try:r=f.result()
            except Exception as exc:r={'status':'runner_error','error':str(exc),'traceback':traceback.format_exc()}
            r['case_index']=i;results.append(r);dump(root/'results.json',sorted(results,key=lambda x:x['case_index']))
            print(json.dumps({k:r.get(k) for k in ('case_index','model','backend','seed','status','block_us','error')}),flush=True)
    assert all(r['status']=='passed' for r in results)
    indexed={r['case_index']:r for r in results}
    for name in ('task_statistics.csv','port_statistics.csv','resource_statistics.csv'):
        assert (Path(indexed[4]['run'])/name).read_bytes()==(Path(indexed[5]['run'])/name).read_bytes(),name
    dump(root/'verification.json',dict(status='passed',cases=len(results),mpu_only_exact_regression=True))
if __name__=='__main__':main()
