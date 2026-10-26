#!/usr/bin/env python3
"""Serve only the approved report directory and its downloadable archive."""
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
from urllib.parse import unquote, urlsplit
import gzip
ROOT = (Path(__file__).resolve().parents[2]/'results/sue/v2-topology-kimi-glm53').resolve()
ARCHIVE = ROOT.parent/'SUE-v2-experiments.zip'
class ReportHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)
    def list_directory(self,path):
        self.send_error(404)
        return None
    def send_head(self):
        urlpath=unquote(urlsplit(self.path).path)
        if urlpath in ('/','/index.html'):
            self.path='/report.html'
            urlpath=self.path
        if urlpath=='/SUE-v2-experiments.zip':
            f=ARCHIVE.open('rb')
            self.send_response(200)
            self.send_header('Content-Type','application/zip')
            self.send_header('Content-Length',str(ARCHIVE.stat().st_size))
            self.send_header('Content-Disposition','attachment; filename="SUE-v2-experiments.zip"')
            self.end_headers()
            return f
        target=(ROOT/urlpath.lstrip('/')).resolve()
        if not target.is_relative_to(ROOT) or any(part.startswith('.') for part in Path(urlpath).parts):
            self.send_error(404)
            return None
        return super().send_head()
    def end_headers(self):
        self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('X-Robots-Tag','noindex, nofollow')
        super().end_headers()
ThreadingHTTPServer(('127.0.0.1',18765),ReportHandler).serve_forever()
