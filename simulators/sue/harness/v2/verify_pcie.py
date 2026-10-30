"""Mechanism tests with real ns-3 events and bounded-counter assertions."""
import copy,json,sys
from pathlib import Path
from collections import Counter
import pcie_config as pc
from run import create,run_case,dump,csvread
from workloads import task
from offload_experiments import Offload,definition,DS,MODELS
from inference_experiments import model_block

def main(root):
    root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
    p=pc.load();results={}
    dump(root/'plan.json',{'scope':'transactional PCIe mechanism validation, not model performance',
      'predictions':['finite tags/window/uplink/DDR can constrain throughput','HBM contention must backpressure receiver','page cache reduces repeated-page transfer','Host packing increases DDR traffic','legacy FIFO remains exact'], 'profile':p})
    def run(name,profile=None,requests=None,mode=0,n=262144,extra_hbm=False,sequential=False,address=0):
        profile=copy.deepcopy(profile or p)
        specs=requests or [(0,0),(0,1),(0,2),(0,3)]
        tasks=[];resources=[];req=[]
        if extra_hbm:
            tasks.append(task(0,0,0,0,0,delay=1000,label='HBM-contention'))
            resources.append(dict(id=0,kind=1,units=1,service_ns=100000,peer_die=-1))
        for j,(chip,die) in enumerate(specs):
            i=len(tasks);tasks.append(task(chip,die,chip,die,0,delay=0 if sequential and j else 1000,deps=[i-1] if sequential and j else [],label='pcie-test'))
            resources.append(dict(id=i,kind=5,service_ns=0,units=n,peer_die=-1))
            socket=chip//8*2+(chip%8)//4
            if profile['host']['remote_numa']:socket^=1
            req.append(dict(id=i,mode=mode,address=address,row_bytes=264 if mode else 4096,row_stride=4096,memory_socket=socket))
        case=create(root/name,'hw-16-0',tasks,{'window':'pcie-mechanism'},settings={'stop_ns':10000000},resources=resources)
        pc.write(case,profile,req,tasks,resources,16)
        result=run_case(case);results[name]=result;dump(root/'results.json',results)
        assert result['status']=='passed',(name,result)
        print(name,round(result['makespan_us'],4),flush=True)
        return result
    base=run('baseline')
    q=copy.deepcopy(p);q['dma']['tags_per_chip']=1
    assert run('one-tag',q)['makespan_us']>base['makespan_us']
    q=copy.deepcopy(p);q['dma']['max_inflight_bytes_per_chip']=512
    assert run('512B-inflight',q)['makespan_us']>base['makespan_us']
    q=copy.deepcopy(p);q['dma']['descriptor_queue_depth']=1
    shallow=run('one-descriptor',q)
    chips=csvread(Path(shallow['run'])/'pcie_chips.csv');assert int(chips[0]['peak_descriptors'])==1 and int(chips[0]['descriptor_stall_checks'])>0
    q=copy.deepcopy(p);q['link']['socket_uplink_bytes_s']=16000000000
    assert run('slow-uplink',q)['makespan_us']>base['makespan_us']
    shared=run('same-socket',q,requests=[(0,0),(1,0)])
    separate=run('different-sockets',q,requests=[(0,0),(4,0)])
    assert shared['makespan_us']>separate['makespan_us']*1.3
    q=copy.deepcopy(p);q['host']['ddr_efficiency']=0.01
    assert run('slow-DDR',q)['makespan_us']>base['makespan_us']
    shared_ddr=run('DDR-same-socket',q,requests=[(0,0),(1,0)])
    separate_ddr=run('DDR-different-sockets',q,requests=[(0,0),(4,0)])
    assert shared_ddr['makespan_us']>separate_ddr['makespan_us']*1.3
    q=copy.deepcopy(p);q['host']['remote_numa']=True
    remote=run('remote-NUMA',q);assert any(r['resource'].startswith('numa-') for r in csvread(Path(remote['run'])/'pcie_resources.csv'))
    q=copy.deepcopy(p);q['link']['topology']='direct'
    direct=run('direct',q);assert not any(r['resource'].startswith('uplink-') for r in csvread(Path(direct['run'])/'pcie_resources.csv'))
    r=run('HBM-backpressure',requests=[(0,0)],extra_hbm=True)
    rx=next(x for x in csvread(Path(r['run'])/'pcie_resources.csv') if x['resource']=='rx-0')
    assert int(rx['peak_bytes'])==p['receiver']['buffer_bytes_per_chip'] and int(rx['admission_wait_ps'])>0
    gather=run('random-gather',requests=[(0,0)],mode=1,n=264*64)
    packed=run('host-pack',requests=[(0,0)],mode=3,n=264*64)
    assert packed['pcie']['ddr_bytes']>gather['pcie']['ddr_bytes']*2
    page=run('page-cache',requests=[(0,0),(0,0)],mode=2,n=264*16,sequential=True)
    rows=csvread(Path(page['run'])/'pcie_descriptors.csv')
    assert int(rows[0]['pcie_payload_bytes'])==16*4096 and int(rows[1]['pcie_payload_bytes'])==0 and int(rows[1]['page_hits'])==16
    boundary=run('4K-boundary',requests=[(0,0)],n=513,address=4095)
    assert boundary['pcie']['read_requests']==2 and boundary['pcie']['completions']==3
    q=copy.deepcopy(p);q['link']['mps_bytes']=128
    small=run('128B-MPS',q,requests=[(0,0)],n=513,address=4095)
    assert small['pcie']['completions']==5
    q=copy.deepcopy(p);q['link']['completion_data_credit_bytes']=256;q['link']['completion_header_credits']=1;q['receiver']['buffer_bytes_per_chip']=256
    bounded=run('one-completion-credit',q,requests=[(0,0)],n=65536)
    assert bounded['pcie']['credit_wait_us']>0
    # Profile errors must fail before invoking ns-3.
    for section,key,value in [('dma','tags_per_chip',0),('link','mps_bytes',123),('receiver','buffer_bytes_per_chip',1),('host','ddr_efficiency',2)]:
        q=copy.deepcopy(p);q[section][key]=value
        try:pc.validate(q)
        except AssertionError:pass
        else:raise AssertionError(('accepted invalid config',section,key))
    # Exact legacy kind-4 semantics, shared across Die but independent chips.
    ts=[task(c,d,c,d,0,delay=1000) for c,d in [(0,0),(0,1),(1,0)]]
    rs=[dict(id=i,kind=4,service_ns=1000,units=128000,peer_die=-1) for i in range(3)]
    case=create(root/'legacy','hw-16-0',ts,{'window':'legacy'},resources=rs);r=run_case(case)
    assert r['status']=='passed';assert [int(x['end_ps']) for x in csvread(Path(r['run'])/'resource_statistics.csv')]==[2000000,3000000,2000000]
    results['legacy']=r
    # Payload/compute/routes invariance between old and new transport for all model workloads.
    for model,target in [(m,'kv') for m in MODELS]+[(DS,'engram'),(DS,'both')]:
        a=definition(model,target,'pcie',0);old=Offload(a);od,ro,_=model_block(a,offload=old)
        a['pcie_profile']=p;new=Offload(a);nd,rn,_=model_block(a,offload=new)
        assert ro==rn and old.bytes==new.bytes
        def compute(d):return Counter((d.tasks[q['id']]['label'],d.tasks[q['id']]['src_chip'],d.tasks[q['id']]['src_die'],q['units']) for q in d.resources if q['kind']==0)
        assert compute(od)==compute(nd)
        assert new.capacity()['admitted']
    d=definition(DS,'both','pcie',0);d['pcie_profile']=copy.deepcopy(p);d['pcie_profile']['host']['capacity_bytes']=1000000000
    assert not Offload(d).capacity()['admitted'],'Host capacity overflow must reject'
    dump(root/'results.json',results)
    dump(root/'verification.json',dict(status='passed',cases=len(results),checks=['payload and bounded queues','tag/inflight throttling','descriptor admission','shared/different socket uplinks','DDR contention','NUMA and direct paths','HBM backpressure','gather versus host packing','page amplification and cache hit','invalid inputs rejected','exact legacy FIFO','all-model workload invariance']))
if __name__=='__main__':main(sys.argv[1])
