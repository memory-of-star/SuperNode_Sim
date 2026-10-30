"""Archive a completed inference experiment directory and reproducible harness sources."""
import sys,zipfile,shutil,json,hashlib
from pathlib import Path
root=Path(sys.argv[1]).resolve();project=Path(__file__).resolve().parents[2]
src=root/'source-final';src.mkdir(exist_ok=True)
shutil.copytree(project/'harness'/'v2',src/'harness'/'v2',dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','cases','validation','*.pyc'))
scratch=src/'ns-3-alibabacloud'/'simulation'/'scratch';scratch.mkdir(parents=True,exist_ok=True)
shutil.copy2(project/'ns-3-alibabacloud'/'simulation'/'scratch'/'sue-harness-v3.cc',scratch)
(root/'README.md').write_text('''# SUE v2 hardware inference evidence

Open report.html offline. summary.csv retains every planned condition, including capacity rejections. verification.json audits input hashes, FIFO service intervals, payload completion and paired expert selections. browser-verification.json records report acceptance checks.

The source-final folder contains the final harness and adapter; source-executed preserves the main matrix implementation at execution time; source-before preserves the previous adapter and runner. Source snapshots are changes for the existing simulators/sue project, not a standalone copy of the full ns-3 source tree. See source-final/harness/v2/RESOURCE_MODEL.md for reproduction and assumptions. Inputs are frozen configurations and primary model sources. Commands, input hashes and simulator binary SHA256 are recorded per run. Absolute run paths in JSON refer to the original machine; evidence links in the report are relative.

No result here is a measurement of complete-model TPOT or TTFT. Capacity checks, analytical resource budgets and simulated phase windows are separate evidence classes.
''')
out=root/'hardware-evidence.zip'
with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=3,allowZip64=True) as z:
 for p in sorted(root.rglob('*')):
  if p.is_file() and p!=out and '__pycache__' not in p.parts and p.suffix!='.zip':z.write(p,p.relative_to(root))
h=hashlib.sha256()
with out.open('rb') as f:
 for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
print(json.dumps({'file':str(out),'bytes':out.stat().st_size,'sha256':h.hexdigest()}))
