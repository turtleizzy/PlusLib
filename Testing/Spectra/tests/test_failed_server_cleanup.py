"""Occupied listener after acquisition startup, guarded owned PTY only."""
from pathlib import Path
import json,os,pty,socket,subprocess,tempfile,unittest,xml.etree.ElementTree as ET
from pty_simulator import Simulator
C=Path(__file__).resolve().parents[1]
class FailureCleanupTests(unittest.TestCase):
 def test_bind_failure_finalizes_started_sidecar(self):
  self.assertTrue((C/'bin/pty_only_shim.so').is_file());out=Path(tempfile.mkdtemp(prefix='startup-cleanup-',dir=C/'audit'));side=out/'sidecar';side.mkdir()
  master,slave=pty.openpty();path=os.ttyname(slave);self.assertTrue(path.startswith('/dev/pts/'));sim=Simulator(master)
  with socket.socket() as occupied,socket.socket() as reserve:
   occupied.bind(('127.0.0.1',0));occupied.listen(1);port=occupied.getsockname()[1];reserve.bind(('127.0.0.1',0));rawport=reserve.getsockname()[1];reserve.close()
   tree=ET.parse(C/'config/generated/spectra-raw-gui-test.xml');d=tree.getroot().find('./DataCollection/Device');d.set('SerialPort','1');d.set('RawRecoverInitial','false');d.set('MarkerSidecarDirectory',str(side));d.set('RawPort',str(rawport));tree.getroot().find('PlusOpenIGTLinkServer').set('ListeningPort',str(port));cfg=out/'config.xml';tree.write(cfg,encoding='utf-8')
   env=dict(os.environ,NDI_MARKER_PTY=path,NDICAPI_SERIAL_WRITE_DELAY_US='1500',LD_PRELOAD=str(C/'bin/pty_only_shim.so'),LD_LIBRARY_PATH=str(C/'bin'))
   try:
    with (out/'plus.log').open('w') as log:r=subprocess.run([str(C/'bin/PlusServer'),'--config-file='+str(cfg),'--verbose=4'],cwd=C/'bin',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=25)
   finally:sim.close();os.close(master);os.close(slave)
  log=(out/'plus.log').read_text();self.assertNotEqual(r.returncode,0);self.assertIn('errno=98',log);self.assertIn('closing acquired devices and archives',log)
  summaries=[json.loads(p.read_text()) for p in side.glob('*/summary.json')];self.assertEqual(len(summaries),1);self.assertTrue(summaries[0]['storage_complete']);self.assertIsNone(sim.error)
  (out/'result.json').write_text(json.dumps({'classification':'SYNTHETIC_PTY_ONLY','passed':True,'exit':r.returncode,'sidecar':summaries[0]},indent=2)+'\n');print('EVIDENCE',out,flush=True)
if __name__=='__main__':unittest.main(verbosity=2)
