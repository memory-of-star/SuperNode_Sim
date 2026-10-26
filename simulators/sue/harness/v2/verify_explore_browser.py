"""Optional Playwright acceptance checks for the offline exploration dashboard."""
import json,sys
from pathlib import Path
from playwright.sync_api import sync_playwright
root=Path(sys.argv[1]).resolve()
with sync_playwright() as p:
 browser=p.chromium.launch(channel="chromium",headless=True,args=['--no-sandbox'])
 page=browser.new_page(viewport={'width':1440,'height':1100},device_scale_factor=1)
 errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
 page.goto((root/'report.html').as_uri());page.locator('#landscape-chart svg').wait_for()
 assert page.locator('.heatcell').count()==16
 page.screenshot(path=str(root/'preview-desktop.png'))
 page.locator('.heatcell[data-model="glm-5.3"][data-variant="n64-hotspot80-c0"]').click()
 assert page.locator('#model').input_value()=='glm-5.3'
 assert page.locator('#variant').input_value()=='n64-hotspot80-c0'
 assert page.locator('#rows tr').count()==6
 page.select_option('#study','ring_rate')
 assert 'log' not in page.locator('#times').inner_text()
 assert 'Gbit/s' in page.locator('#times').inner_text()
 page.select_option('#study','ring_buffer')
 assert 'log₂' in page.locator('#times').inner_text()
 page.select_option('#study','packetization');page.select_option('#variant','512')
 assert page.locator('#rows tr').count()==6 and page.locator('#rows .failed').count()==3
 assert '未完成' in page.locator('#times').inner_text()
 with page.expect_download() as dl:page.locator('#export').click()
 dl.value.save_as(str(root/'browser-filter-export.csv'))
 assert len((root/'browser-filter-export.csv').read_text(encoding='utf-8-sig').splitlines())==7
 with page.expect_download() as dl:page.locator('[data-export="times"]').click()
 dl.value.save_as(str(root/'browser-chart-export.svg'))
 assert '<svg' in (root/'browser-chart-export.svg').read_text()
 page.select_option('#study','ring_rate');page.locator('#results').scroll_into_view_if_needed()
 page.screenshot(path=str(root/'preview-resource.png'))
 assert page.evaluate('document.documentElement.scrollWidth<=window.innerWidth')
 page.set_viewport_size({'width':390,'height':844});page.evaluate('window.scrollTo(0,0)')
 page.screenshot(path=str(root/'preview-mobile.png'))
 assert page.evaluate('document.documentElement.scrollWidth<=window.innerWidth')
 assert not errors,errors
 browser.close()
 result={'status':'passed','browser':'Playwright Chromium','offline':True,'viewports':['1440x1100','390x844'],'checks':['heatmap click filters correct model and condition','linear and log resource axes','three failures retained in 512B filter','CSV download contents','SVG download','no page errors','no document horizontal overflow']}
 (root/'browser-verification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
 print(json.dumps(result,ensure_ascii=False))
