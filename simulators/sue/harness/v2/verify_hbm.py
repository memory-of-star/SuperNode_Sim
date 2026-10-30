"""Analytical max-min scheduling and transactional RX backpressure probes."""
import json,sys,copy,math
from pathlib import Path
from run import create,run_case,dump,csvread
from workloads import task
import hbm_config as hc
import pcie_config as pc

def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False);results={}
 def probe(name,jobs,caps,ends,enabled=True):
  # jobs: source, bytes, arrival_ns, die
  p=dict(schema_version=1,policy='capped-max-min',bandwidth_bytes_s_per_die=100000000000,sources=[dict(id=i,name=str(i),cap_bytes_s=c) for i,c in enumerate(caps)])
  ts=[task(0,d,0,d,0,delay=a,label='test') for s,n,a,d in jobs];rs=[dict(id=i,kind=1,units=n,service_ns=math.ceil(n/100),peer_die=-1) for i,(s,n,a,d) in enumerate(jobs)]
  case=create(root/name,'hw-16-0',ts,{'window':'HBM analytic test'},resources=rs)
  if enabled:hc.write(case,p,ts,rs,[dict(id=i,source=s) for i,(s,n,a,d) in enumerate(jobs)])
  r=run_case(case);results[name]=r;dump(root/'results.json',results);assert r['status']=='passed',r
  actual=[int(x['end_ps']) for x in csvread(Path(r['run'])/'task_statistics.csv')]
  assert all(abs(a-b)<=2 for a,b in zip(actual,ends)),(name,actual,ends)
  print(name,'PASS',flush=True)
 probe('equal',[(0,100000,1000,0),(1,100000,1000,0)],[0,0],[3000000,3000000])
 probe('capped',[(0,100000,1000,0),(1,100000,1000,0)],[20000000000,0],[6000000,2250000])
 probe('three-waterfill',[(0,100000,1000,0),(1,100000,1000,0),(2,100000,1000,0)],[10000000000,20000000000,0],[11000000,6000000,2428572])
 probe('arrival',[(0,100000,1000,0),(1,100000,1500,0)],[0,0],[2500000,3000000])
 probe('release',[(0,20000,1000,0),(1,200000,1000,0)],[20000000000,0],[2000000,3200000])
 probe('packet-count-not-weight',[(0,10000,1000,0)]*10+[(1,100000,1000,0)],[0,0],[1000000+200000*(i+1) for i in range(10)]+[3000000])
 probe('independent-dies',[(0,100000,1000,0),(1,100000,1000,1)],[0,0],[2000000,2000000])
 probe('legacy-fifo',[(0,100000,1000,0),(1,100000,1000,0)],[0,0],[2000000,3000000],False)
 # Receive buffer remains occupied until fair-shared HBM staging completes.
 p=pc.load();p['receiver']['hbm_bytes_s_per_die']=100000000
 h=dict(schema_version=1,policy='capped-max-min',bandwidth_bytes_s_per_die=100000000,sources=[dict(id=0,name='expert',cap_bytes_s=0),dict(id=1,name='dma',cap_bytes_s=0)])
 ts=[task(0,0,0,0,0,delay=1000,label='expert'),task(0,0,0,0,0,delay=1000,label='dma')]
 rs=[dict(id=0,kind=1,units=100000,service_ns=1000000,peer_die=-1),dict(id=1,kind=5,units=262144,service_ns=0,peer_die=-1)]
 case=create(root/'pcie-backpressure','hw-16-0',ts,{'window':'HBM/PCIe'},resources=rs,settings={'stop_ns':10000000})
 pc.write(case,p,[dict(id=1,mode=0,address=0,row_bytes=4096,row_stride=4096,memory_socket=0)],ts,rs,16)
 hc.write(case,h,ts,rs,[dict(id=0,source=0),dict(id=1,source=1)])
 r=run_case(case);results['pcie-backpressure']=r;dump(root/'results.json',results);assert r['status']=='passed',r
 rows=csvread(Path(r['run'])/'pcie_resources.csv');rx=next(x for x in rows if x['resource']=='rx-0')
 assert int(rx['peak_bytes'])==65536 and int(rx['admission_wait_ps'])>0
 stats=csvread(Path(r['run'])/'task_statistics.csv');assert int(stats[0]['end_ps'])>1001000000 # expert slowed by concurrent writes
 for bad in [dict(h,bandwidth_bytes_s_per_die=0),dict(h,policy='fifo')]:
  try:hc.validate(bad)
  except AssertionError:pass
  else:raise AssertionError('bad profile admitted')
 dump(root/'verification.json',dict(status='passed',cases=len(results),checks=['analytical equal share','capped water filling','arrival and exit redistribution','packet count does not change source weight','per-Die independence','legacy FIFO exact timing','finite PCIe RX backpressure and expert slowdown','source/Die bandwidth and byte conservation','invalid profiles rejected']))
 print('HBM verification PASS',flush=True)
if __name__=='__main__':main(sys.argv[1])
