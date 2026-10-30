"""Paired KV/Engram offload DAGs: SUE MPU, chip-shared PCIe FIFO, and both.

Scope is an end-to-end representative block, not full-model TPOT. PCIe is
an explicit discrete-event bandwidth server, not a PCIe TLP/PHY simulation.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import hashlib
import pcie_config
import math
from pathlib import Path
import traceback

from hardware_config import HW, network_settings
from inference_experiments import DAG, SETTINGS, model_block
from inference_profile import profile, budget, kv_budget
from model_config import load_model, save_model
from parallel_config import resolve_parallel
from run import create, run_case, csvread, dump, sha, HERE, SOURCE, BINARY
from workloads import task

DS = 'deepseek-v4.1-flash-1017'
MODELS = ('glm-5.3', 'kimi-k3', DS)


def kda_state(m):
    if m['name'] != 'kimi-k3':
        return 0
    c = m['config'].get('text_config', m['config'])['linear_attn_config']
    return len(c['kda_layers']) * c['num_heads'] * c['head_dim'] ** 2 * 4


class Offload:
    def __init__(self, definition):
        self.d = definition
        self.m = profile(definition['model'], definition.get('model_input'))
        self.hw = {**HW, 'mpu_efficiency': 1.0, **definition.get('hardware_overrides', {})}
        self.dag = DAG(self.hw)
        self.kv = definition['target'] in ('kv', 'both')
        self.engram = definition['target'] in ('engram', 'both')
        self.bytes = Counter()
        self.requests = []
        self.pcie = pcie_config.validate(definition["pcie_profile"]) if definition.get("pcie_profile") else None
        self.pcie_requests = []
        if self.pcie:
            assert self.pcie['receiver']['hbm_bytes_s_per_die']==round(self.hw['hbm_bytes_s_per_die']*self.hw['hbm_efficiency']),'PCIe HBM staging rate must match the shared HBM profile'

    def fraction(self, target):
        if self.d['backend'] == 'pcie':
            return 0.0
        if self.d['backend'] == 'mpu':
            return 1.0
        if self.d['backend'] == 'tiered':
            return 1.0 if target == 'engram' else 0.0
        return self.d.get('mpu_fraction', self.d['mpus'] * 190 / (self.d['chips'] * 128 + self.d['mpus'] * 190))

    def pcie_read(self, sa, n, deps, label, target="kv", rows=0):
        dag = self.dag
        if self.pcie:
            mode=pcie_config.MODES[self.pcie['access'][target]]
            i=dag.local(*sa,5,n,deps,label+':pcie-transaction',service_ns=0)
            # Stable synthetic addresses across decode steps, isolated per Die/target.
            key=f'{sa[0]}:{sa[1]}:'+label.split(':',1)[-1]
            address=(int(hashlib.sha256(key.encode()).hexdigest()[:8],16)%1000000)*65536
            socket=sa[0]//8*2+(sa[0]%8)//4
            if self.pcie['host']['remote_numa']:socket^=1
            self.pcie_requests.append(dict(id=i,mode=mode,address=address,
                row_bytes=self.m['engram_row_bytes'] if rows else min(n,4096),
                row_stride=self.pcie['access']['row_stride_bytes'] if rows else 4096,memory_socket=socket))
            return [i] # Includes HBM staging internally, with shared HBM backpressure.
        latency = self.d.get('pcie_setup_ns', 0)
        if latency:
            deps = [dag.add(task(*sa, *sa, 0, delay=latency + (1000 if not deps else 0),
                                 deps=sorted(set(deps)), label=label + ':dma-setup'))]
        dma = dag.local(*sa, 4, n, deps, label + ':pcie-dma',
                        service_ns=max(1, math.ceil(n / self.hw['pcie_chip_bytes_s'] * 1e9)))
        return [dag.local(*sa, 1, n, [dma], label + ':hbm-write')]

    def read(self, sa, n, deps, label, target, rows=0):
        """Disjoint backing placement; both paths can run concurrently, then join."""
        n = math.ceil(n)
        fraction = self.fraction(target)
        chunk = self.d.get('chunk_bytes', 262144)
        batches = []
        if rows:
            # Stable synthetic row identity; whole rows assigned to one backing.
            remote_rows = round(rows * fraction)
            banks = Counter()
            for row in range(remote_rows):
                banks[(sa[0] * 4 + sa[1] + row) % self.d['mpus']] += 1
            for bank, count in sorted(banks.items()):
                # 16-row MPU random-read parallelism is retained, each bank
                # receives one batched request rather than one request per row.
                batches.append(('mpu', bank, count * self.m['engram_row_bytes'], count))
            host_rows = rows - remote_rows
            if host_rows:
                batches.append(('pcie', -1, host_rows * self.m['engram_row_bytes'], host_rows))
        else:
            remote_bytes = round(n * fraction)
            for index, offset in enumerate(range(0, remote_bytes, chunk)):
                bank = (sa[0] * 4 + sa[1] + index) % self.d['mpus']
                batches.append(('mpu', bank, min(chunk, remote_bytes - offset), 0))
            for offset in range(0, n - remote_bytes, chunk):
                batches.append(('pcie', -1, min(chunk, n - remote_bytes - offset), 0))
        assert sum(b[2] for b in batches) == n
        ready = []
        for index, (backend, bank, size, row_count) in enumerate(batches):
            tag = f'{label}:{backend}:chunk{index}'
            first = len(self.dag.tasks)
            if backend == 'mpu':
                done = self.dag.remote_read(sa, self.d['chips'] + bank, size, deps, tag, rows=row_count)
            else:
                done = self.pcie_read(sa, size, deps, tag, target, row_count)
            # Both paths stage into HBM, then the consumer reads staged data.
            consume = self.dag.local(*sa, 1, size, done, tag + ':hbm-consume')
            ready.append(consume)
            self.bytes[target + '_' + backend] += size
            self.requests.append(dict(target=target, backend=backend, chip=sa[0], die=sa[1],
                                      bank=bank, bytes=size, rows=row_count, first_task=first, consumer_task=consume))
        return ready

    def read_engram(self, sa, count, deps, label):
        rows = count * self.m['engram_rows_per_token']
        return self.read(sa, rows * self.m['engram_row_bytes'], deps, label, 'engram', rows=rows)

    def read_kv(self, sa, batch_group, tp, context, deps, label):
        _, traffic, _ = kv_budget(self.m, context)
        local_state = 2 * kda_state(self.m)
        factor = batch_group / self.m['layers'] / tp
        remote = (traffic - local_state) * factor
        return local_state * factor, self.read(sa, remote, deps, label, 'kv')

    def capacity(self):
        d, m = self.d, self.m
        original = budget(m, d['chips'], d['tp'], d['batch'], d['context'], parallel=d['parallel'])
        kv_capacity, _, _ = kv_budget(m, d['context'])
        moved_kv = (kv_capacity - kda_state(m)) * d['batch'] if self.kv else 0
        moved_engram = m['engram_bytes'] if self.engram else 0
        local = original['die_used_GB'] * 1e9 - (moved_kv + moved_engram) / (d['chips'] * 4)
        remote = moved_kv * self.fraction('kv') + moved_engram * self.fraction('engram')
        host = moved_kv + moved_engram - remote
        # Balanced static placement is a capacity budget; hot access shares
        # are enforced separately by actual per-MPU FIFO queues.
        result = dict(admitted=local <= original['die_limit_GB'] * 1e9 and remote <= d['mpus'] * self.hw['mpu_capacity_bytes'],
                    die_used_GB=local/1e9, die_limit_GB=original['die_limit_GB'],
                    HBM_freed_GB=(moved_kv+moved_engram)/1e9, MPU_dataset_GB=remote/1e9,
                    host_unique_dataset_GB=host/1e9,
                    host_capacity_note='Host has all requested rows locally available. Engram replicas across physical hosts, shared host DDR controllers, and host capacity are not modeled.')
        if self.pcie:
            sockets=math.ceil(d['chips']/8)*2
            kv_host=moved_kv*(1-self.fraction('kv'))
            engram_replica=moved_engram*(1-self.fraction('engram'))
            # KV partitions follow local ranks; local Engram lookup needs one
            # full host-backed table partition per socket (including partial hosts).
            counts=Counter(c//8*2+(c%8)//4 for c in range(d['chips']))
            used={str(socket):engram_replica+kv_host*counts[socket]/d['chips'] for socket in range(sockets)}
            if self.pcie['host']['remote_numa']:used={str(int(k)^1):v for k,v in used.items()}
            result.update(host_socket_used_bytes=used,host_physical_bytes=sum(used.values()),
                host_capacity_note='KV partitioned by XPU; host-backed Engram partition replicated per NUMA socket. Decimal 1TB/host, half per socket.')
            result['admitted']=result['admitted'] and all(v<=self.pcie['host']['capacity_bytes']/2 for v in used.values())
        return result



def definition(model, target, backend, mpus, seed=1, **overrides):
    chips = 16
    tp = 32 if model == 'kimi-k3' else 4
    return dict(study='offload-block', model=model, target=target, backend=backend,
                chips=chips, tp=tp, parallel=resolve_parallel(chips, dict(tp=tp, ep=64)),
                batch=64, context=32768, mpus=mpus, seed=seed, steps=2,
                include_engram=model == DS, hardware_overrides={'mpu_efficiency':1.0},
                chunk_bytes=262144, pcie_setup_ns=1000, **overrides)


def definitions():
    result = []
    workloads = [(m, 'kv') for m in MODELS] + [(DS, 'engram'), (DS, 'both')]
    for model, target in workloads:
        for seed in (1, 2, 3):
            result.append(definition(model, target, 'pcie', 0, seed))
        for m in (1, 2, 4, 8, 16):
            for backend in ('mpu', 'hybrid'):
                for seed in (1, 2, 3):
                    result.append(definition(model, target, backend, m, seed))
    for m in (1, 2, 4, 8, 16):
        for seed in (1, 2, 3):
            result.append(definition(DS, 'both', 'tiered', m, seed))
    # A fixed-share sensitivity avoids presenting bandwidth-proportional
    # division as an optimized scheduler. All policies are registered in advance.
    for model, target in workloads:
        for fraction in (0.125, 0.25, 0.5, 0.75):
            result.append(definition(model, target, 'hybrid', 8, 1,
                                     study_variant='share-sensitivity', mpu_fraction=fraction))
    return result


def execute(root, d, index):
    offload = Offload(d)
    capacity = offload.capacity()
    if not capacity['admitted']:
        return dict(**d, capacity=capacity, status='capacity_rejected')
    dag, routes, meta = model_block(d, offload=offload)
    meta.update(scope='end-to-end representative MoE block + amortized common/KV work; two sequential steps; Engram lookup includes both tables. NOT full-model TPOT/TTFT. KV append/writeback and full attention arithmetic excluded; Kimi KDA state stays local.',
                window='offload-resource-coupled-block', capacity=capacity,
                transport_model='MPU: SUE packets + shared memory FIFO. PCIe: chip-shared bandwidth FIFO, no TLP/PHY or host DDR model.',
                pcie_setup_ns=d['pcie_setup_ns'], mpu_fraction_kv=offload.fraction('kv'),
                mpu_fraction_engram=offload.fraction('engram'), hardware=offload.hw)
    case = create(root/'cases'/f'{index:03d}-{d["model"]}-{d["target"]}-{d["backend"]}-m{d["mpus"]}',
                  f'hw-{d["chips"]}-{d["mpus"]}', dag.tasks, meta,
                  settings={**SETTINGS, **network_settings(offload.hw), 'seed':d['seed'], 'timeout_s':d.get('timeout_s',180), **d.get('simulation_settings', {})},
                  routes=routes, resources=dag.resources)
    if offload.pcie:
        pcie_config.write(case,offload.pcie,offload.pcie_requests,dag.tasks,dag.resources,d['chips'])
        meta['transport_model']='SUE packets for MPU; transactional PCIe reads, dual-socket DDR, bounded DMA/tags/credits, streaming HBM staging. No PHY.'
        meta['pcie_setup_ns']=None
        spec=json.loads((case/'experiment.json').read_text());spec['workload']=meta;dump(case/'experiment.json',spec)
    if d.get('hbm_profile'):
        import hbm_config
        h=d['hbm_profile'];assert h['bandwidth_bytes_s_per_die']==round(offload.hw['hbm_bytes_s_per_die']*offload.hw['hbm_efficiency'])
        caps={s['name']:s['cap_bytes_s'] for s in h['sources']}
        assert 0<caps['sue-ingress-write']<=offload.hw['scaleup_chip_send_bytes_s']/4
        assert 0<caps['pcie-ingress-write']<=min(offload.hw['pcie_chip_bytes_s'],offload.pcie['link']['endpoint_bytes_s'] if offload.pcie else offload.hw['pcie_chip_bytes_s'])
        hbm_config.write(case,h,dag.tasks,dag.resources)
    save_model(case, d.get('model_input') or load_model(name=d['model']))
    dump(case/'offload-requests.json', offload.requests)
    result = run_case(case)
    result.update(d)
    result.update(capacity=capacity, offload_bytes=dict(offload.bytes), scope=meta['scope'],
                  tasks_sha256=sha(case/'tasks.json'), resources_sha256=sha(case/'resources.json'),
                  routes_sha256=sha(case/'token_routes.csv'))
    if result['status']=='passed':
        stats = csvread(Path(result['run'])/'task_statistics.csv')
        result['window_us'] = (max(int(r['end_ps']) for r in stats)-1000000)/1e6
        result['block_us'] = result['window_us']/d['steps']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--timeout-s',type=int,default=180,help='wall-clock limit per case; does not change simulated timing')
    parser.add_argument('--hbm-profile',type=Path,help='capped max-min HBM JSON; omit for historical FIFO')
    parser.add_argument('--pcie-profile',type=Path,help='transaction model JSON; omit for historical FIFO')
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    defs = definitions()
    if args.smoke:
        defs = [definition(DS, 'both', b, m) for b,m in [('pcie',0),('mpu',1),('hybrid',8)]]
    if args.pcie_profile:
        profile=pcie_config.load(args.pcie_profile)
        for d in defs:d['pcie_profile']=profile
    if args.hbm_profile:
        import hbm_config
        h=hbm_config.load(args.hbm_profile)
        for d in defs:d['hbm_profile']=h
    for d in defs:d['timeout_s']=args.timeout_s
    dump(root/'design.json', defs)
    dump(root/'plan.json', dict(
        purpose='Fixed 16 XPU; sweep shared 190 GB/s MPU count against independent 128 GB/s PCIe per chip.',
        topology='Fixed eight 256-port rails, 16 XPU and 0/1/2/4/8/16 MPU. It is the explicit experimental hw-N-M topology, not a single-switch product deployment.',
        prediction='More MPUs may reduce service contention but increase small-request overhead. Hybrid can overlap independent resources, yet short Engram queries may prefer PCIe. No guaranteed monotonic speedup.',
        hybrid='Disjoint backing partitions; proportional share = 190M/(2048+190M), independently scheduled PCIe/MPU requests followed by a join. Fixed-share scans and Engram-on-MPU/KV-on-PCIe controls included.',
        PCIe='One shared FIFO per XPU across all four Die, 128 GB/s. Independent chips overlap. Every PCIe request has 1000ns non-link-occupying setup before queue admission in all arms. No extra host-memory bottleneck.',
        KV='Read working set only, 256KiB transfer chunks; no new-token KV append or dirty-page eviction. Kimi KDA state stays local.',
        boundaries='Two representative blocks; no full-model attention QK/AV, full-layer schedule, prefill or TPOT. Host Engram locality/replication is assumed, not optimized.',
        seeds=[1,2,3], cases=len(defs)))
    if args.pcie_profile:
        plan=json.loads((root/'plan.json').read_text());plan['PCIe']='Transactional PCIe; see frozen profile. Old setup_ns is inactive.';plan['boundaries']+=' Synthetic addresses and local-socket Engram replicas; no PHY, full attention, or cross-step speculative prefetch.';dump(root/'plan.json',plan)
    dump(root/'implementation.json', dict(binary_sha256=sha(BINARY), adapter_sha256=sha(SOURCE),
         sources={p.name:sha(p) for p in HERE.glob('*.py')}))
    source = root/'source';source.mkdir()
    import shutil
    for p in [SOURCE, HERE/'offload_experiments.py', HERE/'inference_experiments.py', HERE/'run.py', HERE/'topologies.py']:
        shutil.copy2(p, source/p.name)
    results = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(execute, root, d, i):(i,d) for i,d in enumerate(defs)}
        for future in as_completed(futures):
            i,d = futures[future]
            try:
                r = future.result()
            except Exception as exc:
                r = dict(**d, status='runner_error', error=str(exc), traceback=traceback.format_exc())
            r['case_index']=i
            results.append(r)
            dump(root/'results.json', sorted(results, key=lambda x:x['case_index']))
            print(json.dumps(dict(done=len(results), total=len(defs), index=i, model=d['model'],
                                 target=d['target'], backend=d['backend'], mpus=d['mpus'],
                                 status=r['status'], block_us=r.get('block_us'), error=r.get('error'))), flush=True)
    if any(r['status']!='passed' for r in results):
        raise SystemExit(1)


if __name__=='__main__':
    main()
