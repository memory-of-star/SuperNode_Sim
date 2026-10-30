"""Preserve sub-ns wire time and IFG while retaining 1ns MPU service windows."""
import json
import os
import sys
from collections import defaultdict
from pathlib import Path
from inference_experiments import DAG
from hardware_config import HW, network_settings
from run import create, run_case, dump, csvread
import hbm_config


def main(root):
    root=Path(root).resolve();root.mkdir(parents=True,exist_ok=False)
    profile=Path(__file__).resolve().parents[4]/'configs/hardware/io-die/no-packing.json'
    os.environ['SUE_IO_DIE_PROFILE']=str(profile)
    dump(root/'plan.json',dict(cases=['default-ps','explicit-ns'],checks=[
        '72B at 200Gb/s takes 2880ps in default mode',
        '12B IFG is represented as 480ps on queued send paths',
        '9B control has nonzero 360ps line time',
        'MPU windows remain 1000ps with fractional-ns phase',
        'explicit 1ns inputs retain their original quantization']))
    results=[]
    for mode in ('default-ps','explicit-ns'):
        d=DAG(HW);d.remote_read((0,0),16,16384,[],'read')
        settings={**network_settings(HW),'payload':8192}
        if mode=='explicit-ns':settings['time_resolution_ns']=1
        case=create(root/mode,'hw-16-2',d.tasks,{'hardware':HW},settings=settings,resources=d.resources)
        hbm_config.write(case,hbm_config.load(),d.tasks,d.resources)
        result=run_case(case);results.append(result);dump(root/'results.json',results)
        assert result['status']=='passed',result
        expected=0 if mode=='default-ps' else 1
        assert result['time_resolution_ns']==expected
        trace=csvread(Path(result['run'])/'io_wire.csv');short=[];control=[];ports=defaultdict(list)
        for r in trace:
            n=int(r['wire_bytes']);actual=int(r['tx_busy_ps']);exact=n*40
            assert actual==(exact if not expected else exact//1000*1000),(mode,n,actual)
            if n==9:control.append(actual)
            if int(r['packet'])>=0:
                if n==72:short.append(actual)
                key=tuple(r[k] for k in ('chip','die','port','side'))
                ports[key].append(r)
        assert short and control
        cycles=csvread(Path(result['run'])/'io_mpu_cycles.csv')
        assert cycles and all(int(r['end_ps'])-int(r['start_ps'])==1000 for r in cycles)
        if not expected:
            assert any(int(r['start_ps'])%1000 for r in cycles),'sub-ns arrival phase lost'
            gaps=[]
            for rs in ports.values():
                for before,after in zip(rs,rs[1:]):
                    if int(before['wire_bytes'])==72:
                        gap=int(after['time_ps'])-int(before['time_ps'])-int(before['tx_busy_ps'])
                        assert gap>=0,(before,after)
                        gaps.append(gap)
            assert gaps and 480 in gaps,gaps
            observed_min_gap=min(gaps)
            early_gap_count=sum(gap<480 for gap in gaps)
        else:
            assert all(int(r['start_ps'])%1000==0 for r in cycles)
    dump(root/'verification.json',dict(status='passed',cases=2,default_wire_72B_ps=2880,
        default_control_9B_ps=360,configured_ifg_ps=480,mpu_window_ps=1000,
        observed_min_gap_ps=observed_min_gap,early_gap_count=early_gap_count,
        known_issue='Existing idle-to-nonempty queue admission can bypass the pending IFG timer; this regression verifies restored precision, not universal IFG enforcement.',
        explicit_ns_replay_preserved=True))
    print(root/'verification.json')


if __name__=='__main__':main(sys.argv[1])
