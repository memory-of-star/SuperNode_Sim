"""Per-Die capped max-min HBM sharing; explicit source identity, never per packet."""
import json,math
from collections import Counter
from pathlib import Path
DEFAULT=Path(__file__).resolve().parents[4]/'configs/hardware/hbm/fair-share.json'
def validate(p):
 assert set(p)=={'schema_version','policy','bandwidth_bytes_s_per_die','sources'}
 assert p['schema_version']==1 and p['policy']=='capped-max-min'
 assert type(p['bandwidth_bytes_s_per_die']) is int and p['bandwidth_bytes_s_per_die']>0
 ids=set();names=set()
 for s in p['sources']:
  assert set(s)=={'id','name','cap_bytes_s'}
  assert type(s['id']) is int and 0<=s['id']<100 and s['id'] not in ids
  assert isinstance(s['name'],str) and s['name'] and s['name'] not in names
  assert type(s['cap_bytes_s']) is int and s['cap_bytes_s']>=0
  ids.add(s['id']);names.add(s['name'])
 assert ids
 return p
def load(path=DEFAULT):return validate(json.loads(Path(path).read_text()))
def text(p):
 validate(p)
 return str(p['bandwidth_bytes_s_per_die'])+'\n'+''.join(f"{s['id']} {s['cap_bytes_s']}\n" for s in p['sources'])
def request_text(rows):return ''.join(f"{r['id']} {r['source']}\n" for r in rows)
def source_for(t,q):
 label=t['label']
 if q['kind']==5 or (':pcie:' in label and label.endswith(':hbm-write')):return 'pcie-ingress-write'
 if label.endswith(':hbm-write'):return 'sue-ingress-write'
 if 'expert-hbm' in label:return 'expert-read'
 if 'common-hbm' in label:return 'common-read'
 if ':kv:' in label and label.endswith(':hbm-consume'):return 'kv-consume'
 if ':engram:' in label and label.endswith(':hbm-consume'):return 'engram-consume'
 return 'other'
def check(p,rows,resources):
 validate(p);expected={q['id'] for q in resources if q['kind'] in (1,5)}
 assert len(rows)==len(expected) and {r['id'] for r in rows}==expected
 ids={s['id'] for s in p['sources']}
 for r in rows:assert set(r)=={'id','source'} and type(r['id']) is int and type(r['source']) is int and r['source'] in ids

def write(case,p,tasks,resources,rows=None):
 from run import dump
 ids={s['name']:s['id'] for s in p['sources']}
 if rows is None:rows=[dict(id=q['id'],source=ids[source_for(tasks[q['id']],q)]) for q in resources if q['kind'] in (1,5)]
 check(p,rows,resources)
 dump(case/'hbm.json',p);dump(case/'hbm-requests.json',rows)
 (case/'hbm.tsv').write_text(text(p));(case/'hbm-requests.tsv').write_text(request_text(rows))

def analyze(run):
 from run import csvread
 p=load(run/'hbm.json');mapping={r['id']:r['source'] for r in json.loads((run/'hbm-requests.json').read_text())}
 tasks=json.loads((run/'tasks.json').read_text());resources=json.loads((run/'resources.json').read_text())
 expected=Counter();counts=Counter()
 for q in resources:
  if q['kind']==1:
   t=tasks[q['id']];key=(t['src_chip'],t['src_die'],mapping[q['id']]);expected[key]+=q['units'];counts[key]+=1
 if (run/'pcie_descriptors.csv').exists():
  for r in csvread(run/'pcie_descriptors.csv'):
   key=(int(r['chip']),int(r['die']),mapping[int(r['id'])]);expected[key]+=int(r['hbm_written_bytes'])
 caps={s['id']:min(s['cap_bytes_s'] or p['bandwidth_bytes_s_per_die'],p['bandwidth_bytes_s_per_die']) for s in p['sources']}
 actual={};source_rows=csvread(run/'hbm_sources.csv')
 for r in source_rows:
  key=tuple(int(r[k]) for k in ('chip','die','source'));assert key not in actual;actual[key]=int(r['bytes'])
  assert int(r['pending'])==0 and int(r['requests'])==int(r['completed'])>=counts[key]
  assert int(r['cap_bytes_s'])==caps[key[2]]
  assert math.isclose(float(r['integrated_bytes']),int(r['bytes']),rel_tol=1e-9,abs_tol=.01)
  assert int(r['bytes'])<=int(r['active_ps'])*caps[key[2]]/1e12+.01
 assert dict(expected)==actual,(expected,actual)
 die_rows=csvread(run/'hbm_dies.csv')
 for r in die_rows:
  assert int(r['bandwidth_bytes_s'])==p['bandwidth_bytes_s_per_die']
  assert int(r['bytes'])==sum(v for (c,d,s),v in actual.items() if (c,d)==(int(r['chip']),int(r['die'])))
  assert math.isclose(float(r['integrated_bytes']),int(r['bytes']),rel_tol=1e-9,abs_tol=.01)
  assert int(r['bytes'])<=int(r['busy_ps'])*p['bandwidth_bytes_s_per_die']/1e12+.01
 return dict(policy=p['policy'],bytes=sum(actual.values()),sources=len(actual),dies=len(die_rows))
