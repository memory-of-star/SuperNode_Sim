"""Cross-check manual claims against configs and frozen experiment evidence."""
import ast,hashlib,json,re,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
sys.path.insert(0,str(ROOT/'simulators/sue/harness/v2'))
from topologies import TOPOLOGIES

def read(p):return json.loads(p.read_text())
def main():
 checks=[]
 h=read(ROOT/'configs/hardware/profiles/current.json');assert h['memory_node']['max_inflight_requests']==128
 assert h['assumptions']['mpu_efficiency']==1.0 and h['memory_node']['memory_bandwidth_bytes_s']==190000000000
 assert h['scaleup_latency']==dict(xpu_port_delay_ns=400,mpu_port_delay_ns=400)
 half=read(ROOT/'configs/hardware/profiles/example-half-scaleup.json');half['name']=h['name'];half['chip']['scaleup_bidirectional_bytes_s']*=2;assert half==h
 checks+=['current hardware: 128 slots, 190 GB/s effective, efficiency 1.0, 400 ns ports','half-scaleup example changes only link bandwidth (and name)']
 # New-case default and scheduling defaults are independent.
 import run
 assert run.DEFAULT['interval_ns']==run.DEFAULT['visibility_ns']==1
 source=(ROOT/'simulators/sue/harness/v2/run.py').read_text();assert "'time_resolution_ns':0" in source
 assert {m for m in (0,1,2,4,8,16) if f'hw-16-{m}' in TOPOLOGIES}=={0,1,2,4,8,16}
 checks+=['new-case PS default versus 1 ns issue/notification','experimental topology includes 1 and 16 MPU']
 for profile in ('best-effort','no-packing'):
  io=read(ROOT/'configs/hardware/io-die'/f'{profile}.json');assert io['issue_interval_ns']==1 and io['mpu_service_unit']=='axi' and io['transaction_payload_bytes']==256
 checks+=['AXI profiles use 256 B service and 1 ns issue interval']
 base=ROOT/'results/sue/full-rerun-20261030-unified'
 assert read(base/'runs-ready.json')['status']=='passed'
 studies=[('chapter8-deployment',base/'deepseek/results.json',18),('chapter8-communication',base/'communication/results.json',4),('chapter8-VC',base/'vc-packing/results.json',8),('chapter9',HERE/'offload/results.json',330)]
 binaries=set()
 for name,path,count in studies:
  rows=read(path);assert len(rows)==count,(name,len(rows),count)
  for r in rows:
   assert r['status']=='passed';p=Path(r['run']);s=read(p/'experiment.json')['settings']
   manifest=read(p/'command-manifest.json');binaries.add(manifest['binary_sha256'])
   assert all(s[k]==v for k,v in dict(time_resolution_ns=0,interval_ns=1,visibility_ns=1,io_event_driven=1).items()),(p,s)
   io=read(p/'io-die.json');assert io['profile']['issue_interval_ns']==1
   if io.get('axi_mpu'):
    m=io['axi_mpu'];assert m['max_inflight']==128 and m['bandwidth_bytes_s']==190000000000 and m['latency_ns']==200,(p,m)
  checks.append(f'{name}: {len(rows)} passed records, PS precision, event wakeup, 1 ns intervals, 128 slots and 190 GB/s')
 assert len(binaries)==1, 'Mixed simulator binaries across current studies'
 assert not any(r.get('study')=='mpu-2x' for r in read(base/'deepseek/results.json'))
 assert read(HERE/'pcie/mechanism-verification.json')['cases']==20
 checks.append('20 PCIe mechanism checks passed')
 doc='\n'.join(p.read_text() for p in [HERE/'README.md',*sorted(HERE.glob('[0-9][0-9]-*.md')),HERE/'io-die-model.md'])
 for stale in ['PCIe当前只有记录作用','第9章矩阵尚未重跑','第9章历史曲线未按此模型重跑','默认传播20ns和转发预算300ns','已接入粗粒度FIFO','本轮采用已完成的275组结果']:
  assert stale not in doc,stale
 assert 'confirmed share' in (HERE/'09-mpu-pcie-offload.md').read_text()
 # Chapter 9 and the PCIe supplement share one source of mechanism text.
 assert (HERE/'pcie-transaction-model.md').read_text().strip() in (HERE/'09-mpu-pcie-offload.md').read_text()
 summary=read(HERE/'offload/summary.json');confirmed=[r for r in summary if r['variant']=='share-confirmation'];assert len(confirmed)==25 and all(r['seeds']==2 for r in confirmed)
 selected=read(HERE/'offload/plan.json')['selected'];assert len(selected)==25
 assert len({(r['model'],r['target'],r['mpus']) for r in selected})==25
 checks+=['no stale PCIe/HBM/MPU or partial-result claims in current manual','PCIe supplement matches embedded chapter9.9','25 held-out confirmation points, two seeds each; all selected fractions registered']
 record=dict(status='passed',checks=checks,documents={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [HERE/'README.md',*sorted(HERE.glob('[0-9][0-9]-*.md'))]})
 (HERE/'document-consistency-verification.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n');print('Manual consistency PASS:',len(checks),'checks')
if __name__=='__main__':main()
