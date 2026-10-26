import copy
import csv
import json
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import sue_harness as h

class ValidationTests(unittest.TestCase):
    def test_cycle_and_unknown_dependency(self):
        s=copy.deepcopy(h.DEFAULT); ts=h.generate(s); ts[0]['deps']=[2]; ts[2]['deps']=[0]
        with self.assertRaisesRegex(ValueError,'cycle'): h.validate_tasks(s,ts)
        ts[0]['deps']=[999]
        with self.assertRaises(ValueError): h.validate_tasks(s,ts)

    def test_invalid_credit_queue_and_causality(self):
        for group,key,val in [('network','credits',1),('runtime','queue_bytes',64),('runtime','visibility_ns',1),('network','rate_gbps',True)]:
            with self.subTest(key=key):
                s=copy.deepcopy(h.DEFAULT); s[group][key]=val
                with self.assertRaises(ValueError): h.validate_spec(s)
        s=copy.deepcopy(h.DEFAULT); s['network']['UB_JETTY']=1
        with self.assertRaises(ValueError): h.validate_spec(s)

    def test_duplicate_deps_and_local_semantics(self):
        s=copy.deepcopy(h.DEFAULT); ts=h.generate(s); ts[3]['deps']=[0,0]
        with self.assertRaisesRegex(ValueError,'duplicate'): h.validate_tasks(s,ts)
        ts=h.generate(s); ts[0]['dst']=0
        with self.assertRaises(ValueError): h.validate_tasks(s,ts)

class BackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='sue-harness-tests-'); cls.root=Path(cls.tmp.name)
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def run_spec(self,name,s,tasks=None):
        case=self.root/name; h.create(case,s,tasks)
        ok=h.run_case(case); run=Path(json.loads((case/'latest.json').read_text())['run'])
        return ok,run,json.loads((run/'summary.json').read_text())

    def test_fragment_bytes_fanin_and_mtp(self):
        s=copy.deepcopy(h.DEFAULT); s['workload']['bytes_per_peer']=9999
        ts=h.generate(s)
        # A final fan-in barrier must wait for every combine completion.
        ts.append(dict(id=len(ts),src=0,dst=0,bytes=0,delay_ns=777,vc=0,deps=[4,6,8],label='barrier'))
        ok,a,summary=self.run_spec('fanin-1',s,ts); self.assertTrue(ok)
        s['runtime']['threads']=8
        ok,b,_=self.run_spec('fanin-8',s,ts); self.assertTrue(ok)
        # Equal-timestamp event order can change per-task timing; compare delivery and fan-in completion.
        self.assertEqual(summary['received_bytes'],json.loads((b/'summary.json').read_text())['received_bytes'])
        self.assertEqual(summary['makespan_us'],json.loads((b/'summary.json').read_text())['makespan_us'])
        self.assertEqual(summary['received_bytes'],9999*6)
        rows=list(csv.DictReader(io.StringIO((a/'task_statistics.csv').read_text())))
        for dep in (4,6,8): self.assertGreaterEqual(int(rows[9]['end_ps']),int(rows[dep]['end_ps'])+877000)
        # Report is portable and embedded JSON round-trips malicious-looking labels.
        ts[0]['label']='</script><img src=x onerror=alert(1)>'
        from report import render
        render(a,s,ts,rows,[],summary)
        text=(a/'report.html').read_text(); self.assertNotIn('</script><img',text); self.assertNotIn('cdn.',text)

    def test_llr_recovery(self):
        s=copy.deepcopy(h.DEFAULT); s['network']['bad_every']=10
        ok,_,summary=self.run_spec('loss',s); self.assertTrue(ok); self.assertGreater(summary['replay_frames'],0); self.assertEqual(summary['discard'],0)

    def test_credit_backpressure_and_bounded_injection(self):
        s=copy.deepcopy(h.DEFAULT); s['workload']['bytes_per_peer']=262144; s['network']['credits']=32; s['runtime']['interval_ns']=1
        ok,_,summary=self.run_spec('incast',s); self.assertTrue(ok)
        self.assertGreater(summary['credit_exhausted_checks'],0); self.assertGreater(summary['queue_blocked_polls'],0)

    def test_short_deadline_is_failure_and_reruns_preserve_history(self):
        s=copy.deepcopy(h.DEFAULT); s['runtime']['stop_ns']=11000
        ok,run,summary=self.run_spec('incomplete',s); self.assertFalse(ok); self.assertLess(summary['completed'],summary['tasks'])
        self.assertTrue((run/'report.html').exists())
        before=(run/'execution.json').read_bytes(); h.run_case(run.parents[1]); self.assertEqual(before,(run/'execution.json').read_bytes())
        self.assertEqual(len((run.parents[1]/'run-ledger.jsonl').read_text().splitlines()),2)

    def test_timeout_is_recorded(self):
        s=copy.deepcopy(h.DEFAULT)
        case=self.root/'timeout'; h.create(case,s)
        with patch('sue_harness.subprocess.run',side_effect=subprocess.TimeoutExpired('backend',1)):
            self.assertFalse(h.run_case(case))
        run=Path(json.loads((case/'latest.json').read_text())['run'])
        self.assertEqual(json.loads((run/'execution.json').read_text())['returncode'],124)
        self.assertTrue((run/'report.html').exists())

    def test_flow_control_modes_and_leaf_spine_ring(self):
        for mode in ('CBFC','PFC','OFF'):
            with self.subTest(mode=mode):
                s=copy.deepcopy(h.DEFAULT); s['workload']['kind']='ring'; s['topology']['kind']='leaf_spine'; s['network']['fc']=mode
                s['network']['llr']=False
                ok,_,summary=self.run_spec('ring-'+mode,s); self.assertTrue(ok); self.assertEqual(summary['received_bytes'],4*16384)

    def test_sweep_report_and_failed_case_ledger(self):
        s=copy.deepcopy(h.DEFAULT); s['runtime']['stop_ns']=11000
        base=self.root/'sweep-base'; out=self.root/'sweep-results'; h.create(base,s)
        with patch.object(sys,'argv',['sue_harness.py','sweep',str(base),str(out),'--field','network.credits','--values','32,64','--prediction','short deadline leaves incomplete tasks']):
            self.assertEqual(h.main(),1)
        ledger=json.loads((out/'run-ledger.json').read_text())
        self.assertEqual(len(ledger),2); self.assertTrue(all(not r['passed'] for r in ledger))
        self.assertTrue((out/'comparison.csv').exists()); self.assertIn('failed',(out/'report.html').read_text())

    def test_ub_import_phase_and_rejected_operation(self):
        source=self.root/'ub.csv'
        source.write_text('taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n10,0,1,9999,URMA_WRITE,3,10us,8,\n11,1,2,200,URMA_WRITE,3,2us,9,8\n')
        dest=self.root/'import'; args=type('Args',(),dict(source=str(source),case=str(dest),name='import',max_tasks=0))()
        h.import_ub(args); s,ts,_=h.read_case(dest)
        self.assertEqual(ts[1]['deps'],[0]); self.assertEqual(ts[1]['delay_ns'],2000); self.assertTrue(h.run_case(dest))
        source.write_text(source.read_text().replace('URMA_WRITE','URMA_READ'))
        args.case=str(self.root/'bad-import')
        with self.assertRaisesRegex(ValueError,'unsupported UB'): h.import_ub(args)

if __name__=='__main__': unittest.main()
