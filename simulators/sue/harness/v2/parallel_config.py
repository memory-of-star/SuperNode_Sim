"""Explicit contiguous TP/EP groups and deterministic placement policies."""
import math
DEFAULT=dict(schema_version=1,name='default',tp=4,dp=None,ep=None,expert_tp=1,pp=1,
 moe_transport='tp-striped',expert_placement='round_robin',token_assignment='round_robin',decode_expert_reuse_probability=.75)

def resolve_parallel(chips,values=None,**overrides):
 values={} if values is None else values
 if not isinstance(values,dict) or set(values)-set(DEFAULT):raise ValueError('unknown parallel configuration fields')
 p={**DEFAULT,**values,**{k:v for k,v in overrides.items() if v is not None}}
 if set(p)-set(DEFAULT):raise ValueError('unknown parallel override')
 if type(chips) is not int or chips<1:raise ValueError('positive chip count required')
 ranks=chips*4
 if type(p['schema_version']) is not int or p['schema_version']!=1:raise ValueError('parallel schema_version must be 1')
 if not isinstance(p['name'],str):raise ValueError('parallel name must be a string')
 for k in ('tp','expert_tp','pp'):
  if type(p[k]) is not int or p[k]<1:raise ValueError(k+' must be a positive integer')
 if p['expert_tp']!=1 or p['pp']!=1:raise ValueError('expert_tp and pp currently support only 1; not silently approximated')
 if ranks%p['tp']:raise ValueError('TP must divide chips*4')
 if p['dp'] is None:p['dp']=ranks//p['tp']
 if p['ep'] is None:p['ep']=ranks
 for k in ('dp','ep'):
  if type(p[k]) is not int or p[k]<1:raise ValueError(k+' must be a positive integer or null')
 if p['tp']*p['dp']!=ranks:raise ValueError('TP*DP must equal chips*4 for the common component')
 if ranks%p['ep'] or p['ep']%p['tp']:raise ValueError('EP must divide chips*4 and be a multiple of TP; TP groups cannot cross EP replica boundaries')
 for k,allowed in [('moe_transport',('tp-striped','leader')),('expert_placement',('round_robin','contiguous')),('token_assignment',('round_robin','contiguous'))]:
  if p[k] not in allowed:raise ValueError(k+': expected '+str(allowed))
 v=p['decode_expert_reuse_probability']
 if type(v) not in (int,float) or not math.isfinite(v) or not 0<=v<=1:raise ValueError('decode_expert_reuse_probability must be in [0,1]')
 return p

def from_definition(d):
 return resolve_parallel(d['chips'],d.get('parallel'),tp=d.get('tp'),moe_transport=d.get('moe_transport'))

def token_group(token,batch,groups,policy):
 if policy=='round_robin':return token%groups
 # Balanced contiguous intervals; first remainder groups own one extra token.
 q,r=divmod(batch,groups);cut=(q+1)*r
 return token//(q+1) if token<cut else r+(token-cut)//q

def expert_offset(expert,ep,experts,placement):
 return expert%ep if placement=='round_robin' else min(expert//math.ceil(experts/ep),ep-1)

def expert_owner(expert,source_rank,p,experts):
 return (source_rank//p['ep'])*p['ep']+expert_offset(expert,p['ep'],experts,p['expert_placement'])

def layout_map(chips,p,experts):
 ranks=chips*4
 return dict(world_size=ranks,rank_formula='rank = chip*4 + die',expert_replica_groups=ranks//p['ep'],
  tp_groups=[list(range(i,i+p['tp'])) for i in range(0,ranks,p['tp'])],
  ep_groups=[list(range(i,i+p['ep'])) for i in range(0,ranks,p['ep'])],
  ranks=[dict(rank=r,chip=r//4,die=r%4,tp_group=r//p['tp'],ep_group=r//p['ep'],experts=[e for e in range(experts) if expert_offset(e,p['ep'],experts,p['expert_placement'])==r%p['ep']]) for r in range(ranks)])
