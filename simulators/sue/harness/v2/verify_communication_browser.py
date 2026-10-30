"""Offline dashboard acceptance: all charts, seed switching, hover, CSV and image export."""
import json,sys,os
from pathlib import Path
from playwright.sync_api import sync_playwright
root=Path(sys.argv[1]).resolve();errors=[]
metrics={r['seed']:r for r in json.loads((root/'metrics.json').read_text())}
with sync_playwright() as p:
 browser=p.chromium.launch(executable_path=os.environ.get('SUE_CHROMIUM'),headless=True,args=['--no-sandbox'])
 page=browser.new_page(viewport={'width':1440,'height':1000},accept_downloads=True);page.on('pageerror',lambda e:errors.append(str(e)))
 page.goto((root/'report.html').as_uri());page.wait_for_function("document.querySelectorAll('.js-plotly-plot').length===12")
 page.wait_for_function("document.getElementById('12-mpu-waiting')._fullLayout!==undefined")
 assert page.locator('.js-plotly-plot').count()==12
 assert f"{metrics[1]['window_us']:.3f}" in page.locator('#metrics').inner_text()
 page.locator('#seed').select_option('2');page.wait_for_function("value => document.getElementById('metrics').textContent.includes(value)",arg=f"{metrics[2]['window_us']:.3f}")
 assert page.evaluate("document.getElementById('01-card-matrix').data[0].z.length") ==18
 # Hover in matrix and exercise the downloadable CSV.
 page.evaluate("Plotly.Fx.hover('01-card-matrix',[{curveNumber:0,xval:0,yval:1}])")
 page.wait_for_function("document.getElementById('01-card-matrix').querySelectorAll('.hovertext').length>0")
 with page.expect_download() as info:page.locator('#csv').click()
 download=info.value;download.save_as(root/'browser-export.csv');assert 'max_sampled_switch_queue_KiB' in (root/'browser-export.csv').read_text()
 # Real SVG export from the current seed chart, not only static link existence.
 with page.expect_download() as info:page.evaluate("Plotly.downloadImage('01-card-matrix',{format:'svg',filename:'matrix-seed2'})")
 info.value.save_as(root/'browser-export.svg');assert '<svg' in (root/'browser-export.svg').read_text()
 page.locator('#seed').select_option('1');page.screenshot(path=str(root/'preview-desktop.png'))
 page.set_viewport_size({'width':390,'height':844});page.wait_for_timeout(1000)
 assert page.evaluate('document.documentElement.scrollWidth<=innerWidth'),page.evaluate('document.documentElement.scrollWidth')
 page.screenshot(path=str(root/'preview-mobile.png'));assert not errors,errors
 browser.close()
(root/'browser-verification.json').write_text(json.dumps({'status':'passed','checks':['12 offline plots','seed switch and metrics','heatmap hover','CSV download','Plotly SVG export','desktop/mobile no overflow','no JS errors']},ensure_ascii=False,indent=2));print('Communication browser PASS')
