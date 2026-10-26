"""Resource FIFO, independence, ring and MPU causality acceptance checks."""
import json,sys,math
from pathlib import Path
from run import create,run_case,csvread,dump
from workloads import task

def main(dest):
 dest=Path(dest);dest.mkdir(parents=True,exist_ok=False);cases=[]
 def run(name,ts,rs):
  c=create(dest/name,'hw-4-2',ts,{'model':'resource-probe','window':name},settings={'ring_gbps':2000,'ring_bytes':1048576,'stop_ns':10000000},resources=rs)
  out=run_case(c);assert out['status']=='passed',out;cases.append(out);return csvread(Path(out['run'])/'task_statistics.csv'),csvread(Path(out['run'])/'resource_statistics.csv')
 def r(i,k,ns,units=1,peer=-1):return dict(id=i,kind=k,service_ns=ns,units=units,peer_die=peer)
 ts=[task(0,0,0,0,0,delay=1000) for _ in range(3)]
 stats,rs=run('fifo-and-independent-resources',ts,[r(0,0,1000),r(1,0,1000),r(2,1,1000)])
 assert [int(x['end_ps']) for x in stats]==[2000000,3000000,2000000]
 assert [int(x['queued_ps']) for x in rs]==[0,1000000,0]
 ts=[task(0,0,0,0,0,delay=1000),task(0,1,0,1,0,delay=1000)]
 stats,_=run('independent-dies',ts,[r(0,0,1000),r(1,0,1000)])
 assert int(stats[0]['end_ps'])==int(stats[1]['end_ps'])==2000000
 ts=[task(0,0,0,0,0,delay=1000),task(0,0,0,0,0,delay=1000)]
 stats,rs=run('local-ring',ts,[r(0,3,0,65536,1),r(1,3,0,65536,2)])
 assert all(int(x['local_received_bytes'])==65536 for x in rs)
 assert all(int(x['port'])==-1 for x in stats)
 ts=[task(0,0,4,0,64,delay=1000),task(4,0,4,0,0,deps=[0]),task(4,0,0,0,65536,deps=[1])]
 stats,_=run('remote-read',ts,[r(1,2,math.ceil(65536/190e9*1e9),65536)])
 assert int(stats[2]['start_ps'])>=int(stats[1]['end_ps'])+20000
 # Shared MPU bandwidth: independent requests serialize even from different sources.
 ts=[task(4,0,4,0,0,delay=1000),task(4,0,4,0,0,delay=1000)]
 stats,rs=run('mpu-serialization',ts,[r(0,2,1000),r(1,2,1000)])
 assert int(rs[1]['queued_ps'])==1000000
 dump(dest/'verification.json',{'status':'passed','cases':len(cases),'results':cases,'checks':['same resource FIFO','compute/HBM independent','Die resource independence','local ring byte delivery without fabric','MPU service precedes data response','shared MPU queue']})
 print('Resource acceptance PASS',len(cases))
if __name__=='__main__':main(sys.argv[1])
