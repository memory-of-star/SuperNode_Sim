"""Derived visual aggregates; never infer packet time series from port totals."""
from collections import Counter, defaultdict
from math import ceil


def aggregate(stats, ports, routes, result):
    nx = result['xpu']
    stride = max(1, ceil(nx / 32))
    groups = [[i, min(i + stride - 1, nx - 1)] for i in range(0, nx, stride)]
    n = len(groups)
    matrix = [[0] * n for _ in range(n)]
    # Time-weighted mean outstanding tasks per source group in 64 equal bins.
    bins = 64
    end_ps = result['sim_end_us'] * 1e6
    width = end_ps / bins
    activity = [[0.0] * bins for _ in range(n)]
    for row in stats:
        size = int(row['bytes'])
        if not size:
            continue
        src, dst = int(row['src_chip']), int(row['dst_chip'])
        assert 0 <= src < nx and 0 <= dst < nx, 'Model charts require XPU-only traffic'
        matrix[src // stride][dst // stride] += size
        start, end = int(row['start_ps']), int(row['end_ps'])
        assert 0 <= start <= end <= end_ps + 1
        for b in range(max(0, int(start / width)), min(bins, ceil(end / width))):
            activity[src // stride][b] += max(0, min(end, (b + 1) * width) - max(start, b * width)) / width
    assert sum(map(sum, matrix)) == result['logical_bytes']
    # Connected switch-side ports only. Empty groups remain null, never zero.
    switch_ids = sorted({int(p['switch']) for p in ports if p['side'] == '1'})
    max_port = max(int(p['physical_port']) for p in ports if p['side'] == '1')
    port_groups = [[i, min(i + 15, max_port)] for i in range(0, max_port + 1, 16)]
    busy, counts = defaultdict(float), Counter()
    for p in ports:
        if p['side'] != '1':
            continue
        key = (int(p['switch']), int(p['physical_port']) // 16)
        value = int(p['tx_busy_ps']) / end_ps * 100
        assert 0 <= value <= 100 + 1e-8
        busy[key] += value
        counts[key] += 1
    port_load = [[busy[s, b] / counts[s, b] if counts[s, b] else None
                  for b in range(len(port_groups))] for s in switch_ids]
    experts = Counter(int(row['expert']) for row in routes)
    return dict(card_groups=groups, card_bytes=matrix, activity=activity,
                activity_end_us=result['sim_end_us'], activity_bins=bins,
                port_groups=port_groups, port_switches=switch_ids, port_load=port_load,
                port_counts=[[counts[s,b] for b in range(len(port_groups))] for s in switch_ids],
                expert_assignments=[dict(expert=k, assignments=v) for k, v in sorted(experts.items(), key=lambda kv:(-kv[1],kv[0]))],
                expert_total_assignments=len(routes), expert_local_assignments=sum(r['local']=='True' for r in routes))
