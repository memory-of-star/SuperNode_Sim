"""Strict transactional PCIe profile and immutable case sidecars."""
import copy,json,math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
DEFAULT=ROOT/'configs/hardware/pcie/dual-socket-ddr5-6400.json'
MODES={'sequential':0,'gather':1,'page':2,'host_pack':3}
SCHEMA={
 'host':'chips_per_host sockets_per_host capacity_bytes channels_per_socket ddr_mt_s ddr_efficiency ddr_read_latency_ns ddr_max_inflight_per_socket ddr_line_bytes numa_link_bytes_s numa_delay_ns remote_numa packing_bytes_s',
 'dma':'engines_per_chip descriptor_queue_depth descriptor_submit_ns engine_start_ns max_inflight_bytes_per_chip tags_per_chip prefetch_requests_per_descriptor',
 'link':'topology endpoint_bytes_s socket_uplink_bytes_s hop_delay_ns packet_overhead_bytes mrrs_bytes mps_bytes rcb_bytes request_header_credits completion_header_credits completion_data_credit_bytes credit_return_ns data_credit_unit_bytes',
 'receiver':'buffer_bytes_per_chip hbm_bytes_s_per_die page_bytes page_cache_bytes_per_die',
 'access':'kv engram row_stride_bytes'}

def load(path=DEFAULT):
    return validate(json.loads(Path(path).read_text()))

def validate(p):
    p=copy.deepcopy(p)
    assert set(p)=={'schema_version','name','host','dma','link','receiver','access','notes'},'unknown/missing PCIe profile keys'
    assert p['schema_version']==1 and isinstance(p['name'],str) and isinstance(p['notes'],str)
    for section in ('host','dma','link','receiver','access'):
        assert set(p[section])==set(SCHEMA[section].split()),section
        for k,v in p[section].items():
            if k in ('topology','kv','engram'):
                assert v in (('direct','switch') if k=='topology' else MODES),(k,v)
            elif k=='remote_numa':assert type(v) is bool
            elif k=='ddr_efficiency':assert type(v) in (float,int) and 0<v<=1
            elif k=='page_cache_bytes_per_die':assert type(v) is int and v>=0
            else:assert type(v) is int and v>0,(k,'positive integer required')
    h,d,l,r=p['host'],p['dma'],p['link'],p['receiver']
    assert h['chips_per_host']==8 and h['sockets_per_host']==2,'current topology is 8 chips / dual socket'
    assert 1<=h['channels_per_socket']<=32 and 800<=h['ddr_mt_s']<=20000
    assert h['capacity_bytes']%2==0
    assert l['mrrs_bytes'] in (128,256,512,1024,2048,4096)
    assert l['mps_bytes'] in (128,256,512,1024,2048,4096)
    assert l['rcb_bytes'] in (64,128)
    assert l['data_credit_unit_bytes']==16
    assert r['page_bytes']==4096 and r['page_cache_bytes_per_die']%4096==0
    assert d['max_inflight_bytes_per_chip']>=l['mrrs_bytes']
    assert r['buffer_bytes_per_chip']>=l['mps_bytes']
    assert l['completion_data_credit_bytes']>=l['mps_bytes']
    assert p['access']['row_stride_bytes']>=264
    return p

def text(p):
    validate(p);out={}
    for section in ('host','dma','link','receiver'):
        for k,v in p[section].items():
            if k=='topology':v=int(v=='switch')
            if isinstance(v,bool):v=int(v)
            if k=='ddr_efficiency':continue
            out[k]=v
    out['ddr_channel_bytes_s']=round(p['host']['ddr_mt_s']*10**6*8*p['host']['ddr_efficiency'])
    return ''.join(f'{k} {v}\n' for k,v in sorted(out.items()))

def request_text(rows):
    return ''.join(' '.join(str(q[k]) for k in ('id','mode','address','row_bytes','row_stride','memory_socket'))+'\n' for q in rows)

def check_requests(p,rows,tasks,resources,chips):
    validate(p);rs={r['id']:r for r in resources if r['kind']==5}
    assert len(rows)==len(rs) and {q['id'] for q in rows}==set(rs),'PCIe request coverage'
    for q in rows:
        assert set(q)=={'id','mode','address','row_bytes','row_stride','memory_socket'}
        assert all(type(v) is int for v in q.values())
        t=tasks[q['id']];assert 0<=q['mode']<=3 and 0<=q['address']<2**50
        assert q['row_bytes']>0 and q['row_stride']>=q['row_bytes']
        assert q['memory_socket']//2==t['src_chip']//8,'cross-host address is unsupported'
        assert 0<=q['memory_socket']<math.ceil(chips/8)*2

def write(case,p,rows,tasks,resources,chips):
    check_requests(p,rows,tasks,resources,chips)
    (case/'pcie.json').write_text(json.dumps(p,indent=2)+'\n')
    (case/'pcie.tsv').write_text(text(p))
    (case/'pcie-requests.json').write_text(json.dumps(rows,indent=2)+'\n')
    (case/'pcie-requests.tsv').write_text(request_text(rows))

def analyze(run):
    from run import csvread
    p=load(run/'pcie.json');requests=json.loads((run/'pcie-requests.json').read_text())
    expected={q['id']:q for q in json.loads((run/'resources.json').read_text()) if q['kind']==5}
    rows=csvread(run/'pcie_descriptors.csv');assert len(rows)==len(expected)
    for r in rows:
        q=expected[int(r['id'])]
        assert int(r['useful_bytes'])==q['units']
        assert int(r['pcie_payload_bytes'])==int(r['received_bytes'])==int(r['hbm_written_bytes'])
        assert 0<int(r['begin_ps'])<=int(r['admit_ps'])<=int(r['engine_start_ps'])<=int(r['end_ps'])
        if int(r['mode'])!=2:assert int(r['pcie_payload_bytes'])==q['units']
    for r in csvread(run/'pcie_chips.csv'):
        assert int(r['peak_tags'])<=p['dma']['tags_per_chip']
        assert int(r['peak_inflight_bytes'])<=p['dma']['max_inflight_bytes_per_chip']
        assert int(r['peak_descriptors'])<=p['dma']['descriptor_queue_depth']
        assert all(int(r[k])==0 for k in ('remaining_tags','remaining_inflight_bytes','remaining_descriptors'))
    resources=csvread(run/'pcie_resources.csv')
    for r in resources:
        name=r['resource']
        if name.startswith('rx-'):assert int(r['peak_bytes'])<=p['receiver']['buffer_bytes_per_chip']
        if name.startswith(('endpoint-','uplink-')):
            completion=name.endswith('-down')
            assert int(r['peak_items'])<=p['link']['completion_header_credits' if completion else 'request_header_credits']
            assert int(r['peak_bytes'])<=(p['link']['completion_data_credit_bytes'] if completion else p['link']['request_header_credits']*32)
    total_payload=sum(int(r['pcie_payload_bytes']) for r in rows)
    completions=sum(int(r['completions']) for r in rows)
    read_requests=sum(int(r['read_requests']) for r in rows)
    def total(prefix,column='wire_bytes',suffix=''):
        return sum(int(r[column]) for r in resources if r['resource'].startswith(prefix) and r['resource'].endswith(suffix))
    assert total('endpoint-',suffix='-up')==read_requests*32,'request wire byte conservation'
    assert total('endpoint-',suffix='-down')==total_payload+completions*p['link']['packet_overhead_bytes'],'completion wire byte conservation'
    assert total('hbm-write-','bytes')==total_payload,'HBM byte conservation'
    assert sum(int(r['bytes']) for r in resources if r['resource'].startswith('ddr-') and not r['resource'].startswith('ddr-slots-'))==sum(int(r['ddr_bytes']) for r in rows)
    return dict(descriptors=len(rows),useful_bytes=sum(int(r['useful_bytes']) for r in rows),
                transferred_bytes=sum(int(r['pcie_payload_bytes']) for r in rows),ddr_bytes=sum(int(r['ddr_bytes']) for r in rows),
                read_requests=sum(int(r['read_requests']) for r in rows),completions=sum(int(r['completions']) for r in rows),
                page_hits=sum(int(r['page_hits']) for r in rows),
                tag_stall_checks=sum(int(r['tag_stall_checks']) for r in rows),
                inflight_stall_checks=sum(int(r['inflight_stall_checks']) for r in rows),
                descriptor_wait_us=sum((int(r['engine_start_ps'])-int(r['begin_ps']))/1e6 for r in rows),
                credit_wait_us=sum(int(r['admission_wait_ps'])/1e6 for r in resources if r['resource'].startswith(('endpoint-','uplink-'))))
