"""Offline recorder/driver fault injection. No device, socket, or live files."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import shutil
import unittest
from bx_fixtures import packet, tool

ROOT=Path(__file__).resolve().parents[1]
class LifecycleTests(unittest.TestCase):
    def case(self,mode='normal',fault=''):
        temp=tempfile.TemporaryDirectory(prefix='ndi-lifecycle-fault-');self.addCleanup(temp.cleanup)
        parent=Path(temp.name);frame=parent/'synthetic.bin';frame.write_bytes(packet([tool(markers={0:(3,[9,8,7],0)})]))
        env=dict(os.environ,LD_PRELOAD=str(ROOT/'bin/fault_shim.so'),LD_LIBRARY_PATH=str(ROOT/'bin'),NDI_TEST_STORAGE_FAULT=fault)
        result=subprocess.run([str(ROOT/'bin/recorder_lifecycle_fixture'),str(parent),mode,str(frame)],env=env,capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr)
        value=json.loads(next(line for line in result.stdout.splitlines() if line.startswith('{')))
        evidence=ROOT/'audit/lifecycle-cases.jsonl'
        with evidence.open('a') as f:f.write(json.dumps(dict(mode=mode,fault=fault,result=value,source='SYNTHETIC_ONLY'))+'\n')
        samples=ROOT/'audit/regression-summaries';samples.mkdir(exist_ok=True)
        for session in parent.glob('ndi-marker-*'):
            if (session/'summary.json').exists():shutil.copy2(session/'summary.json',samples/(str(time.time_ns())+'-'+mode+'-'+(fault or 'none')+'.json'))
        return value,parent,result.stderr
    def counters(self,summary):
        c=summary['counters']
        self.assertEqual(c['attempted'],sum(c[k] for k in ('enqueued','dropped_queue_full','dropped_queue_busy','dropped_allocation','rejected_not_accepting')))
        self.assertEqual(c['enqueued'],c['written']+c['discarded_after_write_failure'])
        self.assertTrue(summary['counter_conservation'])
    def summary(self,value):return json.loads((Path(value['path'])/'summary.json').read_text())
    def test_invalid_start_is_not_reported_as_never_started_success(self):
        v,_,_=self.case('validation_failure');self.assertEqual((v['started'],v['state'],v['stop']),(0,'startup_failed',0))
    def test_complete_and_never_started_are_distinct_and_sticky(self):
        v,_,_=self.case();self.assertEqual((v['virgin'],v['virgin_stop']),('never_started',1))
        self.assertEqual((v['state'],v['stop'],v['again']),('finalized_complete',1,1))
        self.assertFalse(v['post_seal']);self.assertEqual(v['outside_session'],1);self.assertTrue(v['same_summary']);self.assertEqual(v['fds_before'],v['fds_after'])
        s=self.summary(v);self.assertTrue(s['storage_complete']);self.counters(s)
    def test_queue_loss_remains_false_on_repeated_stop(self):
        v,_,_=self.case('drop');self.assertEqual((v['stop'],v['again']),(0,0))
        self.assertEqual(v['state'],'finalized_incomplete');self.assertTrue(v['same_summary'])
        s=self.summary(v);self.assertFalse(s['storage_complete']);self.counters(s)
        self.assertGreater(s['counters']['dropped_queue_full']+s['counters']['dropped_queue_busy'],0)
    def test_stop_seals_inflight_producers_and_conserves_counters(self):
        for _ in range(5):
            v,_,_=self.case('race_stop');self.assertEqual(v['stop'],v['again'])
            self.assertFalse(v['post_seal']);self.assertTrue(v['same_summary']);self.counters(self.summary(v))
    def test_data_write_fsync_close_failures_sticky_without_fd_leak(self):
        for fault in ('measurement_write','data_fsync','data_close'):
            with self.subTest(fault=fault):
                v,_,err=self.case(fault=fault);self.assertEqual((v['stop'],v['again']),(0,0))
                s=self.summary(v);self.assertFalse(s['storage_complete']);self.assertTrue(s['error']);self.counters(s)
                self.assertEqual(v['fds_before'],v['fds_after']);self.assertIn('NDI_MARKER_SIDECAR_ERROR',err)
    def test_summary_failure_does_not_publish_authoritative_success(self):
        for fault in ('summary_write','summary_fsync','summary_close','summary_link'):
            with self.subTest(fault=fault):
                v,_,_=self.case(fault=fault);self.assertEqual((v['stop'],v['again']),(0,0))
                self.assertEqual(v['state'],'finalized_incomplete');self.assertEqual(v['fds_before'],v['fds_after'])
                self.assertFalse((Path(v['path'])/'summary.json').exists())
                self.assertTrue((Path(v['path'])/'summary.pending.json').exists())
    def test_header_and_thread_failures_preserve_failed_session_and_allow_retry(self):
        for fault in ('header_write','thread_create'):
            with self.subTest(fault=fault):
                v,_,_=self.case('start_failure',fault)
                self.assertEqual((v['started'],v['state'],v['stop'],v['stop_again']),(0,'startup_failed',0,0))
                self.assertEqual((v['retry'],v['retry_stop']),(1,1));self.assertNotEqual(v['first'],v['second'])
                self.assertTrue(Path(v['first']).is_dir());self.assertFalse(json.loads((Path(v['first'])/'summary.json').read_text())['storage_complete'])
                self.assertTrue(json.loads((Path(v['second'])/'summary.json').read_text())['storage_complete'])
                self.assertEqual(v['fds_before'],v['fds_failed']);self.assertEqual(v['fds_before'],v['fds_after'])
    def test_driver_queue_loss_is_structured_failure(self):
        v,_,_=self.case('driver_drop')
        self.assertEqual(v['state'],'finalized_incomplete')
        for key in ('internal','internal_disconnect','stop','again','disconnect','disconnect_again'):self.assertEqual(v[key],0)
        self.counters(self.summary(v))
    def test_driver_public_stop_disconnect_and_internal_disconnect_retain_archive_failure(self):
        for fault in ('measurement_write','data_fsync','data_close','summary_write','summary_fsync','summary_close','summary_link'):
            with self.subTest(fault=fault):
                v,_,_=self.case('driver',fault)
                for key in ('internal','internal_disconnect','stop','again','disconnect','disconnect_again'):self.assertEqual(v[key],0,(fault,key,v))
                self.assertEqual(v['state'],'finalized_incomplete')

if __name__=='__main__':unittest.main(verbosity=2)
