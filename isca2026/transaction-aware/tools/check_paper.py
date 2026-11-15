#!/usr/bin/env python3
"""Check the build artifact and frozen numerical claims, not scientific validity."""
import csv,json,re,subprocess,hashlib
from pathlib import Path
P=Path(__file__).resolve().parents[1]
def run(*args):return subprocess.check_output(args,text=True)
info=run('pdfinfo',str(P/'paper.pdf'));fonts=run('pdffonts',str(P/'paper.pdf'))
text=run('pdftotext',str(P/'paper.pdf'),'-')
log=(P/'build/main.log').read_text(errors='replace')
assert '612 x 792' in info,'not Letter'
pages=int(re.search(r'Pages:\s*(\d+)',info)[1])
aux=(P/'build/main.aux').read_text()
body_pages=int(re.search(r'\\newlabel\{page:body-end\}\{\{[^}]*\}\{(\d+)\}',aux)[1])
assert body_pages==11,'requested 11 pages of main text'
assert pages>body_pages,'references must start after main text'
page_text=run('pdftotext','-layout',str(P/'paper.pdf'),'-').split('\f')
assert 'R EFERENCES' in page_text[body_pages] or 'REFERENCES' in page_text[body_pages], 'references not on the next page'
assert not re.search(r'(?:Citation|Reference).*undefined|There were undefined|Overfull \\[hv]box',log), 'unresolved references or overflow'
assert 'LaTeX Font Warning:' not in log,'font warning or missing shape'
assert 'NimbusRom' in fonts or 'Termes' in fonts or 'Times' in fonts,'Times-compatible body font absent'
assert 'Type 3' not in fonts,'bitmap font'
assert not re.search(r'^Author:\s*\S',info,re.M),'author metadata present'
assert 'Not Submitted' in text
assert '22.37' in text and '372.474' in text and '289.155' in text
assert '6.43' in text and '9.33' in text
assert (P/'IEEEtran.cls').read_bytes()==(P/'vendor/official/IEEEtran.cls').read_bytes()
d=json.load(open(P/'evidence/precision-analysis.json'));rs={r['condition']:r for r in d['results']}
for prefix in ['ns','ps']:assert rs[prefix+'-poll-seed1']['block_us']==rs[prefix+'-wake-seed1']['block_us']
reduction=100*(1-rs['ps-wake-seed1']['block_us']/rs['ns-wake-seed1']['block_us']);assert abs(reduction-22.37)<0.005
packing=list(csv.DictReader(open(P/'evidence/packing-summary.csv')))
for seed in ['1','2','3']:
 a=next(r for r in packing if r['seed']==seed and r['mode']=='none');b=next(r for r in packing if r['seed']==seed and r['mode']=='best_effort')
 assert 100*(1-int(b['data_frames'])/int(a['data_frames']))>92
bbl=(P/'build/main.bbl').read_text();assert bbl.count('\\bibitem')==3;assert 'et~al.' not in bbl
result={'status':'passed','pages':pages,'main_text_pages':body_pages,'reference_pages':pages-body_pages,'letter':True,'fonts_embedded_listing':fonts,'precision_latency_reduction_pct':reduction,'official_class_unmodified':True,'pdf_sha256':hashlib.sha256((P/'paper.pdf').read_bytes()).hexdigest(),'scope':'Build, layout warnings, citation resolution, template integrity and frozen numerical checks; not hardware validation or submission certification.'}
(P/'build/verification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='fonts_embedded_listing'},indent=2))
