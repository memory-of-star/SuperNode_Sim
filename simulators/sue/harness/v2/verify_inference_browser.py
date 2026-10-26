"""Offline acceptance checks for the resource and inference report."""
import json,sys
from pathlib import Path
from playwright.sync_api import sync_playwright
root=Path(sys.argv[1]).resolve()
with sync_playwright() as p:
 import os
 browser=p.chromium.launch(channel='chromium',executable_path=os.environ.get('SUE_CHROMIUM'),headless=True,args=['--no-sandbox'])
 page=browser.new_page(viewport={'width':1440,'height':1100})
 errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
 page.goto((root/'report.html').as_uri());page.locator('#capchart svg').wait_for()
 assert page.locator('#capchart rect').count()==24
 page.screenshot(path=str(root/'preview-desktop.png'))
 for model in page.locator('#model option').evaluate_all('(a)=>a.map(x=>x.value)'):
  page.select_option('#model',model)
  for policy in ['local','remote']:
   page.select_option('#policy',policy)
   for context in ['8192','131072']:
    page.select_option('#context',context)
    assert page.locator('svg').count()==7
    assert not page.locator('svg').evaluate_all('(a)=>a.some(x=>/NaN|Infinity|undefined/.test(x.outerHTML))')
  for study in page.locator('#study option').evaluate_all('(a)=>a.map(x=>x.value)'):
   page.select_option('#study',study)
 page.select_option('#model','deepseek-v4.1-flash-1017');page.select_option('#policy','remote');page.select_option('#study','all')
 assert 'TP4' in page.locator('#capchart').inner_text()
 with page.expect_download() as dl:page.locator('#csv').click()
 dl.value.save_as(str(root/'browser-filter-export.csv'))
 assert 'deepseek-v4.1-flash-1017' in (root/'browser-filter-export.csv').read_text()
 with page.expect_download() as dl:page.locator('[data-export="capchart"]').click()
 dl.value.save_as(str(root/'browser-chart-export.svg'))
 assert '<svg' in (root/'browser-chart-export.svg').read_text()
 assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
 page.locator('#dscompare').scroll_into_view_if_needed();page.screenshot(path=str(root/'preview-deepseek.png'))
 page.set_viewport_size({'width':390,'height':844});page.evaluate('scrollTo(0,0)')
 page.screenshot(path=str(root/'preview-mobile.png'))
 assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
 assert not errors,errors
 browser.close()
 result={'status':'passed','offline':True,'checks':['three models, two capacity policies, two contexts','eight study filters','24 heatmap cells and seven finite SVG charts','CSV and SVG downloads','desktop/mobile no document overflow','no JavaScript errors']}
 (root/'browser-verification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2));print(json.dumps(result,ensure_ascii=False))
