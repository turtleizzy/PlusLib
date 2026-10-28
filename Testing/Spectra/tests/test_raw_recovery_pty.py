"""Full candidate, one guarded PTY command owner, actual unmodified RMT receiver.
No physical device access. Restarts require complete reset/INIT/COMM/ROM/TSTART.
"""
from pathlib import Path
import json,os,pty,signal,socket,subprocess,sys,tempfile,time,unittest,xml.etree.ElementTree as ET
C=Path(__file__).resolve().parents[1];R=Path(os.environ['RMT_ROOT'])
sys.dont_write_bytecode=True;sys.path.insert(0,str(R))
from experiments.python_dry_v2.realtime_runtime_v1.ndi_raw_receiver import NdiRawReceiver,RawConnection
from experiments.python_dry_v2.realtime_runtime_v1.reference_archive import inspect_archive
from pty_simulator import Simulator
from raw_contract import IGTL,Streams
class RecoveryPTYTests(unittest.TestCase):
 def execute(self,reject_init=False):
  assert (C/'bin/pty_only_shim.so').is_file(), 'PTY guard must exist before starting a child'
  out=Path(tempfile.mkdtemp(prefix='raw-recovery-pty-',dir=C/'audit'));side=out/'sidecar';side.mkdir()
  master,slave=pty.openpty();path=os.ttyname(slave);self.assertTrue(path.startswith('/dev/pts/'))
  rd,wr=os.pipe()
  with socket.socket() as s:s.bind(('127.0.0.1',0));rawport=s.getsockname()[1]
  with socket.socket() as s:s.bind(('127.0.0.1',0));poseport=s.getsockname()[1]
  tree=ET.parse(C/'config/generated/spectra-raw-gui-test.xml');d=tree.getroot().find('./DataCollection/Device')
  d.set('SerialPort','1');d.set('RawPort',str(rawport));d.set('RawRecoverInitial','true');d.set('MarkerSidecarDirectory',str(side));d.set('MarkerReplyTimeoutMs','1000')
  tree.getroot().find('PlusOpenIGTLinkServer').set('ListeningPort',str(poseport));config=out/'config.xml';tree.write(config,encoding='utf-8')
  env=dict(os.environ,NDI_MARKER_PTY=path,NDI_RESET_TEST_FD=str(wr),NDICAPI_SERIAL_WRITE_DELAY_US='1500',LD_PRELOAD=str(C/'bin/pty_only_shim.so'),LD_LIBRARY_PATH=str(C/'bin'))
  sim=Simulator(master,reset_fd=rd,reject_recovery_init=reject_init);receiver=None
  with (out/'plus.log').open('w') as log:
   proc=subprocess.Popen([str(C/'bin/PlusServer'),'--config-file='+str(config),'--verbose=3','--running-time=3'],cwd=C/'bin',env=env,pass_fds=[wr],stdout=log,stderr=subprocess.STDOUT)
   try:
    deadline=time.monotonic()+20
    while proc.poll() is None and time.monotonic()<deadline:
     if 'Measured marker sidecar:' in (out/'plus.log').read_text(errors='replace'):
      receiver=NdiRawReceiver(RawConnection('127.0.0.1',port=rawport,expected_source_id=d.get('RawSourceId'),reconnect_attempts=0));receiver.start(out/'archive',context={'classification':'SYNTHETIC_PTY_ONLY','physical_device':False});break
     time.sleep(.005)
    proc.wait(timeout=18)
   finally:
    if proc.poll() is None:proc.send_signal(signal.SIGINT);proc.wait(timeout=10)
    if receiver:
     deadline=time.monotonic()+4
     while not receiver.snapshot()['producer_done'] and time.monotonic()<deadline:time.sleep(.01)
     state=receiver.stop(timeout=10)
    else:state={}
  sim.close();os.close(master);os.close(slave);os.close(rd);os.close(wr)
  self.assertIsNone(sim.error,sim.error)
  (out/'commands.json').write_text(json.dumps(sim.commands,indent=2)+'\n');(out/'synthetic-wire.json').write_text(json.dumps(sim.frames,indent=2)+'\n')
  parser=IGTL();parser.feed((out/'archive/wire.bin').read_bytes());payloads=parser.finish();rows=[json.loads(x) for x in payloads]
  (out/'payloads.jsonl').write_bytes(b'\n'.join(payloads)+b'\n')
  receipt=inspect_archive(out/'archive');proofs=[json.loads(x.read_text()) for x in side.glob('raw-recovery-*.json')]
  result={'classification':'SYNTHETIC_PTY_ONLY','exit':proc.returncode,'receiver':state,'archive_inspection':receipt,'proofs':proofs,'resets':sim.resets,'path':str(out)}
  (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
  print('EVIDENCE',out,flush=True)
  return out,result,rows,side,sim
 def test_full_recovery_end_then_real_commands_new_first(self):
  out,result,rows,side,sim=self.execute()
  self.assertEqual(result['exit'],0,(out/'plus.log').read_text());self.assertEqual(result['resets'],1)
  self.assertTrue(result['receiver']['storage_complete'],json.dumps(result['receiver'],indent=2));self.assertTrue(result['archive_inspection']['complete'])
  streams=Streams()
  for payload in (out/'payloads.jsonl').read_bytes().splitlines():streams.add(payload)
  self.assertEqual(len(streams.streams),2);self.assertTrue(streams.complete)
  a,b=streams.streams;self.assertEqual(a.rows[-1]['data']['reason'],'RECOVERY');self.assertEqual(a.rows[1]['data']['recovery_state'],'QUARANTINED')
  self.assertEqual(b.rows[0]['data']['previous_session_id'],a.rows[0]['session_id']);self.assertEqual(b.rows[0]['data']['resume_reason'],'RECOVERY');self.assertNotEqual(a.rows[0]['acquisition_id'],b.rows[0]['acquisition_id'])
  first=next(x for x in b.rows if x['record_type']=='frame');self.assertEqual(first['data']['reply_seq'],'1')
  for t in first['data']['tools']:
   self.assertEqual(t['counter']['classification'],'FIRST');self.assertIsNone(t['counter']['is_new_device_frame']);self.assertIsNone(t['counter']['delta_modulo'])
  proof=result['proofs'][0];self.assertTrue(proof['success']);self.assertGreaterEqual(int(proof['recovery_start_monotonic_ns']),int(proof['old_end_send_completed_monotonic_ns']));self.assertIn('5245534554424536460d',proof['reset_wire_hex'])
  measurement=[json.loads(x) for p in side.glob('ndi-marker-*/measurements.jsonl') for x in p.read_text().splitlines() if json.loads(x)['record_type']=='measurement_reply'];mapped={x['frame_id']:x for x in measurement}
  frames=[r for r in rows if r['record_type']=='frame'];self.assertTrue(frames)
  for r in frames:self.assertEqual(mapped[r['data']['frame_id']]['raw_v1_snapshot_data'],r['data'])
  self.assertEqual(len(sim.frames),len(measurement));self.assertEqual([x['host_poll_sequence'] for x in measurement],list(range(1,len(measurement)+1)))
  names=[x['command'] for x in sim.commands];reset=names.index('SERIAL_BREAK');self.assertEqual(names[reset-1],'BX');self.assertEqual(names[reset+1],'INIT');self.assertEqual(names[reset+2],'COMM');self.assertIn('PVWR',names[reset:]);self.assertIn('PENA',names[reset:]);self.assertIn('TSTART',names[reset:])
 def test_init_rejection_cannot_promote_or_create_new_session(self):
  out,result,rows,side,sim=self.execute(reject_init=True)
  self.assertNotEqual(result['exit'],0);self.assertEqual(result['resets'],1);self.assertFalse(result['proofs'][0]['success']);self.assertFalse(result['proofs'][0]['init_ok'])
  self.assertEqual(sum(r['record_type']=='session' for r in rows),1)
  self.assertTrue(all(r['data']['recovery_state']!='SYNCHRONIZED' for r in rows if r['record_type']=='frame'))
  self.assertEqual(len(sim.frames),len([x for p in side.glob('ndi-marker-*/measurements.jsonl') for x in p.read_text().splitlines() if json.loads(x)['record_type']=='measurement_reply']))
if __name__=='__main__':unittest.main(verbosity=2)
