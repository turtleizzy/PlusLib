"""OFFLINE: golden framing, strict oracle and actual C++ socketpair sender.
Never opens a TCP port or physical serial device. Replay uses explicitly labelled bytes.
"""
from pathlib import Path
import unittest,copy,json,hashlib,socket,subprocess,os,tempfile,time,threading,struct
from raw_contract import P,IGTL,Stream,parse,crc64
from bx_fixtures import packet,tool,error_reply
C=Path(__file__).resolve().parents[1]
def packed(payload):
 body=struct.pack('!HH',106,len(payload))+payload
 return struct.pack('!H12s20sQQQ',1,b'STRING',b'NDI_RAW_V1',0,len(body),crc64(body))+body
def jsonbytes(row):return json.dumps(row,separators=(',',':'),allow_nan=False).encode()
def run_sender(wires,mode='normal',read=True,bytewise=False,environment=None,stall=0,allow_truncated=False):
 parent=Path(tempfile.mkdtemp(prefix='raw-test-',dir=C/'audit'))
 paths=[]
 for i,wire in enumerate(wires):p=parent/f'{i}.bx';p.write_bytes(wire);paths.append(str(p))
 a,b=socket.socketpair();a.settimeout(10);received=bytearray()
 if not read:a.close()
 proc=subprocess.Popen([str(C/'bin/raw_fixture'),mode,str(parent),str(b.fileno()),*paths],pass_fds=[b.fileno()],env=dict(os.environ,LD_LIBRARY_PATH=str(C/'bin'),**(environment or {})),stdout=subprocess.PIPE,stderr=subprocess.PIPE);b.close()
 if read:
  if stall:time.sleep(stall)
  while True:
   data=a.recv(1 if bytewise else 65536)
   if not data:break
   received.extend(data)
  a.close()
 out,err=proc.communicate(timeout=10);assert proc.returncode==0,(proc.returncode,out,err)
 (parent/"wire.igtl").write_bytes(received)
 parser=IGTL();parser.feed(received);payloads=parser.records if allow_truncated else parser.finish()
 return parent,payloads,out.decode()
class GoldenTests(unittest.TestCase):
 def test_all_frozen_hashes(self):
  for name,info in json.loads((P/'FROZEN.json').read_text())['files'].items():
   data=(P/name).read_bytes();self.assertEqual((len(data),hashlib.sha256(data).hexdigest()),(info['bytes'],info['sha256']))
 def test_supplied_igtl_bytewise_and_coalesced(self):
  for name in ['GOLDEN','GOLDEN-VALID-REPEAT']:
   wire=(P/(name+'.igtl')).read_bytes();expected=(P/(name+'.jsonl')).read_bytes().splitlines()
   for step in [1,7,53,len(wire)]:
    parser=IGTL()
    for i in range(0,len(wire),step):parser.feed(wire[i:i+step])
    self.assertEqual(parser.finish(),expected);s=Stream()
    for payload in expected:s.add(payload)
    self.assertTrue(s.complete)
 def test_malformed_json(self):
  valid=(P/'GOLDEN-VALID-REPEAT.jsonl').read_bytes().splitlines()[1]
  for payload in [valid.replace(b'"seq":"1"',b'"seq":"1","seq":"1"'),valid+b'x',b'\xef\xbb\xbf'+valid,valid+b'\0',valid.replace(b'"seq":"1"',b'"seq":"18446744073709551616"'),valid.replace(b'"schema_minor":0',b'"schema_minor":NaN'),valid.replace(b'"schema_minor":0',b'"schema_minor":Infinity'),valid.replace(b'"extensions":{}',b'"extensions":'+b'['*18+b'0'+b']'*18)]:
   with self.assertRaises(Exception):parse(payload)
 def test_crc_truncation_header_length_utf8(self):
  wire=bytearray((P/'GOLDEN.igtl').read_bytes());wire[80]^=1
  with self.assertRaises(ValueError):IGTL().feed(wire)
  parser=IGTL();parser.feed((P/'GOLDEN.igtl').read_bytes()[:-1])
  with self.assertRaises(ValueError):parser.finish()
  for payload in [b'\xff',b'x'*65536]:
   with self.assertRaises(Exception):parse(payload)
  self.assertEqual(len(packed(b'x'*65535)),65597)
 def test_seq_duplicate_conflict_cross_session(self):
  lines=(P/'GOLDEN-VALID-REPEAT.jsonl').read_bytes().splitlines()
  for change in ['duplicate','backward','cross']:
   s=Stream();s.add(lines[0]);s.add(lines[1]);r=json.loads(lines[2])
   r['seq']='1' if change=='duplicate' else ('0' if change=='backward' else '2')
   if change=='cross':r['session_id']='99999999-9999-4999-8999-999999999999'
   with self.assertRaises(AssertionError):s.add(jsonbytes(r))
class SenderTests(unittest.TestCase):
 def test_current_pose_valid_does_not_mean_four_balls(self):
  wires=[packet([tool(frame=i,markers={0:(3,[1.25,2.5,-4.75],0),19:(4,[4,5,6],0)})]) for i in [10,10,11]]
  parent,payloads,out=run_sender(wires,bytewise=True);s=Stream();rows=[s.add(x) for x in payloads];self.assertTrue(s.complete,out);self.assertIn('sidecar=1',out)
  frames=[x for x in rows if x['record_type']=='frame'];t=frames[0]['data']['tools'][0]
  self.assertTrue(t['raw_pose']['sample_valid']);self.assertIsNone(t['defined_slots']);self.assertEqual(sum(x['sample_valid'] for x in t['marker_slots']),1)
  self.assertEqual([f['data']['tools'][0]['counter']['classification'] for f in frames],['FIRST','REPEAT','NEW'])
  self.assertIsNone(t['counter']['is_new_device_frame']);self.assertEqual(t['raw_pose']['fit_error_unit'],'UNKNOWN')
  side=list(parent.glob('ndi-marker-*/measurements.jsonl'));self.assertEqual(len(side),1)
  sc=[json.loads(x) for x in side[0].read_bytes().splitlines() if json.loads(x)['record_type']=='measurement_reply']
  self.assertEqual([x['raw_v1_snapshot_data'] for x in sc],[x['data'] for x in frames])
 def test_nonunit_quaternion_is_raw_invalid(self):
  _,payloads,out=run_sender([packet([tool(pose=[2,0,0,0,1,2,3,.1])])]);s=Stream();rows=[s.add(x) for x in payloads]
  self.assertTrue(s.complete,out);p=next(r for r in rows if r['record_type']=='frame')['data']['tools'][0]['raw_pose']
  self.assertFalse(p['sample_valid']);self.assertIn('QUATERNION_NOT_UNIT',p['invalid_reasons']);self.assertEqual(p['quaternion_wxyz'],[2,0,0,0])
 def test_nan_pose_and_marker_unidentified_oov_missing(self):
  cases=[tool(markers={0:(3,[float('nan'),2,3],0)},pose=[1,0,0,0,float('nan'),0,0,0]),tool(frame=124,markers={0:(3,[1,2,3],1)}),tool(frame=125,status=2),tool(frame=126,markers={0:(3,[1,2,3],0),1:(3,[4,5,6],0)},points=[([9,8,7],0)])]
  _,payloads,out=run_sender([packet([t]) for t in cases]);s=Stream();rows=[s.add(x) for x in payloads];self.assertTrue(s.complete,out)
  ts=[r['data']['tools'][0] for r in rows if r['record_type']=='frame']
  self.assertIsNotNone(ts[0]['raw_pose']);self.assertFalse(ts[0]['raw_pose']['finite']);self.assertIsNone(ts[0]['raw_pose']['translation_mm']);self.assertIsNone(ts[0]['marker_slots'][0]['xyz_mm'])
  self.assertFalse(ts[1]['marker_slots'][0]['sample_valid']);self.assertIsNone(ts[2]['raw_pose']);self.assertEqual(ts[3]['unidentified_positions'][0]['xyz_mm'],[9,8,7]);self.assertFalse(ts[3]['marker_identity_known'])
 def test_counter_changed_backwards_wrap_quarantine_sticky(self):
  for frames,expected in [([5,5,6],['FIRST','REPEAT','NEW']),([5,4,6],['FIRST','RESET_OR_OUT_OF_ORDER','NEW']),([0xffffffff,0,1],['FIRST','WRAP_CANDIDATE','NEW'])]:
   wires=[packet([tool(frame=f,markers={0:(3,[i,2,3],0)})]) for i,f in enumerate(frames)]
   _,payloads,out=run_sender(wires);s=Stream();ts=[r['data']['tools'][0] for x in payloads if (r:=s.add(x))['record_type']=='frame'];self.assertTrue(s.complete,out)
   self.assertEqual([t['counter']['classification'] for t in ts],expected)
   self.assertTrue(ts[0]['marker_slots'][0]['sample_valid']);self.assertFalse(ts[1]['marker_slots'][0]['sample_valid']);self.assertFalse(ts[2]['marker_slots'][0]['sample_valid'])
 def test_error_crc_and_timeout_integrity_vs_measurement(self):
  for wire in [b'',error_reply(7),packet([tool()])[:-1]]:
   _,payloads,out=run_sender([wire]);s=Stream();rows=[s.add(x) for x in payloads];self.assertTrue(s.complete,out)
   frame=next(r for r in rows if r['record_type']=='frame');self.assertFalse(frame['data']['reply']['valid']);self.assertFalse(frame['data']['tools'][0]['reported'])
 def test_disconnect(self):
  parent,payloads,out=run_sender([packet([tool()])],read=False)
  self.assertIn('raw=0,0',out);summary=json.loads(next(parent.glob('raw-*.json')).read_text());self.assertFalse(summary['sender_complete']);self.assertTrue(summary['counter_conservation'])
 def test_saturation_nonblocking_and_idempotent_stop(self):
  parent,payloads,out=run_sender([packet([tool()])],mode='overflow');summary=json.loads(next(parent.glob('raw-*.json')).read_text());self.assertIn('raw=0,0',out);self.assertGreater(int(summary['counters']['frames_dropped']),0);self.assertTrue(summary['counter_conservation']);self.assertFalse(summary['sender_complete']);self.assertLess(int(summary['max_queued_frames']),2)
 def test_short_writes_eintr_eagain_and_deadlines(self):
  for mode in ['short','eintr','eagain','timeout','zero','partial_timeout']:
   env={'LD_PRELOAD':str(C/'bin/raw_fault.so'),'RAW_TEST_FAULT':mode}
   before=time.monotonic();parent,payloads,out=run_sender([packet([tool()])],environment=env,allow_truncated=mode=='partial_timeout')
   self.assertLess(time.monotonic()-before,3)
   if mode in ['short','eintr','eagain']:
    s=Stream()
    for x in payloads:s.add(x)
    self.assertTrue(s.complete,out)
   else:self.assertIn('raw=0,0',out)
   if mode=='partial_timeout':self.assertEqual(len((parent/'wire.igtl').read_bytes()),7)
 def test_all_raw_summary_io_failures(self):
  for mode in ['summary_write','summary_fsync','summary_close','summary_link']:
   env={'LD_PRELOAD':str(C/'bin/raw_fault.so'),'RAW_TEST_FAULT':mode}
   parent,payloads,out=run_sender([packet([tool()])],environment=env)
   self.assertIn('raw=0,0',out);self.assertIn('LOCAL_SUMMARY',out)
   self.assertFalse(list(parent.glob('raw-*.json')))
   self.assertTrue(list(parent.glob('raw-*.json.pending')))
 def test_raw_thread_start_failure_cleanup_and_retry(self):
  env={'LD_PRELOAD':str(C/'bin/fault_shim.so'),'NDI_TEST_STORAGE_FAULT':'thread_create'}
  parent,payloads,out=run_sender([packet([tool()])],mode='start_retry',environment=env)
  self.assertIn('first=0 stop=0,0',out);self.assertIn('raw=1,1',out)
  self.assertTrue(list(parent.glob('raw-startup-*.json')))
 def test_backpressure_does_not_hold_producer(self):
  parent,payloads,out=run_sender([packet([tool(markers={j:(3,[1,2,3],0) for j in range(20)})])],mode='overflow',stall=.25,allow_truncated=True)
  self.assertIn('raw=0,0',out);value=float(out.split('max_producer_s=')[1].split()[0]);self.assertLess(value,.05,out)
 def test_byte_budget_drop_and_exact_summary(self):
  parent,payloads,out=run_sender([packet([tool()])],mode='byte_budget');summary=json.loads(next(parent.glob('raw-*.json')).read_text())
  self.assertIn('raw=0,0',out);self.assertTrue(summary['counter_conservation']);self.assertEqual(summary['counters'],{'frames_considered':'1','frames_enqueued':'0','frames_dropped':'1','frames_sent':'0','frames_discarded':'0'})
  s=Stream()
  for payload in payloads:s.add(payload)
  self.assertFalse(s.complete)
 def test_actual_cpp_pack_boundaries_and_oversize_frame(self):
  for size in [65535,65536]:
   parent,payloads,out=run_sender([b'x'*size],mode='pack')
   self.assertIn('PACK_OK' if size==65535 else 'PACK_SIZE_REJECTED',out)
   if size==65535:self.assertEqual((parent/'packed.igtl').read_bytes(),packed(b'x'*size))
  wire=packet([tool(i+1,markers={j:(3,[i,j,100],0) for j in range(20)}) for i in range(24)])
  parent,payloads,out=run_sender([wire]);summary=json.loads(next(parent.glob('raw-*.json')).read_text())
  self.assertFalse(summary['sender_complete']);self.assertEqual(summary['counters']['frames_discarded'],'1');self.assertTrue(summary['counter_conservation']);self.assertIn('OVERSIZE_FRAME',out)
 def test_restart_is_not_recovery_and_clock_jump_diagnostic(self):
  parent,payloads,out=run_sender([packet([tool()])],mode='model_check')
  self.assertIn('MODEL_RECOVERY_AND_CLOCK_CHECK_OK',out)
if __name__=='__main__':unittest.main(verbosity=2)
