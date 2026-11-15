"""One-sequence speculative-cycle scenarios; actual SUE packets, assumed draft proxy.
No model probabilities or accepted tokens are executed. Acceptance is analytic.
"""
import argparse,csv,json,math,pathlib,shutil,subprocess,os
import numpy as np
import run

def main():
 p=argparse.ArgumentParser();p.add_argument('--config',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args();cfg=a.config.resolve();c,M=run.load(cfg);out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 run.dump(out/'experiment.json',c);(out/'source_snapshot').mkdir()
 for name in ('mtp_run.py','model.py','run.py','engine.cc','sue_backend.h'):shutil.copy2(run.HERE/name,out/'source_snapshot'/name)
 run.build('sue');(out/'bin').mkdir();shutil.copy2(run.BIN,out/'bin/engine');exe=out/'bin/engine'
 run.dump(out/'implementation.json',{'inputs':{str(f):run.sha(f) for f in (cfg,cfg.parent/c['model_config'],cfg.parent/c['weight_inventory'])},'binary_sha256':run.sha(exe),'sources':{f.name:run.sha(f) for f in (out/'source_snapshot').iterdir()}})
 rows=[];rng=np.random.default_rng(c['workload']['seed']);S=2;maxpos=max(c['mtp']['draft_depths'])+1
 # Per-position independent synthetic expert choices; all depths share the prefix.
 routes=np.zeros((S,93,maxpos,16),dtype=np.uint16)
 for st in range(S):
  for l in range(1,93):
   for pos in range(maxpos):routes[st,l,pos]=rng.choice(896,16,replace=False)
 np.save(out/'routes.npy',routes)
 for depth in [0]+c['mtp']['draft_depths']:
  positions=depth+1;tiles=np.zeros((S,93,1))
  for st in range(S):
   for l in range(1,93):tiles[st,l,0]=np.sum(np.ceil(np.bincount(routes[st,l,:positions].ravel(),minlength=896)/32))
  verify,metrics,payload=run.plan_costs(M,positions,8,'TP',tiles,[0,93]);vcount=verify.shape[2]
  # One shared sequence, conservatively charge each position's state and KV read.
  # Context for all positions is raised to the final position (upper budget).
  if depth:
   import copy
   cc=copy.deepcopy(c);cc['workload']['initial_context']+=depth
   MM=run.Model(cc,cfg.parent);verify,metrics,payload=run.plan_costs(MM,positions,8,'TP',tiles,[0,93])
  dlayer=M.layer(c['mtp']['draft_proxy_layer_zero_based'],1,32,np.full((S,1),16.0))
  draft=dlayer[:,:,:3,0].copy();draft[:,:,0]+=M.endpoints(1)[0][0]
  # 3H -> H BF16 feature-fusion projection, replicated as an explicit assumption.
  fusion=M.op(6*M.H*M.H+8*M.H,6*M.H*M.H)[0]
  draft[:,:,0]+=fusion
  tail=M.up(1)[0]+M.endpoints(1)[1][0]
  draft=np.concatenate([draft,np.full((S,1,1),tail)],axis=2)
  dpayload=[M.H*2,M.H*2,M.hf['routed_expert_hidden_size']*2,0]
  # Conservative rollback budget: snapshot once, then replay all draft positions.
  heads=M.hf['linear_attn_config']['num_heads']/32;hd=M.hf['linear_attn_config']['head_dim'];state=heads*hd*hd*4;conv=3*heads*hd*4*2
  recovery_by=69*(2*(state+conv)+2*depth*(state+conv));recovery_fl=69*depth*(6*heads*hd*hd+6*heads*hd*4)
  recovery=float(M.op(recovery_by,recovery_fl)[0]) if depth else 0
  for mult in ([1] if depth==0 else c['mtp']['draft_cost_multipliers']):
   ident=f'd{depth}-draftx{mult}';d=out/ident;d.mkdir()
   parts=[draft*mult]*depth+[verify,np.full((S,1,1),recovery)];cost=np.concatenate(parts,axis=2);packets=dpayload*depth+payload+[0]
   (cost*1e12).astype('<f8').tofile(d/'service.bin');nseg=cost.shape[2]
   (d/'input.txt').write_text(f'TP 8 1 1 2 {nseg} 0 128000000000 2000000 1000 {d}/service.bin\n'+' '.join(map(str,packets))+'\nsingle_switch\n')
   # Admission includes one extra draft block, fusion, snapshot, and verify workspace.
   cap=M.capacity(8,1,'TP',[0,93]);extra=4*(M.weight(3,32)+6*M.H*M.H+69*2*(state+conv)+positions*M.H*16) if depth else 0
   assert max(x['total_bytes'] for x in cap)+extra<=M.h['memory_bytes']
   with (d/'stdout.log').open('w') as so,(d/'stderr.log').open('w') as se:subprocess.run([str(exe),str(d/'input.txt'),str(d/'engine')],check=True,stdout=so,stderr=se,timeout=300,env={**os.environ,**{'HM_SUE_'+k.upper():str(v) for k,v in c['modeling']['sue_profile'].items()}})
   x=json.loads((d/'engine.json').read_text());ports=list(csv.DictReader((d/'engine-sue-ports.csv').open()));assert sum(int(t['discard']) for t in ports)==0;assert x['finished_tokens']==2
   expected_ar=(278+3*depth)*14*2;assert x['ar_steps']==expected_ar
   assert sum(x['chip_tx_bytes'])==sum(x['chip_rx_bytes'])
   r={'id':ident,'path':str(d),'depth':depth,'draft_multiplier':mult,'request_batch':1,'verification_positions':positions,'cycle_ms':x['mean_tpot_ps']/1e9,'draft_local_ms':float(depth*mult*draft[1].sum()*1000),'verify_local_ms':float(verify[1].sum()*1000),'recovery_ms':recovery*1000,'capacity_max_GB':(max(x['total_bytes'] for x in cap)+extra)/1e9,'backend':'sue','status':'completed','service_sha256':run.sha(d/'service.bin'),'data_frame_link_transmissions':sum(int(t['tx_ok']) for t in ports),'replay_frames':sum(int(t['replay_frames']) for t in ports)}
   r['network_ms']=r['cycle_ms']-r['draft_local_ms']-r['verify_local_ms']-r['recovery_ms'];rows.append(r);run.dump(d/'result.json',r);print(ident,r['cycle_ms'],flush=True)
 baseline=rows[0]['cycle_ms'];est=[]
 for r in rows[1:]:
  for alpha in c['mtp']['conditional_acceptance']:
   tokens=sum(alpha**i for i in range(r['depth']+1));ms=r['cycle_ms']/tokens
   est.append({**r,'alpha':alpha,'expected_committed_tokens':tokens,'amortized_ms_per_token':ms,'speedup':baseline/ms,'expected_token_s':1000/ms})
 run.dump(out/'results.json',rows);run.dump(out/'estimates.json',est);run.dump(out/'verification.json',{'status':'passed','completed':len(rows),'failed':0,'packet_backend':'sue','one_request_per_cycle':True,'acceptance_is_assumption':True,'expected_ar_steps_checked':True,'payload_conservation':True,'zero_discard':True,'baseline_cycle_ms':baseline})
 with (out/'estimates.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(est[0]));w.writeheader();w.writerows(est)
if __name__=='__main__':main()
