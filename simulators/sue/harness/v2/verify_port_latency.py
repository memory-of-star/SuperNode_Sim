"""Check independent XPU/MPU access delays without changing packet serialization."""
import sys,json,tempfile,copy
from pathlib import Path
from run import create,run_case,csvread,dump,DEFAULT
from workloads import task
from hardware_config import HW,network_settings,load_hardware,HARDWARE_SOURCE
from inference_experiments import DAG,SETTINGS
import hbm_config

def main(root):
 root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
 dump(root/'plan.json',{'routes':['XPU-MPU','MPU-XPU','XPU-XPU'],'port_delays':[[0,0],[400,0],[0,400],[400,400]],'expected':'Each traversed access link adds its configured delay exactly once per direction; ACK doubles one-way increment; no 20ns double count.'})
 out=[]
 for name,src,dst in [('XPU-MPU',0,16),('MPU-XPU',16,0),('XPU-XPU',0,1)]:
  base=None
  for x,m in ((0,0),(400,0),(0,400),(400,400)):
   settings={**network_settings(HW),'xpu_port_delay_ns':x,'mpu_port_delay_ns':m}
   case=create(root/f'{name}-{x}-{m}','hw-16-1',[task(src,0,dst,0,64,delay=1000)],{'test':'port-latency'},settings=settings)
   r=run_case(case);assert r['status']=='passed',r
   s=csvread(Path(r['run'])/'task_statistics.csv')[0]
   one=(int(s['data_end_ps'])-int(s['start_ps']))/1000;roundtrip=(int(s['end_ps'])-int(s['start_ps']))/1000
   if base is None:base=(one,roundtrip)
   delta=2*x if name=='XPU-XPU' else x+m
   assert abs(one-base[0]-delta)<1e-6
   assert abs(roundtrip-base[1]-2*delta)<1e-6
   assert 'SUE_LINK_PROPAGATION_NS 0\n' in (case/'sue.conf').read_text()
   out.append(dict(route=name,xpu_ns=x,mpu_ns=m,one_way_ns=one,roundtrip_ns=roundtrip,run=r['run']))
 d=DAG({**HW,'mpu_efficiency':1.0});d.remote_read((0,0),16,3168,[],'read',rows=12)
 case=create(root/'read-3168','hw-16-1',d.tasks,{'test':'read-with-overlapped-acks'},settings={**SETTINGS,**network_settings(HW)},resources=d.resources)
 hbm_config.write(case,hbm_config.load(),d.tasks,d.resources);r=run_case(case);assert r['status']=='passed',r
 s=csvread(Path(r['run'])/'task_statistics.csv')
 assert int(s[1]['start_ps'])<int(s[0]['end_ps']) and int(s[3]['start_ps'])<int(s[2]['end_ps'])
 hbm_us=(int(s[3]['end_ps'])-int(s[0]['start_ps']))/1e6
 assert abs(hbm_us-2.62796)<1e-6,hbm_us
 # Missing fields, negative and non-integer delays cannot silently pass.
 for invalid in ({},{'xpu_port_delay_ns':400},{'xpu_port_delay_ns':-1,'mpu_port_delay_ns':400},{'xpu_port_delay_ns':1.5,'mpu_port_delay_ns':400}):
  profile=copy.deepcopy(HARDWARE_SOURCE['profile']);profile['scaleup_latency']=invalid
  file=root/'invalid-profile.json';dump(file,profile)
  try:load_hardware(file)
  except ValueError:pass
  else:raise AssertionError('invalid profile accepted')
 dump(root/'verification.json',dict(status='passed',network_cases=out,read_hbm_ready_us=hbm_us,read_run=r['run'],invalid_profiles=4))
 print(json.dumps({'status':'passed','network_cases':out,'read_hbm_ready_us':hbm_us},indent=2))
if __name__=='__main__':main(sys.argv[1])
