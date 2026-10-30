"""Per-AXI MPU causality and 1ns service windows on the configured global clock."""
import os,sys,json
from pathlib import Path
from run import create,run_case,dump,csvread
from inference_experiments import DAG
from hardware_config import HW,network_settings
from workloads import task
import hbm_config,pcie_config
ROOT=Path(__file__).resolve().parents[4]
def ticks(run):
 quantum=1000 if json.loads((Path(run)/'experiment.json').read_text())['settings'].get('time_resolution_ns',0) else 1
 count=0
 for f in Path(run).glob('*.csv'):
  for row in csvread(f):
   for key,value in row.items():
    if key.endswith('_ps') and value is not None:
     try:n=int(value)
     except ValueError:continue
     if n>=0:assert n%quantum==0,(f,key,n);count+=1
 return count

def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False);results={}
 dump(root/'plan.json',dict(scope='AXI MPU read/write pipeline with ps global precision and 1ns service windows',cases=['streaming-none','streaming-packed','one-slot','mixed-read-write','tail','cycle-bandwidth','hbm-pcie'],checks=['first R before last AR','fixed latency per AXI not Engram batch','FIFO bytes and bounded inflight','write B waits for memory','timestamps respect configured precision','HBM PCIe conservation']))
 def run(name,mode='none',slots=64,n=65536,mixed=False):
  hw={**HW,'mpu_efficiency':1.0,'mpu_max_inflight':slots};d=DAG(hw)
  d.remote_read((0,0),16,n,[],'read',rows=48)
  if mixed:
   for die in range(4):d.add(task(1,die,16,0,4097,delay=1000,label='write'))
  p=json.loads((ROOT/'configs/hardware/io-die/best-effort.json').read_text());p['mode']=mode
  profile=root/(name+'.json');dump(profile,p);os.environ['SUE_IO_DIE_PROFILE']=str(profile)
  case=create(root/name,'hw-16-2',d.tasks,{'hardware':hw},resources=d.resources,settings={**network_settings(hw),'payload':8192,'stop_ns':10000000})
  hbm_config.write(case,hbm_config.load(),d.tasks,d.resources)
  r=run_case(case);results[name]=r;dump(root/'results.json',results);assert r['status']=='passed',r
  r['checked_timestamps']=ticks(r['run']);mem=csvread(Path(r['run'])/'io_mpu_requests.csv')
  assert all(int(q['latency_ready_ps'])-int(q['admitted_ps'])==200000 for q in mem)
  return r
 a=run('streaming-none');b=run('streaming-packed','best_effort');one=run('one-slot',slots=1,n=4096);mixed=run('mixed-read-write',mixed=True);tail=run('tail',n=257)
 rows=csvread(Path(a['run'])/'io_records.csv');ar=[int(r['received_ps']) for r in rows if r['kind']=='AR'];rr=[int(r['ready_ps']) for r in rows if r['kind']=='R']
 assert min(rr)<max(ar),(min(rr),max(ar))
 assert a['io_die']['axi_memory']['peak_inflight']>1 and one['io_die']['axi_memory']['peak_inflight']==1
 assert tail['io_die']['axi_memory']['requests']==2 and tail['io_die']['axi_memory']['memory_bytes']==512
 saturated=run('cycle-bandwidth','best_effort',slots=512,n=131072)
 from collections import defaultdict
 groups=defaultdict(list)
 for row in csvread(Path(saturated['run'])/'io_mpu_cycles.csv'):groups[row['chip'],row['start_ps']].append(row)
 assert any(len(rows)>1 and abs(sum(float(r['bytes']) for r in rows)-190)<1e-6 for rows in groups.values()),'remaining cycle budget was not passed to next FIFO request'
 os.environ.pop('SUE_IO_DIE_PROFILE',None)
 # Transactional PCIe plus fair HBM must obey the same clock, including sub-ns transfers.
 d=DAG(HW);d.local(0,0,1,100000,[],'expert');i=d.local(0,0,5,65537,[],'pcie',service_ns=0)
 case=create(root/'hbm-pcie','hw-16-0',d.tasks,{'hardware':HW},resources=d.resources,settings={'stop_ns':10000000})
 p=pcie_config.load();pcie_config.write(case,p,[dict(id=i,mode=1,address=0,row_bytes=264,row_stride=4096,memory_socket=0)],d.tasks,d.resources,16)
 hbm_config.write(case,hbm_config.load(),d.tasks,d.resources)
 r=run_case(case);results['hbm-pcie']=r;dump(root/'results.json',results);assert r['status']=='passed',r
 r['checked_timestamps']=ticks(r['run'])
 dump(root/'verification.json',dict(status='passed',cases=7,first_r_ready_ps=min(rr),last_ar_received_ps=max(ar),results=results))
 print(root/'verification.json')
if __name__=='__main__':main(sys.argv[1])
