"""Validate and summarize VC/AXI/frame/credit and switch egress evidence."""
import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path
from run import dump, csvread, csvwrite, sha
from trace_storage import open_trace
from io_die_config import sizes
from vc_packet_study import business

BUSINESSES=('MoE','KV','Engram')
KINDS=('AW_W','B','AR','R')
BIN=500000  # ps; fixed full bins, including idle bins in average


def rows(path):
    with open_trace(path) as f:
        yield from csv.DictReader(f)


def quantile(hist, q):
    threshold=max(1,math.ceil(sum(hist.values())*q));n=0
    for size,count in sorted(hist.items()):
        n+=count
        if n>=threshold:return size
    return 0


def describe(hist):
    total=sum(hist.values())
    return dict(min=min(hist,default=0),mean=sum(k*v for k,v in hist.items())/total if total else 0,
                p50=quantile(hist,.5),p95=quantile(hist,.95),max=max(hist,default=0))


def extract(result, dest):
    run=Path(result['run']);tasks=json.loads((run/'tasks.json').read_text())
    profile=json.loads((run/'io-die.json').read_text())['profile'];record_sizes=sizes(profile)
    settings=json.loads((run/'experiment.json').read_text())['settings']
    resolution=settings.get('time_resolution_ns',0)
    duration=int(json.loads((run/'engine.json').read_text())['sim_end_ps']);seconds=duration/1e12
    stats=csvread(run/'task_statistics.csv')
    assert result['status']=='passed' and all(r['status']=='complete' and r['bytes']==r['received_bytes'] for r in stats)
    assert result['replay_frames']==result['discard']==0
    # Record IDs and packet IDs are contiguous; retain compact tuples only.
    records=[];record_counts=Counter();logical=Counter();phase_counts=Counter()
    for r in rows(run/'io_records.csv'):
        tid=int(r['task']);b=business(tasks[tid]);vc=int(r['vc']);kind=r['kind']
        assert vc==BUSINESSES.index(b) and int(r['record'])==len(records)
        assert vc==tasks[tid]['vc'] and int(r['received_ps'])>=int(r['ready_ps'])
        records.append((b,kind,vc));record_counts[b,kind]+=1
        if kind in ('AW_W','R'):logical[b]+=int(r['logical_bytes'])
        phase=tasks[tid]['label'].split(':')[1];phase_counts[phase,kind]+=1
    packets=[];packet_counts=Counter();wire_bytes=Counter();credit_units=Counter()
    mac_hists=defaultdict(Counter);pack_hists=defaultdict(Counter);record_seen=bytearray(len(records))
    for r in rows(run/'io_packets.csv'):
        ids=[int(i) for i in r['records'].split(';')];key=records[ids[0]]
        assert all(records[i]==key for i in ids),'cross-business/VC/kind packing'
        for i in ids:assert not record_seen[i];record_seen[i]=1
        b,kind,vc=key;mac=int(r['mac_frame_bytes']);credit=(mac+255)//256
        assert int(r['packet'])==len(packets)
        assert int(r['unpadded_payload_bytes'])==profile['packet_header_bytes']+len(ids)*record_sizes[kind]
        assert mac==max(profile['min_mac_frame_bytes'],17+int(r['unpadded_payload_bytes']))
        if result['mode']=='none':assert len(ids)==1
        packets.append((b,kind,vc,mac));packet_counts[b,kind]+=1;wire_bytes[b]+=mac+8
        credit_units[b]+=credit;mac_hists[b,kind][mac]+=1;pack_hists[b,kind][len(ids)]+=1
    assert all(record_seen)
    del record_seen,records
    wire_seen=[bytearray(len(packets)),bytearray(len(packets))]
    switch=defaultdict(Counter);port=defaultdict(Counter);time=defaultdict(Counter)
    switch_bins=Counter();port_bins=Counter();control_sizes=Counter();actual_ports=defaultdict(Counter)
    line_times=Counter();previous_data={};short_gaps=defaultdict(Counter)
    for r in rows(run/'io_wire.csv'):
        side=int(r['side']);i=int(r['packet']);n=int(r['wire_bytes']);sw=int(r['switch'])
        key=(int(r['chip']),int(r['die']),int(r['port']),side)
        busy=int(r['tx_busy_ps']);bps=settings['mpu_port_bps' if key[0]>=16 else 'xpu_port_bps']
        exact=n*8*10**12//bps
        assert busy==(exact//1000*1000 if resolution else exact),'incorrect serialization duration'
        actual_ports[key]['frames']+=1;actual_ports[key]['bytes']+=n
        if i>=0:
            assert n==packets[i][3]+8 and not wire_seen[side][i]
            wire_seen[side][i]=1
            if key in previous_data:
                old_time,old_size,old_busy=previous_data[key]
                if old_size==72:
                    gap=int(r['time_ps'])-old_time-old_busy
                    assert gap>=0,'overlapping physical transmissions'
                    short_gaps[side][gap]+=1
            previous_data[key]=(int(r['time_ps']),n,busy)
        if side!=1:continue
        line_times[n,busy,exact]+=1
        pk=(sw,int(r['physical_port']));t=int(r['time_ps']);binid=t//BIN
        switch[sw]['all_events']+=1;port[pk]['all_events']+=1
        if i<0:
            control_sizes[n]+=1;switch[sw]['control_events']+=1
            continue
        b,kind,vc,mac=packets[i]
        switch[sw]['data_frames']+=1;switch[sw]['data_bytes']+=n;port[pk]['data_frames']+=1
        if (binid+1)*BIN<=duration:
            time[binid][b]+=1;switch_bins[sw,binid]+=1;port_bins[sw,pk[1],binid]+=1
    assert all(wire_seen[0]) and all(wire_seen[1])
    for r in rows(run/'port_statistics.csv'):
        k=(int(r['chip']),int(r['die']),int(r['port']),int(r['side']))
        assert actual_ports[k]['frames']==int(r['frames']) and actual_ports[k]['bytes']==int(r['wire_bytes'])
    assert sum(s['data_frames'] for s in switch.values())==len(packets)
    # Passive sparse samples: omitted rows are zero. No claim of exact event peaks.
    vc_port=defaultdict(lambda:dict(peak_credit=0,peak_queue_bytes=0,credit_exhausted=0,credit_samples=0,nonzero_samples=0))
    for r in rows(run/'vc_timeseries.csv'):
        key=tuple(int(r[k]) for k in ('switch','physical_port','side','chip','die','port','vc'))
        x=vc_port[key];cu=int(r['credit_used']);limit=int(r['credit_limit'])
        assert 0<=cu<=limit==512 and key[-1] in (0,1,2)
        x['peak_credit']=max(x['peak_credit'],cu)
        x['peak_queue_bytes']=max(x['peak_queue_bytes'],int(r['egress_bytes']))
        x['credit_exhausted']=max(x['credit_exhausted'],int(r['credit_exhausted']))
        x['credit_samples']+=cu;x['nonzero_samples']+=1
    fields=('switch','physical_port','side','chip','die','port','vc')
    vcrows=[dict(zip(fields,k),**v,business=BUSINESSES[k[-1]],role=('switch-to-' if k[2] else '')+('MPU' if k[3]>=16 else 'XPU')+('' if k[2] else '-to-switch')) for k,v in sorted(vc_port.items())]
    assert sum(v['credit_exhausted'] for v in vc_port.values())==result['credit_exhausted_checks']
    vc_summary=[]
    for vc,b in enumerate(BUSINESSES):
        sub=[v for v in vcrows if v['vc']==vc];sw=[v for v in sub if v['side']==1]
        vc_summary.append(dict(business=b,vc=vc,logical_data_bytes=logical[b],wire_bytes=wire_bytes[b],
            frames=sum(packet_counts[b,k] for k in KINDS),records=sum(record_counts[b,k] for k in KINDS),
            consumed_credit_units=credit_units[b],peak_credit=max(v['peak_credit'] for v in sub),
            peak_switch_credit=max(v['peak_credit'] for v in sw),
            peak_queue_bytes=max(v['peak_queue_bytes'] for v in sub),
            credit_exhausted=sum(v['credit_exhausted'] for v in sub),
            max_frame_credit=max((mac+255)//256 for (bb,k),hist in mac_hists.items() if bb==b for mac in hist)))
    details=[];histrows=[]
    for (b,kind),hist in sorted(mac_hists.items()):
        details.append(dict(business=b,vc=BUSINESSES.index(b),kind=kind,axi_record_bytes=record_sizes[kind],
            records=record_counts[b,kind],frames=packet_counts[b,kind],
            mean_records_per_frame=record_counts[b,kind]/packet_counts[b,kind],
            **{'mac_'+k:v for k,v in describe(hist).items()}))
        for mac,count in sorted(hist.items()):histrows.append(dict(business=b,kind=kind,mac_bytes=mac,wire_bytes=mac+8,credits=(mac+255)//256,frames=count))
    switchrows=[]
    for sw,s in sorted(switch.items()):
        switchrows.append(dict(switch=sw,**s,data_mpps=s['data_frames']/seconds/1e6,
            all_mevents_s=s['all_events']/seconds/1e6,
            peak_500ns_mpps=max((v for (ss,b),v in switch_bins.items() if ss==sw),default=0)*2,
            peak_port_500ns_mpps=max((v for (ss,p,b),v in port_bins.items() if ss==sw),default=0)*2))
    time_rows=[dict(time_us=b*BIN/1e6,**{k:time[b][k]*2 for k in BUSINESSES}) for b in range(duration//BIN)]
    out=dest/result['arm'];out.mkdir(parents=True,exist_ok=True)
    linerows=[dict(wire_bytes=n,modeled_ps=t,exact_ps=exact,events=count) for (n,t,exact),count in sorted(line_times.items())]
    for name,data in [('vc-ports',vcrows),('vc-summary',vc_summary),('packet-details',details),('packet-lengths',histrows),('switch-rates',switchrows),('pps-timeseries',time_rows),('wire-times',linerows)]:csvwrite(out/(name+'.csv'),data)
    timing=dict(time_resolution_ns=resolution,global_quantum_ps=1000 if resolution else 1,
        wire_times=linerows,configured_ifg_bytes=12,exact_ifg_ps=480,
        short_frame_gaps=[dict(side=side,count=sum(hist.values()),min_ps=min(hist),below_480ps=sum(count for gap,count in hist.items() if gap<480)) for side,hist in sorted(short_gaps.items())],
        known_issue='Idle-to-nonempty queue admission may bypass a pending IFG timer; ps precision alone does not enforce all inter-frame gaps.')
    dump(out/'line-time-audit.json',timing)
    total_frames=len(packets)
    metric=dict(arm=result['arm'],seed=result['seed'],mode=result['mode'],run=str(run),time_resolution_ns=resolution,
        sim_end_us=duration/1e6,window_us=result['makespan_us'],data_frames=total_frames,
        wire_bytes=sum(wire_bytes.values()),data_mpps=total_frames/seconds/1e6,
        aggregate_peak_500ns_mpps=max((sum(v.values())*2 for v in time.values()),default=0),
        all_events=sum(s['all_events'] for s in switch.values()),
        control_sizes=dict(control_sizes),vc=vc_summary,packets=details,switches=switchrows,
        phase_records=[dict(phase=p,kind=k,records=n) for (p,k),n in sorted(phase_counts.items())],
        credit_exhausted=result['credit_exhausted_checks'],replays=result['replay_frames'],discard=result['discard'])
    dump(out/'metrics.json',metric)
    dump(out/'evidence.json',dict(run=str(run),files={n:sha(run/n) for n in ('tasks.json','resources.json','sue.conf','io-die.json','command-manifest.json','io_records.csv','io_packets.csv','io_wire.csv','vc_timeseries.csv','port_statistics.csv','summary.json')}))
    return metric


def main():
    p=argparse.ArgumentParser();p.add_argument('study',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--compare-analysis',type=Path);a=p.parse_args()
    dest=a.output;dest.mkdir(parents=True,exist_ok=True)
    results=json.loads((a.study/'results.json').read_text())
    metrics=[]
    for r in results:
        if r['sample_ns']:
            metrics.append(extract(r,dest));print('Analyzed '+r['arm'],flush=True)
    assert len(metrics)==6
    for seed in (1,2,3):
        pair=[m for m in metrics if m['seed']==seed]
        assert len(pair)==2 and pair[0]['phase_records']==pair[1]['phase_records']
        assert [(v['business'],v['logical_data_bytes'],v['records']) for v in pair[0]['vc']]==[(v['business'],v['logical_data_bytes'],v['records']) for v in pair[1]['vc']]
    dump(dest/'metrics.json',metrics)
    dump(dest/'line-time-audit.json',dict(cases=[dict(arm=m['arm'],**json.loads((dest/m['arm']/'line-time-audit.json').read_text())) for m in metrics]))
    if a.compare_analysis:
        before={(m['seed'],m['mode']):m for m in json.loads((a.compare_analysis/'metrics.json').read_text())}
        compared=[]
        for m in metrics:
            old=before[m['seed'],m['mode']]
            assert old['phase_records']==m['phase_records'],'transaction work changed'
            assert [(v['business'],v['logical_data_bytes']) for v in old['vc']]==[(v['business'],v['logical_data_bytes']) for v in m['vc']]
            row=dict(seed=m['seed'],mode=m['mode'])
            for key in ('window_us','data_frames','data_mpps','aggregate_peak_500ns_mpps','credit_exhausted'):
                row['old_'+key]=old[key];row['new_'+key]=m[key]
            compared.append(row)
        dump(dest/'precision-comparison.json',compared);csvwrite(dest/'precision-comparison.csv',compared)
    csvwrite(dest/'summary.csv',[{k:v for k,v in m.items() if not isinstance(v,(list,dict))} for m in metrics])
    dump(dest/'verification.json',dict(status='passed',cases=6,checks=['all task bytes delivered','all records delivered exactly once','packets never cross VC/business/AXI kind','every packet observed once on each hop','per-port wire counts and bytes conserved','paired AXI counts and useful data identical','sparse per-VC credit counters match final aggregate','credit CU within configured window','full fixed 500ns bins only']))

if __name__=='__main__':main()
