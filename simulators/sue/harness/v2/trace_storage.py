"""Lossless optional gzip storage for completed traces; hashes cover decoded bytes."""
import gzip,hashlib,json,os,shutil
from pathlib import Path

def exists(path):
    path=Path(path)
    return path.exists() or Path(str(path)+'.gz').exists()

def open_trace(path,mode='rt'):
    path=Path(path)
    if path.exists():return path.open(mode)
    return gzip.open(str(path)+'.gz',mode)

def digest(path):
    h=hashlib.sha256()
    with open_trace(path,'rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()

def compress_completed(run):
    run=Path(run);rows=[]
    for name in ('io_records.csv','io_wire.csv','io_mpu_cycles.csv','io_mpu_requests.csv','io_packets.csv'):
        p=run/name
        if not p.exists():continue
        original=digest(p);size=p.stat().st_size;dest=Path(str(p)+'.gz');tmp=Path(str(dest)+'.tmp')
        with p.open('rb') as src,gzip.open(tmp,'wb',compresslevel=1) as dst:shutil.copyfileobj(src,dst,1024*1024)
        h=hashlib.sha256()
        with gzip.open(tmp,'rb') as src:
            for b in iter(lambda:src.read(1024*1024),b''):h.update(b)
        assert h.hexdigest()==original,'compressed trace verification failed'
        os.replace(tmp,dest)
        rows.append(dict(file=name,sha256=original,original_bytes=size,compressed_bytes=dest.stat().st_size))
        p.unlink()
    (run/'trace-storage.json').write_text(json.dumps(dict(format='gzip',hash_scope='decompressed original bytes',files=rows),indent=2)+'\n')
