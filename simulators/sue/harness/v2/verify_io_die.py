"""IO Die integration checks: actual SUE packets, FIFO packing, AXI response conservation."""
import sys,os,json,copy
from pathlib import Path
from run import create,run_case,dump,csvread
from workloads import task
from hardware_config import HW,network_settings
from inference_experiments import DAG
from io_die_config import sizes
ROOT=Path(__file__).resolve().parents[4]
def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
 dump(root/'plan.json',dict(modes=['none','best_effort'],cases=['writes','reads','mixed','proxy','bounded','idle'],checks=['fixed 256B data records','AW+W atomic','homogeneous port/destination/VC/type','logical conservation','R reverse request port','B per write','finite queues','no timer for idle packet']))
 results={}
 for mode in ('none','best_effort'):
  for name in ('writes','reads','mixed','proxy','bounded','idle'):
   hw={**HW,'mpu_efficiency':1.0};d=DAG(hw);topo='hw-16-2'
   if name in ('writes','mixed','bounded'):
    for i in range(24):d.add(task(0,i%2,1+i%3,i%4,4097,delay=1000,label='write',vc=i%2))
   if name in ('reads','mixed'):
    for i in range(16):d.remote_read((i,0),16+i%2,3168,[],f'read-{i}',rows=12)
   if name=='proxy':
    topo='xpu128-parity'
    d.add(task(0,0,1,1,4097,delay=1000,label='proxy-write'))
   if name=='idle':d.add(task(0,0,1,0,256,delay=1000,label='idle'))
   p=json.loads((ROOT/'configs/hardware/io-die/best-effort.json').read_text());p['mode']=mode
   if name=='bounded':p['queue_records_per_port']=2
   profile=root/f'{mode}-{name}.json';dump(profile,p);os.environ['SUE_IO_DIE_PROFILE']=str(profile)
   case=create(root/f'{mode}-{name}',topo,d.tasks,{'hardware':hw},settings={**network_settings(hw),'payload':8192,'stop_ns':10000000},resources=d.resources)
   result=run_case(case);results[f'{mode}-{name}']=result;dump(root/'results.json',results)
   assert result['status']=='passed',result
   run=Path(result['run']);packets=csvread(run/'io_packets.csv');records=csvread(run/'io_records.csv')
   if name=='idle':assert int(packets[0]['enqueued_ps'])==1000000
   if name=='reads':
    stats={int(r['id']):r for r in csvread(run/'task_statistics.csv')}
    for r in json.loads((case/'io-die.json').read_text())['tasks']:
     if r['kind']=='R':assert stats[r['id']]['port']==stats[r['parent']]['port']
 for name in ('writes','reads','mixed'):
  assert results['best_effort-'+name]['io_die']['packets']<results['none-'+name]['io_die']['packets'],name
 for bad in ('mode','transaction_payload_bytes','bus_data_bits','max_packet_payload_bytes'):
  p=json.loads((ROOT/'configs/hardware/io-die/best-effort.json').read_text());p[bad]=0
  try:sizes(p)
  except AssertionError:pass
  else:raise AssertionError(bad)
 dump(root/'verification.json',dict(status='passed',cases=12,results=results));print(root/'verification.json')
if __name__=='__main__':main(sys.argv[1])
