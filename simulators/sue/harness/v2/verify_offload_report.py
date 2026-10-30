"""Validate rendered offload tables, local downloads, filter, and optional old MPU control."""
import argparse,json,os,re
from pathlib import Path
from run import dump,sha

def main():
    parser=argparse.ArgumentParser();parser.add_argument('root',type=Path);parser.add_argument('--historical',type=Path)
    args=parser.parse_args();root=args.root.resolve()
    results=json.loads((root/'results.json').read_text());rows=json.loads((root/'summary.json').read_text())
    fair=any(r.get('hbm_profile') for r in results)
    assert len(results)==(330 if fair else 210) and all(r['status']=='passed' for r in results)
    assert len([r for r in rows if r['variant']=='share-confirmation'])==(25 if fair else 5)
    if fair:
        assert len({json.dumps(r['hbm_profile'],sort_keys=True) for r in results})==1
        assert len({json.dumps(r['pcie_profile'],sort_keys=True) for r in results})==1
        selected=json.loads((root/'plan.json').read_text())['selected']
        assert len(selected)==25
        for model,target in sorted({(r['model'],r['target']) for r in rows}):
            conf=[r for r in rows if (r['model'],r['target'],r['variant'])==(model,target,'share-confirmation')]
            assert {r['mpus'] for r in conf}=={1,2,4,8,16}
            assert all(r['seeds']==2 and r['hbm_policy']=='capped-max-min' for r in conf)
            for m in (1,2,4,8,16):
                candidates=[r for r in results if (r['model'],r['target'],r['mpus'],r['backend'],r['seed'])==(model,target,m,'hybrid',1)]
                assert len(candidates)==5
                fraction=lambda r:r.get('mpu_fraction',190*m/(2048+190*m))
                best=min(candidates,key=lambda r:(r['block_us'],fraction(r)))
                choice=next(r for r in selected if (r['model'],r['target'],r['mpus'])==(model,target,m))
                assert choice['selection_case_index']==best['case_index'] and choice['mpu_fraction']==fraction(best)
                held=[r for r in results if (r['model'],r['target'],r['mpus'],r.get('study_variant'))==(model,target,m,'share-confirmation')]
                assert len(held)==2 and {r['seed'] for r in held}=={2,3}
                assert all(r['mpu_fraction']==choice['mpu_fraction'] for r in held)
                if choice['selected_variant']=='main':
                    for r in held:
                        nominal=next(n for n in results if (n['model'],n['target'],n['mpus'],n['backend'],n['seed'],n.get('study_variant','main'))==(model,target,m,'hybrid',r['seed'],'main'))
                        assert r['block_us']==nominal['block_us']
                        for name in ('tasks.json','resources.json','task_statistics.csv','hbm_sources.csv','port_statistics.csv'):
                            assert sha(Path(r['run'])/name)==sha(Path(nominal['run'])/name)
                for r in held:
                    baseline=next(b for b in results if (b['model'],b['target'],b['backend'],b['seed'])==(model,target,'pcie',r['seed']))
                    assert sha(Path(r['run'])/'token_routes.csv')==sha(Path(baseline['run'])/'token_routes.csv')
        # Each green line contains five measured vertices, not interpolated points.
        svg=(root/'offload-speedup.svg').read_text()
        assert 'hybrid: confirmed share' in svg
        lines=json.loads((root/'plot-data.json').read_text())
        green=[r for r in lines if r['label']=='hybrid: confirmed share']
        assert len(green)==5 and all(r['x']==[1,2,4,8,16] for r in green)
        for line in green:
            rs=sorted([r for r in rows if (r['model'],r['target'],r['variant'])==(line['model'],line['target'],'share-confirmation')],key=lambda r:r['mpus'])
            assert line['y']==[r['speedup_vs_PCIe'] for r in rs]
            assert line['fractions']==[r['mpu_fraction'] for r in rs]

    assert len([r for r in rows if r['backend']=='pcie' and r['variant']=='main'])==5
    for row in rows:
        assert abs(row['speedup_vs_PCIe']-row['PCIe_baseline_us']/row['mean_block_us'])<1e-12
        assert row['transport_model']=='transactional' and row['pcie_setup_ns'] is None
    manifests=[json.loads((Path(r['run'])/'command-manifest.json').read_text()) for r in results]
    assert len({m['binary_sha256'] for m in manifests})==1
    assert len({m['adapter_sha256'] for m in manifests})==1
    assert len({json.dumps(m['adapter_headers_sha256'],sort_keys=True) for m in manifests})==1
    if any(r.get('io_die') for r in results):
        profiles=[]
        plan=json.loads((root/'plan.json').read_text())
        expected_slots=plan.get('mpu_max_inflight',64)
        expected_precision=plan.get('time_resolution_ns',1)
        for r in results:
            case=Path(r['run']);spec=json.loads((case/'experiment.json').read_text());s=spec['settings']
            assert s['time_resolution_ns']==expected_precision
            assert all(s[k]==1 for k in ('visibility_ns','interval_ns'))
            if 'io_event_driven' in plan:assert s['io_event_driven']==plan['io_event_driven']
            assert s['xpu_port_delay_ns']==s['mpu_port_delay_ns']==400
            io=json.loads((case/'io-die.json').read_text());profiles.append(io['profile'])
            assert io['profile']['mode']=='best_effort' and io['profile']['mpu_service_unit']=='axi'
            if io.get('axi_mpu'):
                assert io['axi_mpu']['max_inflight']==expected_slots and io['axi_mpu']['latency_ns']==200
                assert io['axi_mpu']['bandwidth_bytes_s']==190000000000
        assert all(p==profiles[0] for p in profiles)
    checks=[f'{len(results)} completed records',f'5 PCIe baselines and {25 if fair else 5} independent-seed confirmations','speedup arithmetic','same binary/adapter/header hashes across sweep','inactive legacy setup excluded from summaries']
    if fair:
        generation=json.loads((root/'report-generation.json').read_text())
        assert all(sha(root/name)==value for name,value in {**generation['inputs'],**generation['outputs']}.items())
    if any(r.get('io_die') for r in results):checks.append(f'Every case uses precision={expected_precision} (0=PS, 1=NS), 1ns issue/notification, 400ns access ports and identical best-effort AXI profile; MPU {expected_slots} slots, 200ns, 190GB/s')
    if fair:checks.extend(['25 selection decisions recomputed from seed1 only, including nominal candidate','all five MPU counts confirmed on held-out seeds2/3','same fair HBM and transactional PCIe profiles across all arms'])
    if args.historical:
        assert not fair, 'Fair HBM changes pure-MPU timing; historical FIFO byte equality does not apply'
        old=json.loads((args.historical/'results.json').read_text())
        key=lambda r:(r['model'],r['target'],r['mpus'],r['seed'])
        ref={key(r):r for r in old if r['backend']=='mpu' and r.get('study_variant','main')=='main'}
        count=0
        for r in results:
            if r['backend']!='mpu' or r.get('study_variant','main')!='main':continue
            prior=ref[key(r)];assert r['block_us']==prior['block_us']
            for name in ('tasks.json','resources.json','token_routes.csv','task_statistics.csv','port_statistics.csv','resource_statistics.csv'):
                assert sha(Path(r['run'])/name)==sha(Path(prior['run'])/name),(r['case_index'],name)
            count+=1
        assert count==75;checks.append('75 pure-MPU controls reproduce historical tasks/resources/routes and timing/port/resource statistics byte-for-byte')
    from playwright.sync_api import sync_playwright
    with sync_playwright() as pw:
        browser=pw.chromium.launch(executable_path=os.environ.get('SUE_CHROMIUM'),headless=True,args=['--no-sandbox'])
        page=browser.new_page(viewport={'width':1440,'height':1100});errors=[]
        page.on('pageerror',lambda e:errors.append(str(e)))
        page.goto((root/'report.html').as_uri())
        assert page.locator('[data-workload]').count()==5
        assert page.locator('img').evaluate_all('(xs)=>xs.length>0&&xs.every(x=>x.complete&&x.naturalWidth>0)')
        for link in page.locator('a').evaluate_all('(xs)=>xs.map(x=>x.getAttribute("href"))'):
            if not re.match(r'\w+://|#',link):assert (root/link).is_file(),link
        page.select_option('#filter','w1');assert page.locator('[data-workload]:visible').count()==1
        page.select_option('#filter','all');assert page.locator('[data-workload]:visible').count()==5
        for width in (1440,390):
            page.set_viewport_size({'width':width,'height':1000})
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
        assert not errors;browser.close()
    checks.extend(['all 5 workload filters','SVG loads','local downloads exist','desktop/mobile no overflow','no JavaScript errors'])
    dump(root/'report-validation.json',dict(status='passed',checks=checks))
    print('Offload report validation PASS')
if __name__=='__main__':main()
