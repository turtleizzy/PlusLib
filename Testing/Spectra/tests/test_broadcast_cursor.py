"""Owned in-memory buffer regression; baseline comparison retained, no device access."""
from pathlib import Path
import json,os,re,subprocess,unittest
C=Path(__file__).resolve().parents[1];OLD=Path(os.environ['PLUS_NDI_BASELINE']) if os.environ.get('PLUS_NDI_BASELINE') else None
def run(mode,base=C):
 r=subprocess.run([str(C/'bin/broadcast_cursor_fixture'),mode],env=dict(os.environ,LD_LIBRARY_PATH=str(base/'bin')),capture_output=True,text=True,timeout=10)
 assert r.returncode==0,(r.stdout,r.stderr)
 return r.stdout+r.stderr
class CursorTests(unittest.TestCase):
 def test_near_exact_interpolation_keeps_requested_timestamp(self):
  out=run('near');self.assertEqual(float(re.search(r'returned=(\S+)',out)[1]),10.1)
 def test_fixed_buffers_do_not_rebroadcast_same_frame(self):
  out=run('cursor');r=re.search(r'frames=(\d+) unique=(\d+) errors=(\d+)',out);self.assertEqual(tuple(map(int,r.groups())),(2,2,0),out)
 def test_future_cursor_waits_without_error(self):
  out=run('future');self.assertIn('frames=0 unique=0 errors=0',out)
 def test_invalid_interpolation_logs_once_preserves_invalid(self):
  out=run('invalid');self.assertEqual(out.count('No usable transform for interpolation'),1,out)
 def test_invalid_second_neighbor_logs_once_preserves_invalid(self):
  out=run('invalid_second');self.assertEqual(out.count('No usable transform for interpolation'),1,out)
 def test_baseline_duplicate_source_is_reproduced(self):
  if OLD is None:self.skipTest('Set PLUS_NDI_BASELINE to reproduce the earlier source build')
  out=run('cursor',OLD);self.assertIn('frames=1001 unique=2 errors=0',out)
if __name__=='__main__':unittest.main(verbosity=2)
