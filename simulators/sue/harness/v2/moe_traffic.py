"""Attribute completed MoE payloads to physical Die endpoints (not wire utilization)."""
from collections import defaultdict

def die_traffic(dag, stats, ranks, window_us):
 stats={int(r['id']):r for r in stats};resources={r['id']:r for r in dag.resources}
 totals=defaultdict(int)
 for t in dag.tasks:
  phase=t['label'].split(':')[-1]
  if phase not in ('dispatch','combine'):continue
  row=stats[t['id']]
  if int(row['end_ps'])==0:continue
  sc,sd,dc,dd=(t[k] for k in ('src_chip','src_die','dst_chip','dst_die'))
  kind='fabric' if sc!=dc else 'd2d'
  if kind=='d2d':
   resource=resources[t['id']];dd=resource['peer_die'];size=resource['units']
  else:size=t['bytes']
  totals[phase,sc*4+sd,kind+'_tx_bytes']+=size
  totals[phase,dc*4+dd,kind+'_rx_bytes']+=size
 rows=[]
 for phase in ('dispatch','combine'):
  for rank in range(ranks):
   r=dict(phase=phase,chip=rank//4,die=rank%4)
   for field in ('fabric_tx_bytes','fabric_rx_bytes','d2d_tx_bytes','d2d_rx_bytes'):r[field]=totals[phase,rank,field]
   r['fabric_payload_average_GB_s']=r['fabric_tx_bytes']/window_us/1000
   r['scope']='completed application payload/window including compute; not wire utilization or peak bandwidth'
   rows.append(r)
 return rows
