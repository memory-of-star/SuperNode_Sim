#!/usr/bin/env python3
"""Freeze a small, cited evidence subset; never rerun a simulation implicitly."""
import csv,json,hashlib,shutil
from pathlib import Path
P=Path(__file__).resolve().parents[1];R=P.parents[1];E=P/'evidence';E.mkdir(exist_ok=True)
sources={'packing-summary.csv':R/'docs/manual/vc-packing/summary.csv','precision-analysis.json':R/'results/sue/pcie-precision-audit/analysis.json','packing-study.md':R/'docs/manual/08-deepseek-16chip-case.md'}
manifest={}
for name,path in sources.items():
 raw=path.read_bytes();manifest[name]={'source':str(path.relative_to(R)),'sha256':hashlib.sha256(raw).hexdigest()}
 if name=='packing-summary.csv':
  rows=list(csv.DictReader(raw.decode().splitlines()))
  with (E/name).open('w') as f:
   fields=[k for k in rows[0] if k!='run'];w=csv.DictWriter(f,fields);w.writeheader();w.writerows({k:r[k] for k in fields} for r in rows)
 elif name=='precision-analysis.json':
  d=json.loads(raw)
  for row in d['results']:row.pop('run',None)
  (E/name).write_text(json.dumps(d,indent=2)+'\n')
manifest['scope']='Frozen summaries, not full raw artifact; source run paths remain in local results. No model outputs are newly simulated by this script.'
(E/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
with (E/'packing.dat').open('w') as f:
 f.write('seed none packed noneframes packedframes reduction\n')
 for seed in ['1','2','3']:
  a=next(r for r in rows if r['seed']==seed and r['mode']=='none');b=next(r for r in rows if r['seed']==seed and r['mode']=='best_effort')
  f.write(f"{seed} {a['window_us']} {b['window_us']} {a['data_frames']} {b['data_frames']} {100*(1-float(b['window_us'])/float(a['window_us'])):.5f}\n")
print((E/'packing.dat').read_text())
