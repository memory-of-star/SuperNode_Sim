import json, pathlib, subprocess, tempfile, unittest
import numpy as np
import run

class EngineTests(unittest.TestCase):
 def sim(self,mode,n,B,m,S,duplex,payload,cost):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);np.asarray(cost,dtype='<f8').tofile(p/'cost.bin')
   (p/'input.txt').write_text(f'{mode} {n} {B} {m} {S} {len(payload)} {duplex} 128000000000 2000000 1000 {p}/cost.bin\n'+ ' '.join(map(str,payload))+'\nsingle_switch\n')
   subprocess.run([str(run.BIN),str(p/'input.txt'),str(p/'out')],check=True)
   return json.loads((p/'out.json').read_text())
 def test_allreduce_step_formula(self):
  for n in (2,4,8):
   for duplex in (0,1):
    for M in (14336,14680064):
     x=self.sim('TP',n,1,1,2,duplex,[M],np.zeros((2,1,1)))
     expected=2*(n-1)*2001000+(2 if duplex else 4)*(n-1)*M/n/128e9*1e12
     self.assertLess(abs(x['makespan_ps']/2-expected),32)
     self.assertEqual(x['ar_steps'],4*(n-1))
     self.assertAlmostEqual(x['chip_tx_bytes'][0],4*(n-1)*M/n,places=3)
 def test_pp_dependency_and_pipeline(self):
  a=self.sim('PP',2,1,1,4,0,[100,4],np.full((4,1,2),1e8))
  b=self.sim('PP',2,2,1,4,0,[100,4],np.full((4,2,2),1e8))
  self.assertGreater(a['mean_tpot_ps'],2e8)
  self.assertLess(b['makespan_ps'],2*a['makespan_ps'])
  self.assertEqual(b['finished_tokens'],8)
 def test_weights_capacity_and_partition(self):
  c,m=run.load(run.ROOT/'configs/experiments/hm-emu/study.json')
  expected=sum(4*m.weight(l,4) for l in range(93))+m.embed+m.head+m.other
  # Replicated scalar/projection weights explain difference from checkpoint bytes.
  self.assertGreater(expected,m.inv['checkpoint_bytes'])
  self.assertFalse(m.rep('self_attn.q_b_proj.weight'));self.assertTrue(m.rep('self_attn.q_a_proj.weight'))
  for n in (2,4,8):
   b,_=m.partition(n);self.assertEqual((b[0],b[-1]),(0,93));self.assertEqual(len(b),n+1)
  self.assertGreater(max(x['total_bytes'] for x in m.capacity(2,1024,'TP',[0,47,93])),1e12)
 def test_expert_reuse_and_d2d(self):
  c,m=run.load(run.ROOT/'configs/experiments/hm-emu/study.json')
  a=m.layer(1,2,4,np.array([[16]]),steps=1);b=m.layer(1,2,4,np.array([[32]]),steps=1)
  self.assertLess(a[0,0,2,4],b[0,0,2,4]);self.assertEqual(a[0,0,2,5],b[0,0,2,5])
  a=m.layer(1,32,4,np.array([[256]],dtype=np.uint16),steps=1);b=m.layer(1,32,4,np.array([[512]],dtype=np.uint16),steps=1)
  self.assertAlmostEqual(b[0,0,2,4]-a[0,0,2,4],256*m.expert/4)
  M=1048576;self.assertAlmostEqual(m.d2d(M),6*(11e-9+M/4/16e9))
if __name__=='__main__':unittest.main()
