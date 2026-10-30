from trace_storage import open_trace, exists as trace_exists
"""AXI4-sized transaction transport; channel fields are modeling assumptions, not a standard wire format."""
import json,math,os
from pathlib import Path
FIELDS={'schema_version','mode','transaction_payload_bytes','address_bits','id_bits','bus_data_bits','user_bits','record_header_bytes','packet_header_bytes','max_packet_payload_bytes','issue_interval_ns','queue_records_per_port','min_mac_frame_bytes'}
def sizes(p):
 assert set(p) in (FIELDS,FIELDS|{'mpu_service_unit'})
 assert p.get('mpu_service_unit','batch') in ('batch','axi')
 assert set(p)-{'mpu_service_unit'}==FIELDS and p['schema_version']==1 and p['mode'] in ('none','best_effort')
 for k in FIELDS-{'mode'}:assert type(p[k]) is int and p[k]>=0,k
 assert p['transaction_payload_bytes']==256 and 1<=p['address_bits']<=64 and 1<=p['id_bits']<=32
 assert p['bus_data_bits'] in (32,64,128,256,512,1024) and p['user_bits']<=256
 assert 1<=p['issue_interval_ns']<=1000000 and 1<=p['queue_records_per_port']<=65536
 assert 64<=p['min_mac_frame_bytes']<=256
 assert 8<=p['record_header_bytes']<=256 and 0<=p['packet_header_bytes']<=256
 beats=2048//p['bus_data_bits'];byte=lambda bits:(bits+7)//8
 a=byte(p['address_bits']+p['id_bits']+29+p['user_bits'])
 w=byte(256+beats*(1+p['user_bits'])) # WSTRB: 1 bit per data byte; WLAST each beat
 r=byte(beats*(p['id_bits']+2+1+p['user_bits'])) # RID, RRESP, RLAST per beat
 b=byte(p['id_bits']+2+p['user_bits'])
 q={'AW_W':a+w+256+p['record_header_bytes'],'AR':a+p['record_header_bytes'],'B':b+p['record_header_bytes'],'R':r+256+p['record_header_bytes']}
 assert p['packet_header_bytes']+max(q.values())<=p['max_packet_payload_bytes']<=8192
 return q

def mapping(tasks):
 # Recognize the explicit remote-read DAG, never infer direction solely from chip type.
 result={t['id']:dict(id=t['id'],kind='AW_W',count=(t['bytes']+255)//256,parent=-1) for t in tasks if t['bytes']}
 for t in tasks:
  if not t['bytes'] or not t['label'].endswith(':data-response'):continue
  assert len(t['deps'])==1,'read response must depend on one service'
  svc=tasks[t['deps'][0]];assert len(svc['deps'])==1
  req=tasks[svc['deps'][0]]
  assert req['label'].endswith(':request') and req['bytes'] and req['id']<t['id']
  assert (req['src_chip'],req['src_die'],req['dst_chip'],req['dst_die'])==(t['dst_chip'],t['dst_die'],t['src_chip'],t['src_die'])
  n=(t['bytes']+255)//256
  result[req['id']]=dict(id=req['id'],kind='AR',count=n,parent=-1)
  result[t['id']]=dict(id=t['id'],kind='R',count=n,parent=req['id'])
 return list(result.values())

def compiled(p,rows):
 q=sizes(p)
 return ' '.join(map(str,[int(p['mode']=='best_effort'),p['max_packet_payload_bytes'],p['issue_interval_ns'],p['queue_records_per_port'],p['packet_header_bytes'],p['min_mac_frame_bytes'],*[q[k] for k in ('AW_W','AR','B','R')]]))+'\n'+''.join(f"{r['id']} {('AW_W','AR','B','R').index(r['kind'])} {r['count']} {r['parent']}\n" for r in rows)

def write(case,profile,tasks,hw=None,xpu=None):
 from run import dump
 p=json.loads(Path(profile).read_text());rows=mapping(tasks)
 (case/'io-die.tsv').write_text(compiled(p,rows))
 spec=dict(profile=p,tasks=rows)
 if p.get('mpu_service_unit')=='axi':
  reads=[dict(request=r['parent'],response=r['id'],service=tasks[r['id']]['deps'][0],count=r['count']) for r in rows if r['kind']=='R']
  writes=[r['id'] for r in rows if r['kind']=='AW_W' and xpu is not None and tasks[r['id']]['dst_chip']>=xpu]
  if reads or writes:
   assert hw is not None,'AXI MPU mode requires hardware profile'
   spec['axi_mpu']=dict(bandwidth_bytes_s=round(hw['mpu_bytes_s']*hw['mpu_efficiency']),max_inflight=hw.get('mpu_max_inflight',64),latency_ns=int(hw['mpu_fixed_ns']),reads=reads,writes=writes)
   (case/'io-mpu.tsv').write_text(memory_text(spec['axi_mpu']))
   (case/'io-mpu-writes.tsv').write_text(''.join(f'{i}\n' for i in writes))
 dump(case/'io-die.json',spec)

def validate(case,tasks,settings):
 p=json.loads((case/'io-die.json').read_text());assert set(p) in ({'profile','tasks'},{'profile','tasks','axi_mpu'})
 assert p['tasks']==mapping(tasks),'AXI task mapping mismatch'
 assert (case/'io-die.tsv').read_text()==compiled(p['profile'],p['tasks'])
 if 'axi_mpu' in p:
  assert p['profile'].get('mpu_service_unit')=='axi'
  assert (case/'io-mpu.tsv').read_text()==memory_text(p['axi_mpu'])
  expect=[dict(request=r['parent'],response=r['id'],service=tasks[r['id']]['deps'][0],count=r['count']) for r in p['tasks'] if r['kind']=='R']
  assert p['axi_mpu']['reads']==expect
  xpu=json.loads((case/'experiment.json').read_text())['topology']['xpu']
  assert p['axi_mpu']['writes']==[r['id'] for r in p['tasks'] if r['kind']=='AW_W' and tasks[r['id']]['dst_chip']>=xpu]
  assert (case/'io-mpu-writes.tsv').read_text()==''.join(f'{i}\n' for i in p['axi_mpu']['writes'])
 else:
  assert not (case/'io-mpu.tsv').exists()
  assert p['profile'].get('mpu_service_unit')!='axi' or not any(r['kind']=='R' for r in p['tasks'])
 assert p['profile']['max_packet_payload_bytes']<=settings['payload'],'IO packet cap must fit SUE payload cap'
 return p

def analyze(run):
 """Reuse an audit only when every evidence file and this validator are byte-identical."""
 from run import sha,dump
 names=['io-die.json','io_records.csv','io_packets.csv','io_wire.csv','tasks.json',
        'task_statistics.csv','resource_statistics.csv','io_mpu_requests.csv','io_mpu_cycles.csv','io_mpu_nodes.csv']
 fingerprint=dict(validator_sha256=sha(Path(__file__)),files={n:sha(run/n) if trace_exists(run/n) else None for n in names})
 cache=run/'io-validation.json'
 if cache.exists():
  saved=json.loads(cache.read_text())
  if saved.get('fingerprint')==fingerprint:return saved['result']
 result=_analyze(run)
 dump(cache,dict(fingerprint=fingerprint,result=result,status='passed'))
 return result

def _analyze(run):
 from run import csvread
 p=json.loads((run/'io-die.json').read_text());q=sizes(p['profile']);records=csvread(run/'io_records.csv');packets=csvread(run/'io_packets.csv')
 assert len({r['record'] for r in records})==len(records)
 by={r['record']:r for r in records};seen=set();counts={};logical={}
 for f in packets:
  ids=f['records'].split(';');assert ids and (p['profile']['mode']!='none' or len(ids)==1)
  rs=[by[i] for i in ids];assert not (set(ids)&seen);seen.update(ids)
  assert len({tuple(r[k] for k in ('sc','sd','port','dc','dd','vc','kind')) for r in rs})==1
  assert int(f['unpadded_payload_bytes'])==p['profile']['packet_header_bytes']+sum(q[r['kind']] for r in rs)
  assert int(f['mac_frame_bytes'])==max(p['profile']['min_mac_frame_bytes'],int(f['unpadded_payload_bytes'])+int(f['mac_frame_bytes'])-int(f['payload_bytes']))
  assert int(f['payload_bytes'])<=p['profile']['max_packet_payload_bytes']
  for r in rs:
   assert int(r['ready_ps'])<=int(f['enqueued_ps'])<=int(r['received_ps'])
   key=(int(r['task']),r['kind']);counts[key]=counts.get(key,0)+1;logical[key]=logical.get(key,0)+int(r['logical_bytes'])
 assert seen==set(by)
 # Wire events can be millions of rows: retain only per-packet checks, not every row.
 import csv
 tx={}
 with open_trace(run/'io_wire.csv') as stream:
  for w in csv.DictReader(stream):
   i=int(w['packet'])
   if i>=0:
    assert i<len(packets)
    side=int(w['side']);assert side in (0,1)
    old=tx.get(w['packet'],(0,int(w['time_ps'])))
    tx[w['packet']]=(old[0]|(1<<side),min(old[1],int(w['time_ps'])))
 for f in packets:
  assert f['packet'] in tx
  sides,earliest=tx[f['packet']];assert sides==3
  assert int(f['enqueued_ps'])<=earliest
 tasks=json.loads((run/'tasks.json').read_text())
 for t in p['tasks']:
  key=(t['id'],t['kind']);assert counts.pop(key,0)==t['count'];assert logical[key]==tasks[t['id']]['bytes']
  if t['kind']=='AW_W':assert counts.pop((t['id'],'B'),0)==t['count']
 assert not counts
 # Pair B to its exact write transaction, and R to the corresponding AR sequence.
 identities={(int(r['task']),r['kind'],int(r['seq'])):r for r in records}
 assert len(identities)==len(records)
 for r in records:
  if r['kind']=='B':
   a=identities[int(r['task']),'AW_W',int(r['seq'])]
   assert r['dc']==a['sc'] and r['dd']==a['sd'] and r['port']==a['port'] and r['vc']==a['vc']
   assert int(r['ready_ps'])>=int(a['received_ps'])
 for t in p['tasks']:
  if t['kind']=='R':
   for seq in range(t['count']):
    a=identities[t['parent'],'AR',seq];r=identities[t['id'],'R',seq]
    assert r['port']==a['port'] and r['dc']==a['sc'] and r['dd']==a['sd']
    assert int(r['ready_ps'])>=int(a['received_ps'])
 summary=dict(records=len(records),packets=len(packets),packing_ratio=len(records)/len(packets),axi_transport_bytes=sum(int(f['payload_bytes']) for f in packets))
 # The memory audit reads its own transaction tables; free transport evidence first.
 del identities,by,seen,records,packets,tx
 import gc
 gc.collect()
 memory=analyze_memory(run)
 return dict(axi_memory=memory,**summary)

def paths(topology,physical,tasks,seed,index,rows):
 from topologies import plan,proxy,ring_distance
 out=[dict(id=t['id'],**plan(topology,physical,t,seed,index)) for t in tasks]
 for r in rows:
  if r['kind']!='R':continue
  t=tasks[r['id']];port=out[r['parent']]['port'];pd=proxy(topology,index,t['src_chip'],t['src_die'],t['dst_chip'],t['dst_die'],port)
  assert pd is not None,'read response cannot return on request port'
  out[r['id']].update(group=port//4,port=port,proxy_die=pd,hops=ring_distance(pd,t['dst_die']),reachable=True)
 return out


def memory_text(p):
 assert set(p)=={'bandwidth_bytes_s','max_inflight','latency_ns','reads','writes'}
 assert type(p['bandwidth_bytes_s']) is int and p['bandwidth_bytes_s']>0
 assert type(p['max_inflight']) is int and 1<=p['max_inflight']<=65536
 assert type(p['latency_ns']) is int and p['latency_ns']>=0
 return f"{p['bandwidth_bytes_s']} {p['max_inflight']} {p['latency_ns']}\n"+''.join(f"{r['request']} {r['service']} {r['response']} {r['count']}\n" for r in p['reads'])

def managed(case):
 path=case/'io-die.json'
 return json.loads(path.read_text()).get('axi_mpu',{}).get('reads',[]) if path.exists() else []

def managed_edges(case):
 return {(r['service'],r['request']) for r in managed(case)}|{(r['response'],r['service']) for r in managed(case)}

def analyze_memory(run):
 """Audit million-request traces using compact tuples and streaming cycle rows."""
 from run import csvread
 import csv
 from collections import defaultdict
 spec=json.loads((run/'io-die.json').read_text());p=spec.get('axi_mpu')
 if not p:return None
 quantum=1000 if json.loads((run/'experiment.json').read_text())['settings'].get('time_resolution_ns',0) else 1
 writes=set(p['writes']);ar={};replies={}
 with open_trace(run/'io_records.csv') as stream:
  for r in csv.DictReader(stream):
   task=int(r['task']);seq=int(r['seq']);kind=r['kind']
   if kind=='AR' or (kind=='AW_W' and task in writes):
    i=int(r['record']);assert i not in ar
    ar[i]=(task,seq,int(r['dc']),int(r['received_ps']))
   if kind=='R' or (kind=='B' and task in writes):
    key=(task,seq);assert key not in replies
    replies[key]=int(r['ready_ps'])
 readmap={r['request']:r for r in p['reads']};groups={};bychip=defaultdict(list);memidx={}
 # Tuple indices: chip, bytes, submitted, admitted, latency-ready, start, end.
 with open_trace(run/'io_mpu_requests.csv') as stream:
  for row in csv.DictReader(stream):
   i=int(row['id']);assert i in ar and i not in memidx
   r=tuple(int(row[k]) for k in ('chip','bytes','submitted_ps','admitted_ps','latency_ready_ps','bandwidth_start_ps','end_ps'))
   task,seq,dc,received=ar[i];read=readmap.get(task)
   assert r[1]==256 and r[0]==dc and r[2]==received<=r[3]
   assert r[4]==r[3]+p['latency_ns']*1000 and r[5]>=r[4] and r[6]>r[5]
   if read:
    reply=replies[read['response'],seq];service=read['service']
    old=groups.get(service,(r[2],r[3],r[6]))
    groups[service]=(min(old[0],r[2]),min(old[1],r[3]),max(old[2],r[6]))
   else:reply=replies[task,seq]
   assert reply>=r[6]
   memidx[i]=r;bychip[r[0]].append(r)
 assert set(memidx)==set(ar)
 del ar,replies
 peaks={}
 for chip,rows in bychip.items():
  rows.sort(key=lambda r:r[5])
  assert all(b[6]>=a[6] and b[2]>=a[2] for a,b in zip(rows,rows[1:]))
  live=peak=0
  for time,delta in sorted([(r[3],1) for r in rows]+[(r[6],-1) for r in rows]):
   live+=delta;peak=max(peak,live);assert 0<=live<=p['max_inflight']
  peaks[chip]=peak
 served=defaultdict(float);budget=defaultdict(float);first={};last={};previous={}
 with open_trace(run/'io_mpu_cycles.csv') as stream:
  for row in csv.DictReader(stream):
   i=int(row['id']);chip=int(row['chip']);start=int(row['start_ps']);end=int(row['end_ps']);n=float(row['bytes'])
   # MPU service windows remain 1ns long; their phase can be fractional ns.
   assert i in memidx and chip==memidx[i][0] and end-start==1000 and start%quantum==0 and n>0
   assert start>=memidx[i][4]
   if chip in previous and previous[chip]!=i:assert served[previous[chip]]>=256-1e-6
   previous[chip]=i
   first.setdefault(i,start);last[i]=end;served[i]+=n;budget[chip,start]+=n
 for n in budget.values():assert n<=p['bandwidth_bytes_s']/1e9+1e-6
 assert set(served)==set(memidx)
 for i,r in memidx.items():
  assert abs(served[i]-256)<1e-6 and first[i]==r[5] and last[i]==r[6]
 nodes=csvread(run/'io_mpu_nodes.csv');assert {int(r['chip']) for r in nodes}==set(peaks)
 for r in nodes:assert int(r['peak_inflight'])==peaks[int(r['chip'])] and int(r['pending'])==0
 stats={int(r['id']):r for r in csvread(run/'resource_statistics.csv')};tasks={int(r['id']):r for r in csvread(run/'task_statistics.csv')}
 for service,(submitted,admitted,end) in groups.items():
  assert int(tasks[service]['start_ps'])==submitted
  assert int(stats[service]['service_start_ps'])==admitted
  assert int(stats[service]['end_ps'])==end
 return dict(requests=len(memidx),memory_bytes=len(memidx)*256,latency_ns=p['latency_ns'],max_inflight=p['max_inflight'],peak_inflight=max(peaks.values(),default=0))
