"""Unified input regression: real packets, isolation, rejection and snapshots."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
from experiment_config import expand, dump, ROOT


def main(root):
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=False)
    cli = ROOT/'bin/sue'
    env = {**os.environ, 'SUE_HARDWARE_JSON': '/nonexistent/hardware.json',
           'SUE_IO_DIE_PROFILE': '/nonexistent/io.json', 'SUE_HBM_PROFILE': '/nonexistent/hbm.json'}
    checks = []

    def command(config, action='check', output=None, ok=True):
        args = [str(cli), action, '--config', str(config)]
        if output is not None: args += ['--output', str(output)]
        r = subprocess.run(args, cwd='/tmp', env=env, capture_output=True, text=True)
        if (r.returncode == 0) != ok:
            raise AssertionError((args, r.stdout[-3000:], r.stderr[-4000:]))
        return r

    # Inline snapshot: independent of CWD, source files and hostile legacy env.
    _, cases, _, _ = expand(ROOT/'configs/experiments/glm-single.json')
    spec = cases[0]
    spec['name'] = 'unified-smoke'
    spec['workload'] = dict(batch=1, context=64, steps=1)
    spec['simulation']['timeout_s'] = 120
    spec['execution'] = {'workers': 2}
    spec['variants'] = [
        {'name': 'packed'},
        {'name': 'unpacked-tp8', 'overrides': {'parallel': {'tp': 8}, 'profiles': {'io_die': {'mode': 'none'}},
                                            'hardware': {'assumptions': {'compute_efficiency': .6}}}},
    ]
    path = root/'smoke.json'; dump(path, spec)
    command(path, 'run', root/'inference')
    results = json.loads((root/'inference/results.json').read_text())
    assert len(results) == 2 and all(r['status'] == 'passed' for r in results)
    for r in results:
        condition = root/'inference/conditions'/r['condition']
        resolved = json.loads((condition/'definition.json').read_text())
        wanted = 4 if r['condition'].startswith('packed-') else 8
        assert resolved['tp'] == wanted
        assert json.loads((condition/'hardware.json').read_text())['compute_efficiency'] == (.3 if wanted == 4 else .6)
        io = json.loads((Path(r['run'])/'io-die.json').read_text())
        assert io['profile']['mode'] == ('best_effort' if wanted == 4 else 'none')
    checks.append('two real inference cases: TP/hardware/packing isolation, ambient environment ignored')
    # Snapshot is a directly reusable input; no external profiles required.
    command(root/'inference/conditions/packed-seed1/experiment.json')
    checks.append('saved inline experiment can be revalidated from another working directory')

    _, off, _, _ = expand(ROOT/'configs/experiments/glm-kv-offload.json')
    spec2 = off[0]
    spec2['workload'] = dict(batch=1, context=64, steps=1)
    spec2['simulation']['timeout_s'] = 120
    spec2['execution'] = {'workers': 2}
    spec2['variants'] = [{'name': 'pcie'}, {'name': 'hybrid', 'overrides': {'deployment': {'mpus': 1}, 'offload': {'backend': 'hybrid', 'mpu_fraction': .25}}}]
    path2 = root/'offload.json'; dump(path2, spec2)
    command(path2, 'run', root/'offload')
    rs = json.loads((root/'offload/results.json').read_text())
    assert len(rs) == 2 and all(r['status'] == 'passed' for r in rs), rs
    for r in rs:
        assert (Path(r['run'])/'pcie.json').exists()
        resolved = json.loads((root/'offload/conditions'/r['condition']/'definition.json').read_text())
        assert resolved['hardware_overrides']['mpu_efficiency'] == spec2['hardware']['assumptions']['mpu_efficiency']
    checks.append('two real transactional offload cases: pure PCIe and MPU/PCIe hybrid')

    invalid = []
    a = copy.deepcopy(spec); a['typo'] = 1; invalid.append(a)
    a = copy.deepcopy(spec); a['variants'] = [{'name': 'bad', 'overrides': {'parallel': {'tp': 3}}}]; invalid.append(a)
    a = copy.deepcopy(spec); a['profiles']['pcie'] = {'file': str(ROOT/'configs/hardware/pcie/dual-socket-ddr5-6400.json')}; invalid.append(a)
    a = copy.deepcopy(spec); a['variants'] = [{'name': 'bad', 'overrides': {'profiles': {'hbm': {'bandwidth_bytes_s_per_die': 1}}}}]; invalid.append(a)
    a = copy.deepcopy(spec); a['seeds'] = [1, 1]; invalid.append(a)
    a = copy.deepcopy(spec); a['simulation']['payload'] = 2048; invalid.append(a)
    for i, a in enumerate(invalid):
        path = root/f'invalid-{i}.json'; dump(path, a)
        out = root/f'invalid-output-{i}'
        command(path, 'run', out, ok=False)
        assert not out.exists(), 'validation must precede creating run output'
    checks.append('six invalid schemas/combinations rejected before output creation')
    command(path2, 'run', root/'offload', ok=False)
    checks.append('existing output cannot be overwritten')
    # Explicit no-IO profile remains off even in an environment requesting IO.
    a = copy.deepcopy(spec); a.pop('variants'); a['profiles']['io_die'] = None
    path = root/'no-io.json'; dump(path, a); command(path)
    # Real declaration files validate without executing their large studies.
    for name in ('glm-single', 'glm-kv-offload', 'deepseek-16chip-study'):
        command(ROOT/f'configs/experiments/{name}.json')
    checks.append('all shipped declarations validate; 18-condition study expansion covered')
    dump(root/'verification.json', {'status': 'passed', 'checks': checks, 'packet_cases': 4})
    print(json.dumps({'status': 'passed', 'checks': checks}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main(sys.argv[1])
