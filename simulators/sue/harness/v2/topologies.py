"""SUE v2 physical port maps. Port group and VC are independent concepts."""
from collections import defaultdict, Counter
from dataclasses import dataclass, asdict
from itertools import combinations

@dataclass(frozen=True)
class Topology:
    name: str
    xpu: int
    mpu: int
    switches: int
    radix: int
    family: str

TOPOLOGIES = {t.name:t for t in [
    Topology('xpu8',8,0,1,256,'small'), Topology('xpu16',16,0,2,256,'small'),
    Topology('xpu32',32,0,4,256,'small'), Topology('xpu64',64,0,8,256,'small'),
    Topology('xpu128-parity',128,0,16,256,'parity'), Topology('xpu128-rotate',128,0,16,256,'rotate'),
    Topology('xpu256',256,0,32,256,'pure'), Topology('mixed6-8',6,8,1,256,'single'),
    Topology('mixed8-8',8,8,2,256,'two'), Topology('mixed32-32',32,32,8,256,'rail'),
    Topology('mixed64-64',64,64,16,256,'mixed'), Topology('mixed64-64-pool',64,64,16,256,'pool'),
    Topology('xpu18',18,0,1,576,'single'), Topology('mixed16-8',16,8,1,576,'single'),
    Topology('xpu144',144,0,8,576,'rail'), Topology('xpu576',576,0,32,576,'pure')
]}
PARTITIONS=(((0,1),(2,3)),((0,2),(1,3)),((0,3),(1,2)))
# Resource experiments keep eight physical rails constant across scale changes.
# These are explicit experimental port maps, separate from the original 16 modes.
BASE_TOPOLOGY_NAMES=tuple(TOPOLOGIES)
for n in (4,8,16,32):
    for m in (0,1,2,4,8,16):
        name=f'hw-{n}-{m}'
        TOPOLOGIES[name]=Topology(name,n,m,8,256,'rail')

def ports(t):
    rows=[]
    for x in range(t.xpu):
        for d in range(4):
            for i in range(8):
                f=t.family
                if f=='small': s,v,p=i%t.switches,i//t.switches,(i//t.switches)*t.xpu*4+4*x+d
                elif f=='single': s,v,p=0,i,32*x+8*d+i
                elif f=='two': s,v,p=i%2,i//2,(i//2)*40+4*x+d
                elif f=='rail': s,v,p=i,0,4*x+d
                elif f in ('parity','mixed','pool'): s,v,p=2*i+d%2,0,2*x+d//2
                elif f=='pure': s,v,p=8*d+i,0,x
                elif f=='rotate':
                    side=next(k for k in range(2) if d in PARTITIONS[i%3][k])
                    s,v,p=2*i+side,0,2*x+PARTITIONS[i%3][side].index(d)
                else: raise ValueError(f)
                rows.append(dict(chip=x,die=d,port=i,switch=s,vrf=v+1,physical=p))
    for j in range(t.mpu):
        for i in range(8):
            f=t.family
            if f=='single': s,v,p=0,i,32*t.xpu+8*j+i
            elif f=='two': s,v,p=i%2,i//2,(i//2)*40+32+j
            elif f=='rail': s,v,p=i,0,4*t.xpu+j
            elif f=='mixed': s,v,p=2*i+int(i>=4),0,128+j
            elif f=='pool': s,v,p=2*i+(t.xpu+j)%2,0,128+j//2
            else: raise ValueError(f)
            rows.append(dict(chip=t.xpu+j,die=0,port=i,switch=s,vrf=v+1,physical=p))
    return sorted(rows,key=lambda r:(r['switch'],r['physical']))

def route_key(t,chip,die):
    return chip if t.family=='pure' else 4*chip+die

def ring_distance(a,b): return min((a-b)%4,(b-a)%4)

def validate(t,rows):
    bysa=defaultdict(list); domains=defaultdict(set); physical=set(); bydi=defaultdict(set)
    local_vrf={}; loads=Counter(); keys=defaultdict(dict)
    for r in rows:
        c,d,i,s,v,p=(r[k] for k in ('chip','die','port','switch','vrf','physical'))
        assert 0<=s<t.switches and 0<=p<t.radix and (s,p) not in physical
        physical.add((s,p)); loads[s]+=1
        assert (c,d) not in domains[s,v]
        domains[s,v].add((c,d)); bysa[c,d].append((s,v))
        if c<t.xpu: bydi[d,i].add((s,v))
        k=route_key(t,c,d); assert 0<=k<1024
        assert k not in keys[s,v] or keys[s,v][k]==(c,d)
        keys[s,v][k]=(c,d)
        # Global port->VRF API: every switch must give an identical VRF to a
        # given dense device index; physical faceplate numbering stays explicit.
        idx=loads[s]-1
        assert idx not in local_vrf or local_vrf[idx]==v
        local_vrf[idx]=v
    assert len(rows)==t.xpu*32+t.mpu*8
    assert all(len(v)==8 and len(set(v))==8 for v in bysa.values())
    assert len(bysa)==t.xpu*4+t.mpu and len(bydi)==32
    assert all(len(v)==1 for v in bydi.values())
    assert len(loads)==t.switches and max(loads.values())<=t.radix
    return dict(status='passed',links=len(rows),domains=len(domains),sa_count=len(bysa),
                switch_loads=dict(loads),port_vrf=local_vrf,
                checks=['physical port uniqueness','capacity','SA uniqueness per domain','R1','R2','10-bit route-key collision check','global port VRF compatibility'])

def proxy(t,index,sc,sd,dc,dd,i):
    source=index[sc,sd,i]; dest=index[dc,dd,i]
    dom=lambda r:(r['switch'],r['vrf'])
    if dom(source)==dom(dest): return dd
    if sc>=t.xpu or dc>=t.xpu: return None
    candidates=[d for d in range(4) if dom(index[dc,d,i])==dom(source)]
    return min(candidates,key=lambda d:(ring_distance(d,dd),d)) if candidates else None

def flow_hash(seed,tid,sc,sd,dc,dd):
    h=2166136261
    for n in (seed,tid,sc,sd,dc,dd):
        for shift in (0,8,16,24): h=((h^((n>>shift)&255))*16777619)&0xffffffff
    # avalanche: avoid low-bit bias when a field differs by multiples of 4
    h^=h>>16; h=(h*0x7feb352d)&0xffffffff; h^=h>>15
    h=(h*0x846ca68b)&0xffffffff; return (h^(h>>16))&0xffffffff

def plan(t,rows,task,seed,index=None):
    idx=index if index is not None else {(r['chip'],r['die'],r['port']):r for r in rows}
    sc,sd,dc,dd=(task[k] for k in ('src_chip','src_die','dst_chip','dst_die'))
    if not task['bytes']: return dict(group=-1,port=-1,proxy_die=-1,hops=0,reachable=True)
    options=[proxy(t,idx,sc,sd,dc,dd,i) for i in range(8)]
    good=[g for g in (0,1) if all(p is not None for p in options[g*4:g*4+4])]
    group=task.get('group',-1)
    h=flow_hash(seed,task['id'],sc,sd,dc,dd)
    if group==-1: group=good[(h>>2)%len(good)] if good else (h>>2)%2
    i=4*group+h%4; pd=options[i]
    return dict(group=group,port=i,proxy_die=-1 if pd is None else pd,
                hops=0 if pd is None else ring_distance(pd,dd),reachable=pd is not None)
