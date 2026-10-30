"""Check configurable XPU task notification gaps independently of physical latency."""
import sys
from pathlib import Path
from run import create,run_case,csvread,dump
from inference_experiments import DAG
from hardware_config import HW,network_settings
from workloads import task

def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
 dump(root/'plan.json',dict(cases=['default-1ns','explicit-20ns'],checks=['local dependency exact gap','receiver dependency exact gap before ACK','sender dependency exact gap','ps global precision']))
 results=[]
 for gap in (1,20):
  d=DAG(HW)
  a=d.local(0,0,0,1,[],'local-parent',service_ns=7)
  b=d.local(0,0,0,1,[a],'local-child',service_ns=1)
  net=d.add(task(1,0,2,0,256,delay=1000,label='network-parent'))
  rx=d.local(2,0,0,1,[net],'receive-child',service_ns=1)
  d.tasks[rx]['receive_deps']=[net]
  tx=d.local(1,0,0,1,[net],'ack-child',service_ns=1)
  settings=network_settings(HW)
  if gap==20:settings['visibility_ns']=20
  case=create(root/f'gap-{gap}','hw-16-0',d.tasks,{'test':'visibility'},settings=settings,resources=d.resources)
  r=run_case(case);results.append(r);dump(root/'results.json',results)
  assert r['status']=='passed',r
  stats={int(row['id']):row for row in csvread(Path(r['run'])/'task_statistics.csv')}
  for child,parent,event in [(b,a,'end_ps'),(rx,net,'data_end_ps'),(tx,net,'end_ps')]:
   assert int(stats[child]['start_ps'])-int(stats[parent][event])==gap*1000,(gap,child,stats)
  assert int(stats[rx]['start_ps'])<int(stats[net]['end_ps'])
  assert r['time_resolution_ns']==0
 dump(root/'verification.json',dict(status='passed',results=results))
 print(root/'verification.json')
if __name__=='__main__':main(sys.argv[1])
