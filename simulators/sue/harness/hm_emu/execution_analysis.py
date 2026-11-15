"""Measure unions of event intervals; never add overlapping communication times."""
import csv

def union(intervals):
 out=[]
 for a,b in sorted(intervals):
  if b<=a:continue
  if out and a<=out[-1][1]:out[-1][1]=max(out[-1][1],b)
  else:out.append([a,b])
 return out

def duration(intervals):return sum(b-a for a,b in union(intervals))

def analyze(path,step=1):
 rows=[r for r in csv.DictReader((path/'engine-execution.csv').open()) if int(r['step'])==step]
 tokens=list(csv.DictReader((path/'engine-tokens.csv').open()));token=next(t for t in tokens if int(t['step'])==step and int(t['group'])==0);end=int(token['completed_ps']);start=end-int(token['interval_ps'])
 def spans(kind):return [(max(start,int(r['start_ps'])),min(end,int(r['end_ps']))) for r in rows if r['kind']==kind]
 loc=spans('local');comm=spans('collective');local=duration(loc);communication=duration(comm);combined=duration(loc+comm)
 total=end-start;assert 0<=local<=total and 0<=communication<=total
 return {'local_active_ms':local/1e9,'exposed_wait_ms':(total-local)/1e9,'collective_union_ms':communication/1e9,'collective_sum_ms':sum(b-a for a,b in comm)/1e9,'local_communication_overlap_ms':(local+communication-combined)/1e9,'tpot_ms':total/1e9,'collectives':len(comm)}
