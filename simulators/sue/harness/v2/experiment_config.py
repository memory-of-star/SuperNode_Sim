#!/usr/bin/env python3
"""One JSON entry for inference and transactional offload experiments.

References are resolved relative to the owning experiment file. Each condition
runs in a separate process so hardware globals and environment cannot leak.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BLOCKS = {'hardware', 'model', 'parallel', 'deployment', 'workload', 'profiles', 'simulation', 'offload'}
TOP = BLOCKS | {'schema_version', 'name', 'seeds', 'variants', 'execution'}
SIMULATION = {'payload', 'interval_ns', 'visibility_ns', 'stop_ns', 'queue_bytes', 'ring_bytes',
              'ring_delay_ns', 'bad_every', 'timeout_s', 'credits', 'port_sample_ns',
              'time_resolution_ns', 'io_event_driven'}


def dump(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def keys(value, allowed, label, required=()):
    if not isinstance(value, dict) or set(value) - set(allowed) or not set(required) <= set(value):
        raise ValueError(f'{label}: unknown/missing fields; allowed={sorted(allowed)}, required={sorted(required)}')


def integer(value, label, low=1, high=2147483646):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'{label}: expected integer {low}..{high}')
    return value


def merge(base, patch):
    """Recursive object overlay; arrays and scalar values replace, never append."""
    result = copy.deepcopy(base)
    for k, v in patch.items():
        result[k] = merge(result[k], v) if isinstance(v, dict) and isinstance(result.get(k), dict) else copy.deepcopy(v)
    return result


def read_json(path):
    def unique(pairs):
        d = {}
        for k, v in pairs:
            if k in d:
                raise ValueError(f'duplicate JSON key: {k}')
            d[k] = v
        return d
    return json.loads(Path(path).read_text(), object_pairs_hook=unique,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError(f'nonfinite JSON value: {x}')))


def component(value, base, sources):
    if isinstance(value, dict) and 'file' in value:
        keys(value, {'file', 'overrides'}, 'component reference', {'file'})
        if not isinstance(value['file'], str):
            raise ValueError('file must be a string')
        path = (base / value['file']).resolve()
        raw = path.read_bytes()
        sources[str(path)] = hashlib.sha256(raw).hexdigest()
        obj = read_json(path)
        patch = value.get('overrides', {})
        if not isinstance(obj, dict) or not isinstance(patch, dict):
            raise ValueError('component and overrides must be objects')
        return merge(obj, patch)
    if not isinstance(value, dict):
        raise ValueError('component must be an inline object or {"file": "..."}')
    return copy.deepcopy(value)


def expand(path):
    path = Path(path).resolve()
    spec = read_json(path)
    keys(spec, TOP, 'experiment', {'schema_version', 'name', 'hardware', 'model', 'deployment', 'workload'})
    if type(spec['schema_version']) is not int or spec['schema_version'] != 1:
        raise ValueError('experiment schema_version must be 1')
    if not isinstance(spec['name'], str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,79}', spec['name']):
        raise ValueError('experiment name must be a safe 1..80 character identifier')
    sources = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()}
    base = {k: copy.deepcopy(spec[k]) for k in BLOCKS if k in spec}
    base['hardware'] = component(base['hardware'], path.parent, sources)
    base['parallel'] = component(base.get('parallel', {}), path.parent, sources)
    keys(base['model'], {'config', 'simulation'}, 'model', {'config', 'simulation'})
    base['model'] = {k: component(v, path.parent, sources) for k, v in base['model'].items()}
    keys(base.get('profiles', {}), {'io_die', 'hbm', 'pcie'}, 'profiles')
    base['profiles'] = {k: None if v is None else component(v, path.parent, sources)
                        for k, v in base.get('profiles', {}).items()}
    execution = spec.get('execution', {})
    keys(execution, {'workers'}, 'execution')
    workers = integer(execution.get('workers', 1), 'workers', high=64)
    variants = spec.get('variants', [{'name': 'base', 'overrides': {}}])
    if not isinstance(variants, list) or not variants:
        raise ValueError('variants must be a nonempty array')
    conditions, names = [], set()
    for variant in variants:
        keys(variant, {'name', 'overrides', 'seeds'}, 'variant', {'name'})
        name = variant['name']
        if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}', name) or name in names:
            raise ValueError('variant names must be unique safe identifiers of at most 64 characters')
        names.add(name)
        overrides = variant.get('overrides', {})
        keys(overrides, BLOCKS, 'variant overrides')
        # Variants overlay resolved values. A replacement reference is unambiguous
        # only at the base component level, so reject it inside variant overlays.
        def reject_file(obj):
            if isinstance(obj, dict):
                if 'file' in obj:
                    raise ValueError('variant overrides use inline values, not file references')
                for v in obj.values(): reject_file(v)
            elif isinstance(obj, list):
                for v in obj: reject_file(v)
        reject_file(overrides)
        values = merge(base, overrides)
        seeds = variant.get('seeds', spec.get('seeds', [1]))
        if not isinstance(seeds, list) or not seeds or any(type(s) is not int for s in seeds) or len(set(seeds)) != len(seeds):
            raise ValueError('seeds must be a nonempty array of unique integers')
        for seed in seeds:
            integer(seed, 'seed')
            conditions.append(dict(schema_version=1, name=f'{name}-seed{seed}', seeds=[seed], **copy.deepcopy(values)))
    return spec, conditions, sources, workers


def prepare(config, directory):
    """Validate every effective input before executing packet simulation."""
    directory = Path(directory)
    profiles = config.get('profiles', {})
    keys(profiles, {'io_die', 'hbm', 'pcie'}, 'profiles')
    # Explicitly remove ambient configuration: JSON is the sole input authority.
    for name in ('SUE_HARDWARE_JSON', 'SUE_IO_DIE_PROFILE', 'SUE_HBM_PROFILE'):
        os.environ.pop(name, None)
    dump(directory/'hardware-profile.json', config['hardware'])
    os.environ['SUE_HARDWARE_JSON'] = str(directory/'hardware-profile.json')
    if profiles.get('io_die') is not None:
        from io_die_config import sizes
        sizes(profiles['io_die'])
        dump(directory/'io-die-profile.json', profiles['io_die'])
        os.environ['SUE_IO_DIE_PROFILE'] = str(directory/'io-die-profile.json')
    from hardware_config import HW, network_settings
    from model_config import load_model
    from inference_profile import profile, budget
    from parallel_config import resolve_parallel, layout_map
    from run import DEFAULT, check_tasks
    from inference_experiments import SETTINGS
    from topologies import TOPOLOGIES
    from workloads import task
    dump(directory/'config.json', config['model']['config'])
    dump(directory/'simulation.json', config['model']['simulation'])
    model_input = load_model(directory/'config.json', directory/'simulation.json')
    model = profile(model_input['family'], model_input)
    deployment = config['deployment']
    keys(deployment, {'chips', 'mpus'}, 'deployment', {'chips', 'mpus'})
    chips = integer(deployment['chips'], 'chips')
    mpus = integer(deployment['mpus'], 'mpus', low=0)
    topo = f'hw-{chips}-{mpus}'
    if topo not in TOPOLOGIES:
        raise ValueError('resource topology supports chips=4/8/16/32 and mpus=0/1/2/4/8/16')
    parallel = resolve_parallel(chips, config.get('parallel', {}))
    if model['width'] % parallel['tp'] or model['hidden'] % parallel['tp']:
        raise ValueError('model dimensions must divide TP without padded shards')
    w = config['workload']
    keys(w, {'batch', 'context', 'steps', 'engram_remote'}, 'workload', {'batch', 'context', 'steps'})
    for k in ('batch', 'context', 'steps'): integer(w[k], k)
    if type(w.get('engram_remote', False)) is not bool:
        raise ValueError('engram_remote must be boolean')
    if w.get('engram_remote') and (not mpus or model_input['family'] != 'deepseek-v4.1-flash-1017'):
        raise ValueError('engram_remote requires DeepSeek with MPU nodes')
    simulation = config.get('simulation', {})
    keys(simulation, SIMULATION, 'simulation')
    seed = config['seeds'][0]
    settings = {**DEFAULT, 'time_resolution_ns': 0, **SETTINGS, **network_settings(HW), **simulation, 'seed': seed}
    check_tasks(TOPOLOGIES[topo], [dict(task(0, 0, 1, 0, 256, delay=0), id=0)], settings)
    if profiles.get('io_die') and profiles['io_die']['max_packet_payload_bytes'] > settings['payload']:
        raise ValueError('IO payload cap exceeds simulation.payload')
    hbm = profiles.get('hbm')
    if hbm is not None:
        import hbm_config
        hbm_config.validate(hbm)
        if hbm['bandwidth_bytes_s_per_die'] != round(HW['hbm_bytes_s_per_die'] * HW['hbm_efficiency']):
            raise ValueError('HBM profile bandwidth must match effective hardware HBM bandwidth')
        caps = {s['name']: s['cap_bytes_s'] for s in hbm['sources']}
        if not 0 < caps.get('sue-ingress-write', 0) <= HW['scaleup_chip_send_bytes_s']/4:
            raise ValueError('SUE HBM ingress cap exceeds hardware budget or is missing')
        if not 0 < caps.get('pcie-ingress-write', 0) <= HW['pcie_chip_bytes_s']:
            raise ValueError('PCIe HBM ingress cap exceeds hardware budget or is missing')
    d = dict(study='unified-block', model=model_input['family'], model_input=model_input,
             chips=chips, mpus=mpus, tp=parallel['tp'], parallel=parallel,
             batch=w['batch'], context=w['context'], steps=w['steps'], seed=seed,
             engram_remote=w.get('engram_remote', False), include_engram=model_input['family'].startswith('deepseek'),
             moe_transport=parallel['moe_transport'], simulation_settings=simulation,
             hbm_profile=hbm, port_sample_ns=settings.get('port_sample_ns', 0))
    offload = config.get('offload')
    if offload is not None:
        keys(offload, {'target', 'backend', 'mpu_fraction', 'chunk_bytes'}, 'offload', {'target', 'backend'})
        if offload['target'] not in ('kv', 'engram', 'both') or offload['backend'] not in ('pcie', 'mpu', 'hybrid', 'tiered'):
            raise ValueError('unsupported offload target/backend')
        if offload['target'] != 'kv' and model_input['family'] != 'deepseek-v4.1-flash-1017':
            raise ValueError('Engram offload requires DeepSeek')
        if w.get('engram_remote'):
            raise ValueError('offload controls placement: do not also set workload.engram_remote')
        if offload['backend'] == 'pcie' and mpus != 0:
            raise ValueError('pure PCIe backend requires mpus=0')
        if offload['backend'] != 'pcie' and not mpus:
            raise ValueError('MPU/hybrid/tiered backend requires MPU nodes')
        if offload['backend'] == 'tiered' and offload['target'] != 'both':
            raise ValueError('tiered means Engram on MPU and KV on PCIe; requires target=both')
        fraction = offload.get('mpu_fraction')
        if offload['backend'] == 'hybrid' and fraction is None:
            raise ValueError('hybrid requires an explicit mpu_fraction; no implicit bandwidth-share policy')
        if fraction is not None and (offload['backend'] != 'hybrid' or type(fraction) not in (int, float) or not math.isfinite(fraction) or not 0 <= fraction <= 1):
            raise ValueError('mpu_fraction is a finite [0,1] value for hybrid only')
        pcie = profiles.get('pcie')
        if pcie is None:
            raise ValueError('offload requires an explicit PCIe profile (also used for MPU comparison capacity accounting)')
        import pcie_config
        pcie_config.validate(pcie)
        if pcie['receiver']['hbm_bytes_s_per_die'] != round(HW['hbm_bytes_s_per_die'] * HW['hbm_efficiency']):
            raise ValueError('PCIe receiver HBM bandwidth must match hardware')
        if pcie['link']['endpoint_bytes_s'] > HW['pcie_chip_bytes_s']:
            raise ValueError('PCIe endpoint rate exceeds chip hardware budget')
        if hbm and caps['pcie-ingress-write'] > pcie['link']['endpoint_bytes_s']:
            raise ValueError('PCIe ingress cap exceeds endpoint rate')
        chunk = integer(offload.get('chunk_bytes', 262144), 'chunk_bytes', high=1000000000)
        d.update(offload, chunk_bytes=chunk, pcie_profile=pcie, pcie_setup_ns=1000,
                 hardware_overrides={'mpu_efficiency': HW['mpu_efficiency']})
        from offload_experiments import Offload
        capacity = Offload(d).capacity()
    else:
        if profiles.get('pcie') is not None:
            raise ValueError('PCIe profile needs an offload definition; it is not silently ignored')
        capacity = budget(model, chips, parallel['tp'], w['batch'], w['context'], mpus,
                          w.get('engram_remote', False), 0, HW, parallel=parallel)
    dump(directory/'parallel.json', parallel)
    dump(directory/'parallel-layout.json', layout_map(chips, parallel, model['experts']))
    dump(directory/'definition.json', d)
    return d, dict(name=config['name'], valid=True, model=d['model'], topology=topo,
                   parallel=parallel, capacity=capacity, simulation=settings,
                   scope='representative block, not full-model TPOT/TTFT')


def worker(config_path, output, check):
    config = read_json(config_path)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    d, validation = prepare(config, output)
    dump(output/'validation.json', validation)
    if check:
        print(json.dumps(validation, ensure_ascii=False))
        return
    from inference_experiments import execute
    if config.get('offload') is not None:
        from offload_experiments import execute
    from hardware_config import save_hardware
    save_hardware(output)
    result = execute(output, d, 0)
    dump(output/'result.json', result)
    if result['status'] not in ('passed', 'capacity_rejected'):
        raise SystemExit(1)


def spawn(config, output, check=False):
    return subprocess.run([sys.executable, str(Path(__file__).resolve()), '_worker',
                           '--config', str(config), '--output', str(output)] + (['--check'] if check else []),
                          capture_output=True, text=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['check', 'run', '_worker'])
    p.add_argument('--config', type=Path, required=True)
    p.add_argument('--output', type=Path)
    p.add_argument('--check', action='store_true', help=argparse.SUPPRESS)
    a = p.parse_args()
    if a.action == '_worker':
        worker(a.config, a.output, a.check)
        return
    try:
        if a.action == 'run' and (a.output is None or a.output.exists()):
            raise ValueError('run requires a new --output directory')
        spec, conditions, sources, workers = expand(a.config)
        checks = []
        with tempfile.TemporaryDirectory(prefix='sue-config-check-') as tmp:
            for i, config in enumerate(conditions):
                path = Path(tmp)/f'{i}.json'
                dump(path, config)
                r = spawn(path, Path(tmp)/str(i), True)
                if r.returncode:
                    raise ValueError(f'{config["name"]}: validation failed\n{r.stderr}')
                check = json.loads(r.stdout)
                checks.append(check)
                # Freeze defaults as well as referenced files. The saved input is
                # accepted by this same CLI and independent of future defaults.
                config['parallel'] = check['parallel']
                config['simulation'] = {k: v for k, v in check['simulation'].items() if k in SIMULATION}
                config['workload'].setdefault('engram_remote', False)
                for key in ('io_die', 'hbm', 'pcie'):
                    config['profiles'].setdefault(key, None)
                if config.get('offload') is not None:
                    config['offload'].setdefault('chunk_bytes', 262144)
        if a.action == 'check':
            print(json.dumps(dict(status='validated', conditions=checks,
                                  note='capacity admission checked; no packet simulation executed'), ensure_ascii=False, indent=2))
            return
        from run import BINARY, SOURCE, sha
        if not BINARY.is_file():
            raise ValueError('simulator binary missing: run bin/sue build first')
        root = a.output.resolve()
        root.mkdir(parents=True, exist_ok=False)
        dump(root/'experiment.json', spec)
        dump(root/'expanded.json', conditions)
        dump(root/'sources.json', sources)
        dump(root/'validation.json', checks)
        dump(root/'implementation.json', {'binary_sha256': sha(BINARY), 'adapter_sha256': sha(SOURCE),
                                         'python': {p.name: sha(p) for p in HERE.glob('*.py')}})
        def run_one(i, config):
            dest = root/'conditions'/config['name']
            dest.mkdir(parents=True)
            snapshot = dest/'experiment.json'
            dump(snapshot, config)
            r = spawn(snapshot, dest)
            (dest/'stdout.log').write_text(r.stdout)
            (dest/'stderr.log').write_text(r.stderr)
            result = read_json(dest/'result.json') if (dest/'result.json').exists() else {'status': 'runner_error', 'error': r.stderr[-4000:]}
            if r.returncode and result.get('status') in ('passed', 'capacity_rejected'):
                result = {'status': 'runner_error', 'error': 'worker failed after result', 'returncode': r.returncode}
            if result.get('status') == 'passed' and os.environ.get('SUE_COMPRESS_TRACES') == '1':
                from trace_storage import compress_completed
                compress_completed(Path(result['run']))
            result.update(condition=config['name'], index=i)
            return result
        results = []
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(run_one, i, c): (i, c) for i, c in enumerate(conditions)}
            for f in as_completed(futures):
                try:
                    r = f.result()
                except Exception as e:
                    i, c = futures[f]
                    r = dict(index=i, condition=c['name'], status='runner_error', error=str(e))
                results.append(r)
                results.sort(key=lambda r: r['index'])
                dump(root/'results.json', results)
                print(json.dumps({k: r.get(k) for k in ('condition', 'status', 'window_us', 'error')}, ensure_ascii=False), flush=True)
        from run import csvwrite
        csvwrite(root/'summary.csv', [{k: r.get(k) for k in ('condition', 'status', 'window_us', 'per_step_block_us', 'block_us')} for r in results])
        counts = {status: sum(r['status'] == status for r in results) for status in {r['status'] for r in results}}
        dump(root/'verification.json', dict(planned=len(conditions), completed=len(results), counts=counts,
                                           scope='representative blocks; capacity rejection is not a measured performance point'))
        if any(r['status'] not in ('passed', 'capacity_rejected') for r in results):
            raise SystemExit(1)
    except (ValueError, OSError, AssertionError, KeyError, TypeError) as e:
        p.error(str(e))


if __name__ == '__main__':
    main()
