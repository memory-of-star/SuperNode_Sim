"""Finite in-flight MPU requests: overlapping access latency and FIFO bandwidth."""
import json
from collections import defaultdict

def text(p):
 assert set(p)=={'bandwidth_bytes_s','max_inflight','requests'}
 assert type(p['bandwidth_bytes_s']) is int and p['bandwidth_bytes_s']>0
 assert type(p['max_inflight']) is int and 1<=p['max_inflight']<=65536
 seen=set()
 for r in p['requests']:
  assert set(r)=={'id','latency_ns'} and type(r['id']) is int and r['id']>=0 and r['id'] not in seen
  assert type(r['latency_ns']) is int and r['latency_ns']>=0;seen.add(r['id'])
 return f"{p['bandwidth_bytes_s']} {p['max_inflight']}\n"+''.join(f"{r['id']} {r['latency_ns']}\n" for r in p['requests'])

def write(case,hw,resources):
 from run import dump
 rate=round(hw['mpu_bytes_s']*hw['mpu_efficiency'])
 requests=[dict(id=r['id'],latency_ns=r['service_ns']-(r['units']*10**9+rate-1)//rate) for r in resources if r['kind']==2]
 p=dict(bandwidth_bytes_s=rate,max_inflight=hw['mpu_max_inflight'],requests=requests)
 compiled=text(p);dump(case/'mpu.json',p);(case/'mpu.tsv').write_text(compiled)

def validate(case,resources):
 p=json.loads((case/'mpu.json').read_text());assert (case/'mpu.tsv').read_text()==text(p)
 expected={r['id']:r for r in resources if r['kind']==2}
 assert {r['id'] for r in p['requests']}==set(expected)
 for r in p['requests']:
  q=expected[r['id']];rate=p['bandwidth_bytes_s']
  assert r['latency_ns']+(q['units']*10**9+rate-1)//rate==q['service_ns']
 return p

def analyze(run):
 from run import csvread
 resources=json.loads((run/'resources.json').read_text());p=validate(run,resources)
 latency={r['id']:r['latency_ns'] for r in p['requests']};expected={r['id']:r for r in resources if r['kind']==2}
 stats={int(r['id']):r for r in csvread(run/'resource_statistics.csv')}
 from io_die_config import managed
 for r in managed(run):expected.pop(r['service'])
 rows=csvread(run/'mpu_requests.csv');assert len(rows)==len(expected) and {int(r['id']) for r in rows}==set(expected);bychip=defaultdict(list)
 for row in rows:
  r={k:int(v) for k,v in row.items()};i=r['id'];q=expected[i]
  assert r['bytes']==q['units'] and r['submitted_ps']<=r['admitted_ps']<=r['latency_ready_ps']<=r['bandwidth_start_ps']<r['end_ps']
  assert r['latency_ready_ps']==r['admitted_ps']+latency[i]*1000
  assert r['end_ps']-r['bandwidth_start_ps']==((q['units']*10**9+p['bandwidth_bytes_s']-1)//p['bandwidth_bytes_s'])*1000
  assert int(stats[i]['service_start_ps'])==r['admitted_ps'] and int(stats[i]['end_ps'])==r['end_ps']
  bychip[r['chip']].append(r)
 for rs in bychip.values():
  rs.sort(key=lambda r:r['bandwidth_start_ps'])
  assert all(b['bandwidth_start_ps']>=a['end_ps'] and b['submitted_ps']>=a['submitted_ps'] for a,b in zip(rs,rs[1:]))
  events=sorted([(r['admitted_ps'],1) for r in rs]+[(r['end_ps'],-1) for r in rs]);live=0
  for _,delta in events:live+=delta;assert 0<=live<=p['max_inflight']
 nodes=csvread(run/'mpu_nodes.csv');assert len(nodes)==len(bychip) and {int(r['chip']) for r in nodes}==set(bychip)
 for r in nodes:
  assert int(r['pending'])==0 and int(r['peak_inflight'])<=p['max_inflight'] and int(r['bandwidth_bytes_s'])==p['bandwidth_bytes_s']
 return dict(requests=len(rows),bytes=sum(r['units'] for r in expected.values()),max_inflight=p['max_inflight'],bandwidth_bytes_s=p['bandwidth_bytes_s'])
