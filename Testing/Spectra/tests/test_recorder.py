"""Synthetic recorder/state/durability checks, independent from hardware capture."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
import unittest
from bx_fixtures import packet,tool

ROOT=Path(__file__).resolve().parents[1]
PROGRAM=ROOT/'bin/recorder_fixture'
class RecorderTests(unittest.TestCase):
    def run_case(self,frames,mode='normal',names=None):
        temporary=tempfile.TemporaryDirectory(prefix='ndi-marker-recorder-')
        self.addCleanup(temporary.cleanup);parent=Path(temporary.name)
        files=[]
        for i,frame in enumerate(frames):
            p=parent/(names[i] if names else f'frame-{i}.bin');p.write_bytes(frame);files.append(p)
        p=subprocess.run([str(PROGRAM),str(parent),mode,*map(str,files)],capture_output=True,text=True,timeout=20)
        session=Path(p.stdout.splitlines()[0])
        start=None;rows=[];partial=False
        for line in (session/'measurements.jsonl').read_text().splitlines():
            try:r=json.loads(line)
            except json.JSONDecodeError:partial=True;continue
            if r['record_type']=='session_start':start=r
            if r['record_type']=='measurement_reply':rows.append(r)
        summary=json.loads((session/'summary.json').read_text())
        return p,start,rows,summary,partial
    def test_missing_point_never_reuses_previous_xyz(self):
        present=packet([tool(markers={0:(3,[7,8,9],0)})])
        absent=packet([tool(frame=124,status=2)])
        p,start,rows,summary,_=self.run_case([present,absent])
        self.assertEqual(p.returncode,0)
        self.assertEqual(start['source_provenance'],'synthetic_fixture')
        self.assertEqual(rows[0]['tools'][0]['markers'][0]['xyz_mm'],[7,8,9])
        self.assertIsNone(rows[1]['tools'][0]['markers'][0]['xyz_mm'])
        self.assertEqual(rows[1]['tools'][0]['markers'][0]['state'],'missing_or_not_defined')
        self.assertIsNone(rows[1]['tools'][0]['pose'])
        self.assertTrue(summary['storage_complete'])
    def test_status_metadata_and_per_tool_frames(self):
        p,start,rows,summary,_=self.run_case([packet([tool(frame=20),tool(2,21,markers={2:(4,[1,2,3],1)},port=0x71)])])
        self.assertEqual(p.returncode,0)
        self.assertEqual([t['device_frame_number'] for t in rows[0]['tools']],[20,21])
        m=rows[0]['tools'][1]['markers'][2]
        self.assertEqual(m['xyz_mm'],[1,2,3]);self.assertFalse(m['sample_valid']);self.assertTrue(m['out_of_volume'])
        self.assertIsNone(rows[0]['raw_measurement_timestamp'])
        self.assertEqual(rows[0]['reply_flags'],0x080b)
    def test_duplicate_wrap_reset_rebind_and_reconnect(self):
        frames=[packet([tool(frame=f,handle=h)]) for f,h in [
            (0xfffffffe,1),(0xffffffff,1),(0,1),(0,1),(2,1),(1,1),(1,2),(1,2)]]
        names=[f'frame-{i}.bin' for i in range(len(frames))];names[-1]='epoch2-frame.bin'
        p,start,rows,summary,_=self.run_case(frames,names=names)
        tools=[r['tools'][0] for r in rows]
        self.assertEqual([t['frame_classification'] for t in tools],
            ['first_frame','new_frame','counter_wrap','repeated_frame','new_frame',
             'counter_reset_or_out_of_order','first_frame','first_frame'])
        self.assertEqual(rows[-1]['connection_epoch'],2)
        self.assertNotEqual(tools[5]['binding_epoch'],tools[6]['binding_epoch'])
        self.assertIsNone(tools[5]['is_new_device_frame'])
        self.assertFalse(tools[3]['is_new_device_frame'])
        self.assertEqual(summary['counters']['counter_wraps'],1)
    def test_duplicate_with_changed_payload_is_marked(self):
        a=packet([tool(frame=8,markers={0:(3,[1,2,3],0)})])
        b=packet([tool(frame=8,markers={0:(3,[9,8,7],0)})])
        p,_,rows,_,_=self.run_case([a,b])
        t=rows[1]['tools'][0]
        self.assertEqual(t['frame_classification'],'repeated_frame');self.assertTrue(t['repeated_frame_payload_changed'])
    def test_crc_error_has_raw_bytes_but_no_old_coordinates(self):
        valid=packet([tool(markers={0:(3,[7,8,9],0)})]);bad=bytearray(valid);bad[-1]^=1
        p,_,rows,_,_=self.run_case([valid,bytes(bad)])
        self.assertFalse(rows[1]['reply_valid'])
        self.assertEqual(rows[1]['raw_reply_hex'],bytes(bad).hex())
        self.assertEqual(rows[1]['tools'][0]['markers'],[])
        self.assertIsNone(rows[1]['tools'][0]['device_frame_number'])
    def test_identity_mismatch_retains_unidentified_measurements(self):
        r=packet([tool(markers={0:(3,[1,2,3],0),1:(3,[4,5,6],0)},points=[([7,8,9],0)])])
        p,_,rows,_,_=self.run_case([r]);t=rows[0]['tools'][0]
        self.assertEqual(t['marker_identity_mapping'],'unknown_count_mismatch_or_not_reported')
        self.assertIsNone(t['markers'][0]['xyz_mm'])
        self.assertEqual(t['unidentified_measured_positions'][0]['xyz_mm'],[7,8,9])
        self.assertIsNone(t['unidentified_measured_positions'][0]['label'])
    def test_overflow_is_counted_and_not_complete(self):
        p,_,rows,summary,_=self.run_case([packet([tool(markers={0:(3,[1,2,3],0)})])],mode='overflow')
        self.assertEqual(p.returncode,1);self.assertFalse(summary['storage_complete'])
        c=summary['counters'];self.assertEqual(c['attempted'],3000)
        self.assertGreater(c['dropped_queue_full']+c['dropped_queue_busy'],0)
        self.assertEqual(c['attempted'],c['enqueued']+c['dropped_queue_full']+c['dropped_queue_busy']+c['dropped_allocation']+c['rejected_not_accepting'])
    def test_write_failure_is_explicit_and_retains_partial_file(self):
        p,_,_,summary,partial=self.run_case([packet([tool(markers={0:(3,[1,2,3],0)})])],mode='write_failure')
        self.assertEqual(p.returncode,1);self.assertIn('NDI_MARKER_SIDECAR_ERROR',p.stderr)
        self.assertFalse(summary['storage_complete']);self.assertTrue(summary['error'])
        self.assertTrue(partial);self.assertGreater(summary['counters']['discarded_after_write_failure'],0)
    def test_rom_identity_matches_original_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            f=Path(tmp)/'fixture.rom';data=b'SYNTHETIC-ID-ONLY\x00\xff'*100;f.write_bytes(data)
            p=subprocess.run([str(PROGRAM),tmp,'hash',str(f)],check=True,capture_output=True,text=True)
            self.assertEqual(p.stdout.splitlines()[0],hashlib.md5(data).hexdigest()+' '+str(len(data)))

if __name__=='__main__':unittest.main(verbosity=2)
