"""Select seed1 static shares, confirm on seeds2/3, and combine immutable ledgers."""
from concurrent.futures import ThreadPoolExecutor,as_completed
from collections import defaultdict
import json
from pathlib import Path
import sys
import traceback

from offload_experiments import execute
from run import dump


def main(root):
    root=Path(root).resolve()
    main_root=root/'matrix'
    results=json.loads((main_root/'results.json').read_text())
    design=json.loads((main_root/'design.json').read_text())
    assert len(results)==len(design) and all(r['status']=='passed' for r in results)
    contract=json.loads((root/'hybrid-selection-plan.json').read_text())
    candidates=defaultdict(list)
    for r in results:
        if r.get('study_variant')=='share-sensitivity':candidates[r['model'],r['target']].append(r)
    selected=[];defs=[]
    for key,rs in sorted(candidates.items()):
        best=min(rs,key=lambda r:r['block_us'])
        selected.append(dict(model=key[0],target=key[1],mpus=8,mpu_fraction=best['mpu_fraction'],
                             selection_seed=1,selection_block_us=best['block_us']))
        for seed in (2,3):
            d={**design[best['case_index']],'seed':seed,'study_variant':'share-confirmation'}
            defs.append(d)
    out=root/'confirmation';out.mkdir(exist_ok=False)
    dump(out/'selection.json',selected);dump(out/'plan.json',contract);dump(out/'design.json',defs)
    confirmed=[]
    with ThreadPoolExecutor(max_workers=3) as pool:
        futures={pool.submit(execute,out,d,i):(i,d) for i,d in enumerate(defs)}
        for future in as_completed(futures):
            i,d=futures[future]
            try:r=future.result()
            except Exception as exc:r=dict(**d,status='runner_error',error=str(exc),traceback=traceback.format_exc())
            r['case_index']=i;confirmed.append(r)
            dump(out/'results.json',sorted(confirmed,key=lambda r:r['case_index']))
            print(json.dumps(dict(done=len(confirmed),total=len(defs),model=d['model'],target=d['target'],
                                 seed=d['seed'],fraction=d['mpu_fraction'],status=r['status'],block_us=r.get('block_us'))),flush=True)
    report=root/'analysis';report.mkdir(exist_ok=False)
    combined=sorted(results,key=lambda r:r['case_index'])+[
        {**r,'case_index':len(results)+r['case_index'],'confirmation_case_index':r['case_index']} for r in sorted(confirmed,key=lambda r:r['case_index'])]
    dump(report/'results.json',combined);dump(report/'design.json',design+defs)
    dump(report/'plan.json',dict(primary=json.loads((main_root/'plan.json').read_text()),confirmation=contract,
                                selected=selected,immutable_primary_results=str(main_root/'results.json'),
                                immutable_confirmation_results=str(out/'results.json')))
    if any(r['status']!='passed' for r in confirmed):raise SystemExit(1)


if __name__=='__main__':main(sys.argv[1])
