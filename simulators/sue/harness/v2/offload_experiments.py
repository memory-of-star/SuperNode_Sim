"""Paired KV/Engram offload DAGs: SUE MPU, chip-shared PCIe FIFO, and both.

Scope is an end-to-end representative block, not full-model TPOT. PCIe is
an explicit discrete-event bandwidth server, not a PCIe TLP/PHY simulation.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
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
        self.m = profile(definition['model'])
        self.hw = {**HW, 'mpu_efficiency': 1.0}
        self.dag = DAG(self.hw)
        self.kv = definition['target'] in ('kv', 'both')
        self.engram = definition['target'] in ('engram', 'both')
        self.bytes = Counter()
        self.requests = []

    def fraction(self, target):
        if self.d['backend'] == 'pcie':
            return 0.0
        if self.d['backend'] == 'mpu':
            return 1.0
        if self.d['backend'] == 'tiered':
            return 1.0 if target == 'engram' else 0.0
        return self.d.get('mpu_fraction', self.d['mpus'] * 190 / (self.d['chips'] * 128 + self.d['mpus'] * 190))

    def pcie_read(self, sa, n, deps, label):
        dag = self.dag
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
                done = self.pcie_read(sa, size, deps, tag)
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
        return dict(admitted=local <= original['die_limit_GB'] * 1e9 and remote <= d['mpus'] * self.hw['mpu_capacity_bytes'],
                    die_used_GB=local/1e9, die_limit_GB=original['die_limit_GB'],
                    HBM_freed_GB=(moved_kv+moved_engram)/1e9, MPU_dataset_GB=remote/1e9,
                    host_unique_dataset_GB=host/1e9,
                    host_capacity_note='Host has all requested rows locally available. Engram replicas across physical hosts, shared host DDR controllers, and host capacity are not modeled.')


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
                  settings={**SETTINGS, **network_settings(offload.hw), 'seed':d['seed'], 'timeout_s':180},
                  routes=routes, resources=dag.resources)
    save_model(case, load_model(name=d['model']))
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
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    defs = definitions()
    if args.smoke:
        defs = [definition(DS, 'both', b, m) for b,m in [('pcie',0),('mpu',1),('hybrid',8)]]
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
