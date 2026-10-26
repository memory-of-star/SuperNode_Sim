#!/usr/bin/env python3
"""Validate report aggregates against raw evidence, independent of renderer bins."""
import csv, hashlib, json, math, sys
from pathlib import Path


def verify(folder):
    root = Path(folder).resolve()
    data = json.loads((root/'report-data.json').read_text())
    read = lambda p: list(csv.DictReader(p.open()))
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    count = 0
    for r in data['results']:
        run = root/r['evidence']
        v = data['details'][r['key']]['visuals']
        tasks = read(run/'task_statistics.csv')
        ports = read(run/'port_statistics.csv')
        routes = read(run/'token_routes.csv')
        assert sum(map(sum,v['card_bytes'])) == r['logical_bytes'] == r['received_bytes']
        # Integral of mean concurrency must equal sum of network task lifetimes.
        expected = sum(int(t['end_ps'])-int(t['start_ps']) for t in tasks if int(t['bytes']))
        measured = sum(map(sum,v['activity'])) * r['sim_end_us']*1e6 / v['activity_bins']
        assert math.isclose(expected, measured, rel_tol=1e-12, abs_tol=.01)
        by_pair = {}
        stride = v['card_groups'][0][1]+1
        for t in tasks:
            if int(t['bytes']):
                key = int(t['src_chip'])//stride, int(t['dst_chip'])//stride
                by_pair[key] = by_pair.get(key,0)+int(t['bytes'])
        for i,row in enumerate(v['card_bytes']):
            for j,n in enumerate(row): assert n == by_pair.get((i,j),0)
        weighted = sum(value*v['port_counts'][i][j] for i,row in enumerate(v['port_load']) for j,value in enumerate(row) if value is not None)
        expected_busy = sum(int(p['tx_busy_ps']) for p in ports if p['side']=='1')/(r['sim_end_us']*1e6)*100
        assert math.isclose(weighted,expected_busy,rel_tol=1e-12)
        assert sum(map(sum,v['port_counts'])) == sum(p['side']=='1' for p in ports)
        assert sum(e['assignments'] for e in v['expert_assignments']) == len(routes) == v['expert_total_assignments']
        manifest=json.loads((run/'command-manifest.json').read_text())
        for name,digest in manifest['inputs'].items():
            assert sha(run/name)==digest,run/name
            count+=1
    import xml.etree.ElementTree as ET
    for name,diagram in data['topology_diagrams'].items():
        r=next(r for r in data['results'] if r['topology']==name)
        raw=[{k:int(v) for k,v in p.items()} for p in read(root/r['evidence']/'ports.csv')]
        index={(p['chip'],p['die'],p['port']):p for p in raw}
        assert len(raw)==diagram['links']
        for p in diagram['representative_ports']:
            assert p==index[p['chip'],p['die'],p['port']]
            label=(f"XPU0.Die{p['die']} P{p['port']} → SW{p['switch']} VRF{p['vrf']} 物理口{p['physical']}")
            if p['chip']==0:assert label in diagram['svg']
        for sw in diagram['switches']:
            rr=[p for p in raw if p['switch']==sw['switch']]
            assert len(rr)==sw['used'] and sorted({p['vrf'] for p in rr})==sw['vrfs']
        for pair in diagram['die_pairs']:
            actual=[]
            for i in range(8):
                a=index[0,pair['src_die'],i];b=index[1,pair['dst_die'],i]
                if (a['switch'],a['vrf'])==(b['switch'],b['vrf']):actual.append(i)
            assert actual==pair['direct_ports']
            assert pair['guaranteed_groups']==[g for g in range(2) if set(range(g*4,g*4+4)).issubset(actual)]
        assert (root/'topologies'/f'{name}.svg').read_text()==diagram['svg']
        ET.fromstring(diagram['svg'])
    for model in ['kimi-k3','glm-5.3']:
        for window in ['prefill','decode','train']:
            rr=[next(r for r in data['results'] if r['topology']==t and r['model']==model and r['window']==window) for t in ['xpu128-parity','xpu128-rotate']]
            a,b=[root/r['evidence'] for r in rr]
            for f in ['tasks.json','token_routes.csv']:assert (a/f).read_bytes()==(b/f).read_bytes()
            for f in ['ports.csv','path_plan.csv']:assert (a/f).read_bytes()!=(b/f).read_bytes()
    before=json.loads((root/'rename-evidence-before.json').read_text())
    assert sha(root/'results.json')==before['results_sha256']
    assert sha(root/'summary.csv')==before['summary_sha256']
    result=dict(status='passed',cases=len(data['results']),input_hashes_checked=count,
        checks=['card-byte matrix per-cell conservation','task-concurrency integral equals total network-task duration',
                'port grouped weighted means and connected-port counts','expert route assignment total',
                'original experiment results and summary unchanged','immutable input SHA256',
                '16 topology diagrams match frozen port maps and direct-rail reachability','six parity/rotate input equality and physical-path difference comparisons'],
        note='report-data.json intentionally extended with derived chart aggregates; no simulation rerun')
    (root/'visualization-verification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(result,ensure_ascii=False,indent=2))

if __name__=='__main__':verify(sys.argv[1])
