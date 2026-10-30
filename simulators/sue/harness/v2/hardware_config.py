"""Strict hardware JSON loader. Decimal SI units; topology shape remains fixed."""
import json,os,math,hashlib
from pathlib import Path
DEFAULT_PROFILE=next((p/'configs/hardware/profiles/current.json' for p in Path(__file__).resolve().parents if (p/'configs/hardware/profiles/current.json').is_file()),None)
if DEFAULT_PROFILE is None:raise RuntimeError('cannot locate workspace configs/hardware/profiles/current.json')

def load_hardware(path=None):
 path=Path(path or os.environ.get('SUE_HARDWARE_JSON',DEFAULT_PROFILE)).resolve()
 raw=path.read_bytes();d=json.loads(raw)
 def keys(obj,expected,label):
  if not isinstance(obj,dict) or set(obj)!=set(expected.split()):raise ValueError(label+': unknown or missing fields')
 keys(d,'schema_version name chip memory_node assumptions'+(' scaleup_latency' if 'scaleup_latency' in d else ''),'root')
 latency=d.get('scaleup_latency',{})
 if latency:
  keys(latency,'xpu_port_delay_ns mpu_port_delay_ns','scaleup_latency')
  if any(type(v) is not int or not 0<=v<=1000000 for v in latency.values()):raise ValueError('port delay must be integer ns in [0,1000000]')
 elif 'scaleup_latency' in d:raise ValueError('empty scaleup_latency')
 if type(d['schema_version']) is not int or d['schema_version']!=1:raise ValueError('schema_version must be 1')
 if not isinstance(d['name'],str) or not d['name'].strip():raise ValueError('name required')
 c,m,a=d['chip'],d['memory_node'],d['assumptions']
 keys(c,'dies fp4_tflops_per_die fp8_tflops_per_die fp16_tflops_per_die memory_capacity_bytes memory_bandwidth_bytes_s d2d_one_way_bytes_s scaleup_bidirectional_bytes_s scaleup_ports_per_die pcie_bytes_s','chip')
 keys(m,'capacity_bytes memory_bandwidth_bytes_s scaleup_ports port_bits_s'+(' max_inflight_requests' if 'max_inflight_requests' in m else ''),'memory_node')
 if 'max_inflight_requests' in m and (type(m['max_inflight_requests']) is not int or not 1<=m['max_inflight_requests']<=65536):raise ValueError('max_inflight_requests must be integer 1..65536')
 keys(a,'compute_efficiency hbm_efficiency mpu_efficiency mpu_fixed_ns runtime_reserve_fraction kv_element_bytes','assumptions')
 for label,obj in [('chip',c),('memory_node',m),('assumptions',a)]:
  for k,v in obj.items():
   if type(v) not in (int,float) or not math.isfinite(v) or v<0 or (v==0 and k not in ('mpu_fixed_ns','runtime_reserve_fraction')):raise ValueError(f'{label}.{k}: invalid nonnegative numeric value')
 if c['dies']!=4 or c['scaleup_ports_per_die']!=8 or m['scaleup_ports']!=8:raise ValueError('current topology requires 4 Die/chip, 8 ports/Die and 8 ports/MPU')
 for k in ('compute_efficiency','hbm_efficiency','mpu_efficiency'):
  if a[k]>1:raise ValueError(k+' must be in (0,1]')
 if a['runtime_reserve_fraction']>=1:raise ValueError('runtime_reserve_fraction must be in [0,1)')
 if a['mpu_fixed_ns']!=int(a['mpu_fixed_ns']):raise ValueError('mpu_fixed_ns must be a whole number of nanoseconds')
 if a['kv_element_bytes']!=2:raise ValueError('current KV formulas require 2-byte elements')
 ring=c['d2d_one_way_bytes_s']/4/2*8/1e9
 port=c['scaleup_bidirectional_bytes_s']/2/(4*8)*8
 if ring!=int(ring) or ring<1:raise ValueError('D2D must resolve to a positive whole Gbit/s per directed ring edge')
 if port!=int(port) or m['port_bits_s']!=int(m['port_bits_s']):raise ValueError('port rate must be integer bit/s')
 hw=dict(dies_per_chip=4,capacity_bytes_per_die=c['memory_capacity_bytes']/4,hbm_bytes_s_per_die=c['memory_bandwidth_bytes_s']/4,
  **{f'fp{p}_flops_s_per_die':c[f'fp{p}_tflops_per_die']*1e12 for p in (4,8,16)},
  d2d_chip_send_bytes_s=c['d2d_one_way_bytes_s'],ring_direction_gbps=int(ring),scaleup_chip_send_bytes_s=c['scaleup_bidirectional_bytes_s']/2,
  pcie_chip_bytes_s=c['pcie_bytes_s'],mpu_capacity_bytes=m['capacity_bytes'],mpu_bytes_s=m['memory_bandwidth_bytes_s'],
  xpu_port_bps=int(port),mpu_port_bps=int(m['port_bits_s']),mpu_fabric_bytes_s=m['scaleup_ports']*m['port_bits_s']/8,**a)
 if 'max_inflight_requests' in m:hw['mpu_max_inflight']=m['max_inflight_requests']
 hw.update(latency)
 return hw,dict(path=str(path),sha256=hashlib.sha256(raw).hexdigest(),profile=d)

HW,HARDWARE_SOURCE=load_hardware()

def network_settings(hw):
 return dict(ring_gbps=hw['ring_direction_gbps'],xpu_port_bps=hw['xpu_port_bps'],mpu_port_bps=hw['mpu_port_bps'],**{k:hw[k] for k in ('xpu_port_delay_ns','mpu_port_delay_ns') if k in hw})


def save_hardware(root):
 """Freeze selected raw and resolved parameters; reject mixed-profile directories."""
 root=Path(root);root.mkdir(parents=True,exist_ok=True)
 profile=root/'hardware-profile.json'
 if profile.exists() and json.loads(profile.read_text())!=HARDWARE_SOURCE['profile']:
  raise ValueError('output directory already belongs to a different hardware profile')
 for name,value in [('hardware-profile.json',HARDWARE_SOURCE['profile']),('hardware-source.json',HARDWARE_SOURCE),('hardware.json',HW)]:
  (root/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n')
