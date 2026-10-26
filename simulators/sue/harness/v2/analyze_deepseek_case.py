"""Evidence tables for the DeepSeek 16-chip case study."""
import csv,json,sys,statistics
from pathlib import Path
from collections import defaultdict
from run import dump,csvwrite,csvread

def main():
 root=Path(sys.argv[1]).resolve();rs=json.loads((root/'results.json').read_text());assert all(r['status']=='passed' for r in rs)
 groups=defaultdict(list)
 for r in rs:groups[r['study']].append(r)
 table=[]
 for name,rows in groups.items():
  times=[r['window_us'] for r in rows];c=rows[0]['capacity']
  table.append(dict(study=name,n=len(rows),tp=rows[0]['tp'],mpus=rows[0]['mpus'],mean_us=statistics.mean(times),min_us=min(times),max_us=max(times),per_step_block_us=statistics.mean(times)/2,die_used_GB=c['die_used_GB'],total_local_GB=c['local_used_GB'],remote_capacity_GB=c['remote_capacity_GB']))
 csvwrite(root/'study-summary.csv',table)
 r=next(r for r in rs if r['study']=='tp4-mpu2' and r['seed']==1);run=Path(r['run']);tasks=json.loads((run/'tasks.json').read_text());stats=csvread(run/'task_statistics.csv');resources=csvread(run/'resource_statistics.csv')
 buckets=defaultdict(list);servers=defaultdict(float);phases=defaultdict(list)
 for row in resources:
  label=tasks[int(row['id'])]['label'].split(':',1)[1];buckets[label].append(row)
  servers[int(row['chip']),int(row['die']),int(row['kind'])]+=int(row['service_ns'])/1000
 for t,row in zip(tasks,stats):
  phases[t['label'].split(':',1)[1]].append(row)
 resource_summary=[]
 for label,rows in buckets.items():
  resource_summary.append(dict(phase=label,tasks=len(rows),units=sum(int(x['units']) for x in rows),service_sum_us=sum(int(x['service_ns']) for x in rows)/1000,queue_sum_us=sum(int(x['queued_ps']) for x in rows)/1e6,queue_max_us=max(int(x['queued_ps']) for x in rows)/1e6))
 phase_summary=[]
 for label,rows in phases.items():
  phase_summary.append(dict(phase=label,first_start_us=min(int(x['start_ps']) for x in rows)/1e6,last_end_us=max(int(x['end_ps']) for x in rows)/1e6,app_bytes=sum(int(x['bytes']) for x in rows)))
 dump(root/'resource-summary.json',resource_summary);dump(root/'phase-summary.json',phase_summary)
 dump(root/'server-busy.json',[dict(chip=c,die=d,kind=k,busy_us=v,busy_fraction=v/r['window_us']) for (c,d,k),v in servers.items()])
 base=r['window_us'];sensitivity=[dict(study=x['study'],window_us=x['window_us'],reduction_pct=(1-x['window_us']/base)*100) for x in rs if x['study'] in ('hbm-2x','compute-2x','mpu-2x','fabric-2x')];dump(root/'sensitivity.json',sensitivity)
 dump(root/'case-metrics.json',dict(base_run=str(run),window_us=base,active_dispatch_dies=r['moe_dispatch_active_dies'],active_combine_dies=r['moe_combine_active_dies'],local_d2d_bytes=r['local_d2d_bytes'],wire_bytes=r['wire_bytes'],engram_return_bytes=sum(int(x['bytes']) for x in phases['engram:data-response']),engram_request_bytes=sum(int(x['bytes']) for x in phases['engram:request']),capacity=r['capacity'],sensitivity=sensitivity))
 print(json.dumps({'summary':table,'resources':resource_summary,'sensitivity':sensitivity,'server_busy_max':{str(k):max(v for (c,d,kind),v in servers.items() if kind==k) for k in (0,1,2)}},ensure_ascii=False,indent=2))
if __name__=='__main__':main()
