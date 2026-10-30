"""Optional offline browser and local-link checks for the manual."""
import json,re,os
from pathlib import Path
from playwright.sync_api import sync_playwright
root=Path(__file__).resolve().parent
for p in [root/'README.md',*root.glob('[0-9][0-9]-*.md')]:
 for link in re.findall(r'\]\(([^)]+)\)',p.read_text()):
  if '://' not in link and not link.startswith('#'):assert (p.parent/link.split('#')[0]).exists(),(p,link)
with sync_playwright() as p:
 browser=p.chromium.launch(executable_path=os.environ.get('SUE_CHROMIUM'),channel=None if os.environ.get('SUE_CHROMIUM') else 'chromium',headless=True,args=['--no-sandbox'])
 page=browser.new_page(viewport={'width':1440,'height':1100});errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
 expected_sections=1+len(list(root.glob('[0-9][0-9]-*.md')))
 page.goto((root/'manual.html').as_uri());assert page.locator('main section').count()==expected_sections and page.locator('table').count()>=15
 assert page.locator('img').evaluate_all('(a)=>a.every(x=>x.complete&&x.naturalWidth>0)')
 harness=page.locator('img[alt^="AI harness"]')
 assert harness.count()==1 and harness.get_attribute('src').startswith('data:image/svg+xml;base64,')
 assert page.locator('#ai-harness').count()==1
 assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
 assert page.locator('a[href^="#"]').evaluate_all('(a)=>a.every(x=>document.getElementById(x.hash.slice(1)))')
 page.screenshot(path=str(root/'preview-desktop.png'))
 page.locator('[id="02-configuration"]').scroll_into_view_if_needed();page.screenshot(path=str(root/'preview-config.png'))
 page.set_viewport_size({'width':390,'height':844});page.evaluate('scrollTo(0,0)');assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
 page.screenshot(path=str(root/'preview-mobile.png'));assert not errors,errors
 browser.close()
(root/'browser-verification.json').write_text(json.dumps({'status':'passed','checks':['local Markdown links',f'{expected_sections} sections and at least15 tables','all manual images load','all internal section links','desktop/mobile no overflow','no JavaScript errors']},ensure_ascii=False,indent=2));print('Manual browser acceptance PASS')
