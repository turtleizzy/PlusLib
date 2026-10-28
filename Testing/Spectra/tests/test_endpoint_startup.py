"""Temporary loopback ports only. No tracker access, including the launcher probe."""
from pathlib import Path
import json,socket,subprocess,tempfile,unittest
C=Path(__file__).resolve().parents[1]
class EndpointTests(unittest.TestCase):
 def test_original_dependency_restart_failure_is_reproduced(self):
  r=subprocess.run([str(C/'bin/socket_baseline'),'0','2'],capture_output=True,text=True,timeout=10)
  if r.returncode==0:self.skipTest('This OpenIGTLink dependency already supports immediate reuse')
  self.assertEqual(r.returncode,2,r.stdout+r.stderr);self.assertIn('BIND_FAILED round=1',r.stdout)
 def test_candidate_restarts_same_port_ten_times(self):
  r=subprocess.run([str(C/'bin/socket_candidate'),'0','10'],capture_output=True,text=True,timeout=10)
  self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertIn('RESTART_PASS rounds=10',r.stdout)
 def test_existing_listener_is_not_replaced(self):
  with socket.socket() as listener:
   listener.bind(('127.0.0.1',0));listener.listen(1);port=listener.getsockname()[1]
   r=subprocess.run([str(C/'bin/socket_candidate'),str(port)],capture_output=True,text=True,timeout=10)
   self.assertEqual(r.returncode,2);self.assertIn('errno=98',r.stdout)
 def test_preflight_stops_at_occupied_transform_before_start(self):
  with socket.socket() as listener,tempfile.TemporaryDirectory(dir=C/'audit') as tmp:
   listener.bind(('127.0.0.1',0));listener.listen(1);port=listener.getsockname()[1];p=Path(tmp)/'config.xml';p.write_text(f'<PlusConfiguration><PlusOpenIGTLinkServer ListeningPort="{port}"/></PlusConfiguration>')
   r=subprocess.run(['python3','-B',str(C/'tools/check_endpoint_ports.py'),str(p)],capture_output=True,text=True,timeout=5)
   self.assertEqual(r.returncode,2);self.assertIn('serial device untouched',r.stderr)
 def test_preflight_free_port_does_not_create_connection(self):
  with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
  with tempfile.TemporaryDirectory(dir=C/'audit') as tmp:
   p=Path(tmp)/'config.xml';p.write_text(f'<PlusConfiguration><PlusOpenIGTLinkServer ListeningPort="{port}"/></PlusConfiguration>')
   r=subprocess.run(['python3','-B',str(C/'tools/check_endpoint_ports.py'),str(p)],capture_output=True,text=True,timeout=5);self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main(verbosity=2)
