"""Configuration-derived, synthetic single MoE-layer windows, not model traces."""
import json,random,hashlib
from pathlib import Path
from collections import Counter,defaultdict
INPUTS=Path(__file__).resolve().parent/'inputs'

def model_config(name):
    p=INPUTS/(name+'-config.json'); raw=json.loads(p.read_text()); c=raw.get('text_config',raw)
    if name=='kimi-k3': e,k,w=c['num_experts'],c['num_experts_per_token'],c['routed_expert_hidden_size']
    elif name in ('glm-5.3','deepseek-v4.1-flash-1017'): e,k,w=c['n_routed_experts'],c['num_experts_per_tok'],c['hidden_size']
    else: raise ValueError(name)
    return dict(model=name,experts=e,top_k=k,width=w,element_bytes=2,metadata_bytes=8,
                config_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),
                evidence='config-derived+synthetic',config_file=str(p),
                width_assumption='Kimi latent projection before dispatch; GLM hidden activation; BF16 communication for both, independent of weight format')

def task(sc,sd,dc,dd,n,delay=0,deps=(),label='probe',group=-1,vc=0):
    return dict(src_chip=sc,src_die=sd,dst_chip=dc,dst_die=dd,bytes=n,delay_ns=delay,
                deps=list(deps),label=label,group=group,vc=vc)

def build(t,model,window,seed=1,*,tokens=None,decode_steps=4,compute_ns=5000,profile="uniform",placement="spread"):
    assert profile in ("uniform","hotspot80","community80")
    assert placement in ("spread","affinity") and compute_ns>=0
    m=model_config(model); rng=random.Random(seed)
    count=tokens if tokens is not None else (128 if window=='prefill' else 64)
    assert count>0 and decode_steps>0
    steps=decode_steps if window=='decode' else 1
    stages=['forward_dispatch','forward_combine']+(['backward_dispatch','backward_combine'] if window=='train' else [])
    out=[]; routes=[]; previous={}; previous_incoming=defaultdict(list)
    def add(row):
        row['id']=len(out);out.append(row);return row['id']
    for step in range(steps):
        counts=Counter(); local=Counter()
        for token in range(count):
            src=(token*t.xpu//count,token%4)
            chosen=previous.get(token) if step and rng.random()<0.75 else None
            if chosen is None:
                if profile=='uniform': chosen=sorted(rng.sample(range(m['experts']),m['top_k']))
                else:
                    hot=[e for e in range(m['experts']) if (e<32 if profile=='hotspot80' else (e//2)%2==token%2)]
                    cold=[e for e in range(m['experts']) if e not in hot]
                    n=round(.8*m['top_k'])
                    chosen=sorted(rng.sample(hot,n)+rng.sample(cold,m['top_k']-n))
            previous[token]=chosen
            for expert in chosen:
                die=expert%4 if placement=='spread' else (0,2,1,3)[expert%4]
                dst=(expert*t.xpu//m['experts'],die)
                routes.append(dict(step=step,token=token,expert=expert,src_chip=src[0],src_die=src[1],dst_chip=dst[0],dst_die=dst[1],local=src==dst))
                if src==dst: local[src]+=1
                else: counts[src,dst]+=1
        record=m['width']*m['element_bytes']+m['metadata_bytes']
        for operation in stages:
            incoming=defaultdict(list); reverse=operation.endswith('combine')
            if reverse:
                # One fixed compute wait per expert endpoint, including local
                # experts; no GPU resource queue or shared-expert compute model.
                participants={dst for src,dst in counts}|set(local)
                for dst in sorted(participants):
                    wait=add(task(*dst,*dst,0,compute_ns,sorted(set(previous_incoming[dst])),f'{step}:{operation}:compute'))
                    incoming[dst]=[wait]
                previous_incoming=incoming;incoming=defaultdict(list)
            for (src,dst),records in sorted(counts.items()):
                if reverse: src,dst=dst,src
                deps=sorted(set(previous_incoming[src]))
                tid=add(task(*src,*dst,records*record,1000 if not deps else 0,deps,f'{step}:{operation}'))
                incoming[dst].append(tid)
            # Keep completed local computations as causal input where relevant.
            for sa in local: incoming[sa].extend(previous_incoming[sa])
            previous_incoming=incoming
    remote=sum(not r['local'] for r in routes)
    assert sum(x['bytes'] for x in out)==remote*(m['width']*2+8)*len(stages)
    return out,routes,dict(**m,window=window,steps=steps,global_tokens_per_step=count,
                          seed=seed,retain_probability=0.75 if window=='decode' else 0,
                          expert_compute_wait_ns=compute_ns,local_assignments=len(routes)-remote,
                          remote_assignments=remote,logical_bytes=sum(x['bytes'] for x in out),
                          placement='source chip=floor(token*N_XPU/token_count), source die=token%4; expert chip=floor(expert*N_XPU/experts), expert die='+('expert%4' if placement=='spread' else '(0,2,1,3)[expert%4]'),
                          routing_profile=profile,die_placement=placement,
                          scope='one representative MoE layer; prefill chunk / four decode steps / forward+backward training window; no TP/PP/DP/attention or optimizer traffic; MPU unused by model workload')
