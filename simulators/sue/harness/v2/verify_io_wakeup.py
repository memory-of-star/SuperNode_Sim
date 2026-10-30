"""Paired polling/event-wakeup checks; no wall-time or timing-equivalence claims inferred."""
import argparse, json, os
from pathlib import Path
import run
from inference_experiments import DAG
from hardware_config import HW, network_settings
from workloads import task

ROOT = Path(__file__).resolve().parents[4]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root', type=Path)
    ap.add_argument('--binary', type=Path)
    ap.add_argument('--source', type=Path)
    a = ap.parse_args()
    if a.binary: run.BINARY = a.binary.resolve()
    if a.source: run.SOURCE = a.source.resolve()
    root = a.root.resolve(); root.mkdir(parents=True, exist_ok=False)
    cases = ['idle', 'bounded-writes', 'mixed', 'read-pressure', 'replay']
    run.dump(root/'plan.json', dict(cases=cases, precisions_ns=[0, 1], modes=[0, 1],
        issue_ns=1, retry_ns=1, visibility_ns=1, mpu_service_ns=1,
        checks=['full byte/AXI conservation', 'FIFO MPU/128 slots', 'no stranded waiter',
                'bounded queues', 'phase-aware wakes', 'polling/event timing differences reported']))
    results = {}; comparisons = []
    for precision in (1, 0):
        for name in cases:
            hw = {**HW, 'mpu_efficiency': 1.0, 'mpu_max_inflight': 128}
            d = DAG(hw)
            if name == 'idle': d.add(task(0, 0, 1, 0, 256, delay=1000, label='idle'))
            if name in ('bounded-writes', 'mixed', 'replay'):
                for i in range(32):
                    d.add(task(0, i % 2, 1 + i % 3, i % 4, 8193,
                               delay=1000 + i % 3, label=f'write-{i}', vc=i % 2))
            if name in ('mixed', 'read-pressure'):
                for i in range(24):
                    d.remote_read((i % 16, i % 4), 16, 65537, [], f'read-{i}', rows=12)
            profile = json.loads((ROOT/'configs/hardware/io-die/best-effort.json').read_text())
            profile['max_packet_payload_bytes'] = 512
            profile['queue_records_per_port'] = 2 if name != 'idle' else 4096
            p = root/f'{precision}-{name}.json'; run.dump(p, profile)
            os.environ['SUE_IO_DIE_PROFILE'] = str(p)
            pair = []
            for mode in (0, 1):
                label = f'{precision}-{name}-{mode}'
                case = run.create(root/label, 'hw-16-2', d.tasks, {'hardware': hw},
                    resources=d.resources, settings={**network_settings(hw), 'payload': 512,
                    'queue_bytes': 544, 'credits': 16, 'time_resolution_ns': precision,
                    'io_event_driven': mode, 'interval_ns': 1, 'visibility_ns': 1,
                    'stop_ns': 10000000, 'timeout_s': 180, 'bad_every': 11 if name == 'replay' else 0})
                r = run.run_case(case); results[label] = r; run.dump(root/'results.json', results)
                assert r['status'] == 'passed', r
                path = Path(r['run']); stats = run.csvread(path/'task_statistics.csv')
                scheduler = {k: int(v) for k,v in run.csvread(path/'io_scheduler.csv')[0].items()}
                assert scheduler['pending'] == 0 and scheduler['parked'] == scheduler['resumed']
                if mode and name != 'idle': assert scheduler['parked'] > 0
                if name == 'replay': assert sum(int(x['replay_frames']) for x in run.csvread(path/'port_statistics.csv')) > 0
                pair.append((r, stats, scheduler))
                print(label, 'PASS', r['wall_s'], scheduler, flush=True)
            old, new = pair
            assert len(old[1]) == len(new[1])
            for x,y in zip(old[1],new[1]):
                for field in ('id','bytes','received_bytes','port','proxy_die','ucie_hops','status'):
                    assert x[field] == y[field], (name,field,x,y)
            delta = max(abs(int(x['end_ps'])-int(y['end_ps'])) for x,y in zip(old[1],new[1]))
            comparisons.append(dict(name=name, time_resolution_ns=precision,
                polling_wall_s=old[0]['wall_s'], event_wall_s=new[0]['wall_s'],
                polling_end_ps=max(int(x['end_ps']) for x in old[1]),
                event_end_ps=max(int(x['end_ps']) for x in new[1]),
                max_task_end_delta_ps=delta,
                polling_blocked=sum(int(x['queue_blocked_polls']) for x in old[1]),
                event_blocked=sum(int(x['queue_blocked_polls']) for x in new[1]),
                event_scheduler=new[2]))
            if name == 'idle': assert delta == 0
            run.dump(root/'comparison.json', comparisons)
    run.dump(root/'verification.json', dict(status='passed', runs=len(results), comparisons=comparisons))
    print(root/'verification.json')

if __name__ == '__main__': main()
