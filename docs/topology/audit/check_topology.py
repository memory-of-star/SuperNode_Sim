#!/usr/bin/env python3
"""Structural audit of topology.md formulas, NOT a packet or UCIe simulation.
Original wording is preserved in topology.md; formulas take precedence over
inconsistent prose (e.g. 8+8 physical port ranges). No simulator code is changed.
"""
from collections import Counter, defaultdict
from itertools import combinations
from pathlib import Path
import hashlib
import json

# name, XPU count, MPU count, switch count, radix, mapping family
CASES = [
    ('8 XPU', 8, 0, 1, 256, 'small'),
    ('16 XPU', 16, 0, 2, 256, 'small'),
    ('32 XPU', 32, 0, 4, 256, 'small'),
    ('64 XPU', 64, 0, 8, 256, 'small'),
    ('128 XPU 奇偶分面', 128, 0, 16, 256, 'parity'),
    ('128 XPU 分法轮转', 128, 0, 16, 256, 'rotate'),
    ('256 XPU', 256, 0, 32, 256, 'pure'),
    ('6+8', 6, 8, 1, 256, 'single'),
    ('8+8', 8, 8, 2, 256, 'mixed_two'),
    ('32+32', 32, 32, 8, 256, 'rail'),
    ('64+64 主方案', 64, 64, 16, 256, 'mixed_parity'),
    ('64+64 内存池备选', 64, 64, 16, 256, 'pool'),
    ('18 XPU', 18, 0, 1, 576, 'single'),
    ('16+8', 16, 8, 1, 576, 'single'),
    ('144 XPU', 144, 0, 8, 576, 'rail'),
    ('576 XPU', 576, 0, 32, 576, 'pure'),
]
PARTITIONS = (((0, 1), (2, 3)), ((0, 2), (1, 3)), ((0, 3), (1, 2)))


def audit(case):
    name, nx, nm, ns, radix, family = case
    ports = {}             # (chip, die, local port) -> (switch, VRF, physical port)
    occupied = set()
    domains = defaultdict(set)
    loads = Counter()

    def add(chip, die, i, sw, vrf, p):
        assert 0 <= sw < ns and 0 <= p < radix
        assert (sw, p) not in occupied, (name, 'physical port collision')
        assert (chip, die) not in domains[sw, vrf], (name, 'SA duplicate in VRF')
        assert (chip, die, i) not in ports
        occupied.add((sw, p))
        domains[sw, vrf].add((chip, die))
        loads[sw] += 1
        ports[chip, die, i] = (sw, vrf, p)

    for x in range(nx):
        for d in range(4):
            for i in range(8):
                if family == 'small':
                    sw, v, p = i % ns, i // ns, (i // ns) * (4 * nx) + 4*x + d
                elif family == 'single':
                    sw, v, p = 0, i, 32*x + 8*d + i
                elif family == 'mixed_two':
                    sw, v, p = i % 2, i // 2, (i // 2)*40 + 4*x + d
                elif family == 'rail':
                    sw, v, p = i, 0, 4*x + d
                elif family in ('parity', 'mixed_parity', 'pool'):
                    sw, v, p = 2*i + d % 2, 0, 2*x + d // 2
                elif family == 'rotate':
                    part = PARTITIONS[i % 3]
                    side = next(s for s in range(2) if d in part[s])
                    sw, v, p = 2*i + side, 0, 2*x + part[side].index(d)
                elif family == 'pure':
                    sw, v, p = 8*d + i, 0, x
                else:
                    raise ValueError(family)
                add(x, d, i, sw, v, p)
    for j in range(nm):
        for i in range(8):
            if family == 'single':
                sw, v, p = 0, i, 32*nx + 8*j + i
            elif family == 'mixed_two':
                sw, v, p = i % 2, i // 2, (i // 2)*40 + 32 + j
            elif family == 'rail':
                sw, v, p = i, 0, 4*nx + j
            elif family == 'mixed_parity':
                sw, v, p = 2*i + int(i >= 4), 0, 128 + j
            elif family == 'pool':
                sw, v, p = 2*i + (nx+j) % 2, 0, 128 + j // 2
            else:
                raise ValueError(family)
            add(nx+j, 0, i, sw, v, p)

    assert len(ports) == 32*nx + 8*nm == sum(loads.values())
    assert set(loads) == set(range(ns)) and max(loads.values()) <= radix
    for chip in range(nx+nm):
        for d in range(4 if chip < nx else 1):
            assert len({ports[chip, d, i][:2] for i in range(8)}) == 8, 'R1'
    # All cards of a given die/rail have the same domain: proves R2 for every
    # cross-card pair without enumerating O(nx^2) identical comparisons.
    for d in range(4):
        for i in range(8):
            assert len({ports[x, d, i][:2] for x in range(nx)}) == 1, 'R2'

    def direct(src, dst):
        return [int(ports[*src, i][:2] == ports[*dst, i][:2]) for i in range(8)]

    pair_paths, pair_groups = {}, {}
    for a, b in combinations(range(4), 2):
        bits = direct((0, a), (1, b))
        pair_paths[f'{a}-{b}'] = sum(bits)
        pair_groups[f'{a}-{b}'] = [g for g in (0, 1) if all(bits[g*4:g*4+4])]
    r5_paths, r5_groups, mm_paths = Counter(), Counter(), Counter()
    for d in range(4):
        for m in range(nx, nx+nm):
            bits = direct((0, d), (m, 0))
            r5_paths[sum(bits)] += 1
            r5_groups[sum(all(bits[g*4:g*4+4]) for g in (0, 1))] += 1
    for a, b in combinations(range(nx, nx+nm), 2):
        mm_paths[sum(direct((a, 0), (b, 0)))] += 1
    expected = ([3, 3, 2, 2, 3, 3] if family == 'rotate' else
                [0, 8, 0, 0, 8, 0] if family in ('parity', 'mixed_parity', 'pool') else
                [0]*6 if family == 'pure' else [8]*6)
    assert list(pair_paths.values()) == expected, (name, pair_paths, expected)
    assert sum(len(s) for s in domains.values()) == len(ports)
    return dict(name=name, xpu=nx, mpu=nm, switches=ns, radix=radix,
                links=len(ports), domains=len(domains), sa_count=nx*4+nm,
                switch_load_histogram=dict(sorted(Counter(loads.values()).items())),
                domain_size_histogram=dict(sorted(Counter(map(len, domains.values())).items())),
                r1=True, r2=True, physical_ports_unique=True, sa_unique_per_domain=True,
                cross_die_direct_rails=pair_paths, cross_die_guaranteed_groups=pair_groups,
                die_mpu_direct_rails_histogram=dict(sorted(r5_paths.items())),
                die_mpu_guaranteed_group_count_histogram=dict(sorted(r5_groups.items())),
                mpu_mpu_direct_rails_histogram=dict(sorted(mm_paths.items())))


if __name__ == '__main__':
    root = Path(__file__).resolve().parent
    results = [audit(c) for c in CASES]
    data = dict(kind='structural_formula_audit_not_ns3_simulation',
                source_sha256=hashlib.sha256((root.parent/'topology.md').read_bytes()).hexdigest(),
                script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                note='Die-MPU histograms use one representative XPU (card-independent domain mapping verified); MPU-MPU uses all unordered MPU pairs. No UCIe, performance, hardware lane map or failure recovery is simulated.',
                cases=results)
    (root/'results.json').write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n')
    for r in results:
        print(f"PASS {r['name']}: {r['links']} links, {r['domains']} domains, loads={r['switch_load_histogram']}, die-pair rails={list(r['cross_die_direct_rails'].values())}")
    print(f'{len(results)} structural audits passed. Not packet-level simulation.')
