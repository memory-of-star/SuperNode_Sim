"""Verify JSON validation and physical-rate changes, not only analytic arithmetic."""
import copy,json,sys
from pathlib import Path
from hardware_config import load_hardware,DEFAULT_PROFILE,network_settings
from inference_experiments import DAG,SETTINGS
from run import create,run_case,dump
from workloads import task
root=Path(sys.argv[1]).resolve();root.mkdir(parents=True,exist_ok=False)
base=json.loads(DEFAULT_PROFILE.read_text());hw,_=load_hardware();assert hw['xpu_port_bps']==200000000000 and hw['ring_direction_gbps']==2000
bad=[]
for key,value in [('dies',8),('fp4_tflops_per_die',-1),('scaleup_ports_per_die',4)]:
 d=copy.deepcopy(base);d['chip'][key]=value;p=root/(key+'.json');dump(p,d)
 try:load_hardware(p)
 except ValueError:bad.append(key)
 else:raise AssertionError(key+' accepted')
results=[]
for kind in ('xpu','mpu','ring'):
 for slow in (False,True):
  d=copy.deepcopy(base)
  if slow:
   if kind=='xpu':d['chip']['scaleup_bidirectional_bytes_s']/=4
   elif kind=='mpu':d['memory_node']['port_bits_s']/=4
   else:d['chip']['d2d_one_way_bytes_s']/=4
  p=root/f'{kind}-{slow}.json';dump(p,d);h,src=load_hardware(p)
  if kind=='ring':
   dag=DAG(h);dag.transfer((0,0),(0,1),1048576,[],'ring');ts,rs=dag.tasks,dag.resources
  else:ts=[task(0,0,1 if kind=='xpu' else 4,0,1048576,1000)];rs=[]
  c=create(root/f'{kind}-{slow}','hw-4-2',ts,{'model':'rate-probe','hardware':h,'hardware_source':src},settings={**SETTINGS,**network_settings(h)},resources=rs)
  r=run_case(c);assert r['status']=='passed',r;results.append(r)
 assert results[-1]['makespan_us']>results[-2]['makespan_us']*1.5,(kind,results[-2]['makespan_us'],results[-1]['makespan_us'])
dump(root/'verification.json',dict(status='passed',cases=6,rejected=bad,results=results));print('PASS hardware validation and six physical-link experiments')
