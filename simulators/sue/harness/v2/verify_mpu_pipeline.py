"""Executable timing checks for MPU latency overlap and exclusive FIFO bandwidth."""
import sys
from pathlib import Path
from run import create,run_case,csvread,dump
from hardware_config import HW,network_settings
from inference_experiments import DAG
import mpu_config

def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
 dump(root/'plan.json',dict(cases=['limit1','limit2','limit4','staggered','head-blocking','independent-nodes','legacy'],rate=190000000000,expected='Latencies overlap within finite slots, byte service remains exclusive and FIFO per MPU.'))
 results={}
 def execute(name,limit,requests):
  hw={**HW,'mpu_efficiency':1.0,'mpu_max_inflight':limit};d=DAG(hw)
  for chip,arrival,latency,n in requests:
   i=d.local(chip,0,2,n,[],name,service_ns=latency+(n*10**9+190000000000-1)//190000000000);d.tasks[i]['delay_ns']=arrival
  case=create(root/name,'hw-16-2',d.tasks,{'hardware':hw},settings=network_settings(hw),resources=d.resources)
  if name=='legacy':
   (case/'mpu.json').unlink();(case/'mpu.tsv').unlink()
  r=run_case(case);assert r['status']=='passed',r
  run=Path(r['run']);results[name]=dict(run=str(run))
  if name=='legacy':return run
  rows=[{k:int(v) for k,v in r.items()} for r in csvread(run/'mpu_requests.csv')]
  results[name].update(mpu_config.analyze(run));results[name]['ends_ns']=[r['end_ps']/1000 for r in rows]
  return rows,run
 req=[(16,1000,200,1900)]*4
 for limit,ends in [(1,[1210,1420,1630,1840]),(2,[1210,1220,1420,1430]),(4,[1210,1220,1230,1240])]:
  rows,run=execute('limit'+str(limit),limit,req);assert [r['end_ps']//1000 for r in rows]==ends,rows
  if limit==1:serial=run
 rows,_=execute('staggered',4,[(16,1000,200,190000),(16,1100,200,1900)])
 assert rows[1]['bandwidth_start_ps']==rows[0]['end_ps']==2200000
 rows,_=execute('head-blocking',4,[(16,1000,400,1900),(16,1001,10,1900)])
 assert rows[1]['bandwidth_start_ps']==rows[0]['end_ps']==1410000
 rows,_=execute('independent-nodes',4,[(16,1000,200,1900),(17,1000,200,1900)])
 assert rows[0]['end_ps']==rows[1]['end_ps']==1210000
 legacy=execute('legacy',1,req)
 for file in ['task_statistics.csv','resource_statistics.csv']:
  assert (legacy/file).read_bytes()==(serial/file).read_bytes(),file
 # End-to-end SUE request/response + HBM with many simultaneous readers.
 import hbm_config
 for limit in [1,64]:
  hw={**HW,'mpu_efficiency':1.0,'mpu_max_inflight':limit};d=DAG(hw)
  for chip in range(16):d.remote_read((chip,0),16,3168,[],f'read-{chip}',rows=12)
  case=create(root/f'remote-{limit}','hw-16-2',d.tasks,{'hardware':hw},settings=network_settings(hw),resources=d.resources)
  hbm_config.write(case,hbm_config.load(),d.tasks,d.resources)
  r=run_case(case);assert r['status']=='passed',r
  run=Path(r['run']);results[f'remote-{limit}']=dict(run=str(run),**mpu_config.analyze(run))
  rows=csvread(run/'mpu_nodes.csv');results[f'remote-{limit}']['peak']=int(rows[0]['peak_inflight'])
 assert results['remote-64']['peak']>1
 for value in [0,-1,65537,1.5,True]:
  try:mpu_config.text(dict(bandwidth_bytes_s=190000000000,max_inflight=value,requests=[]))
  except AssertionError:pass
  else:raise AssertionError(value)
 dump(root/'verification.json',dict(status='passed',cases=results,legacy_byte_identical=True,invalid_limits_rejected=5))
 print(root/'verification.json')
if __name__=='__main__':main(sys.argv[1])
