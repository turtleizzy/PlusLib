"""HOST OFFLINE: real loopback TCP, actual C++ sender. No physical device access."""
from pathlib import Path
import json,os,socket,struct,subprocess,tempfile,time,unittest
from bx_fixtures import packet,tool
from raw_contract import IGTL,Stream
C=Path(__file__).resolve().parents[1]
def free_port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def start(port,n=6):
 parent=Path(tempfile.mkdtemp(prefix='raw-tcp-',dir=C/'audit'));paths=[]
 for i in range(n):
  p=parent/f'{i}.bx';p.write_bytes(packet([tool(frame=100+i,markers={0:(3,[1.25,2.5,-4.75],0)})]));paths.append(str(p))
 p=subprocess.Popen([str(C/'bin/raw_fixture'),'tcp',str(parent),str(port),*paths],env=dict(os.environ,LD_LIBRARY_PATH=str(C/'bin')),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 return parent,p
def connect(port,proc):
 end=time.monotonic()+3
 while time.monotonic()<end:
  if proc.poll() is not None:raise AssertionError(proc.communicate())
  try:s=socket.create_connection(('127.0.0.1',port),timeout=.3);s.settimeout(3);return s
  except ConnectionRefusedError:time.sleep(.005)
 raise AssertionError('connect timeout')
def read_all(s,step=65536):
 p=IGTL()
 while True:
  data=s.recv(step)
  if not data:break
  p.feed(data)
 s.close();p.finish();stream=Stream();rows=[stream.add(x) for x in p.records];return stream,rows
class TCPTests(unittest.TestCase):
 def test_accept_actual_tcp_bytewise(self):
  parent,proc=start(free_port());port=int(proc.args[3]);s=connect(port,proc);stream,rows=read_all(s,1);out,err=proc.communicate(timeout=5)
  self.assertEqual(proc.returncode,0,(out,err));self.assertTrue(stream.complete);self.assertGreater(stream.frames,0)
  self.assertEqual(rows[0]['record_type'],'session');self.assertEqual(rows[-1]['record_type'],'end')
 def test_reconnect_same_acquisition_future_only_and_sticky_old_failure(self):
  port=free_port();parent,proc=start(port,30);s=connect(port,proc);first=IGTL();first_rows=[]
  while not any(x['record_type']=='frame' for x in first_rows):
   first.feed(s.recv(65536));first_rows=[json.loads(x) for x in first.records]
  s.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0));s.close()
  second=None;end=time.monotonic()+1
  while time.monotonic()<end:
   q=connect(port,proc)
   try:
    data=q.recv(65536)
    if data:second=(q,data);break
   except ConnectionResetError:pass
   q.close();time.sleep(.03)
  self.assertIsNotNone(second)
  q,data=second;parser=IGTL();parser.feed(data)
  while True:
   block=q.recv(65536)
   if not block:break
   parser.feed(block)
  q.close();parser.finish();stream=Stream();second_rows=[stream.add(x) for x in parser.records]
  out,err=proc.communicate(timeout=5);self.assertEqual(proc.returncode,0,(out,err));self.assertTrue(stream.complete,(out,err))
  self.assertEqual(first_rows[0]['acquisition_id'],second_rows[0]['acquisition_id']);self.assertNotEqual(first_rows[0]['session_id'],second_rows[0]['session_id'])
  self.assertEqual(second_rows[0]['data']['previous_session_id'],first_rows[0]['session_id']);self.assertEqual(second_rows[0]['data']['resume_reason'],'TCP_RECONNECT')
  last=max(int(r['data']['reply_seq']) for r in first_rows if r['record_type']=='frame');self.assertGreater(int(second_rows[0]['data']['first_reply_seq']),last)
  self.assertTrue(all(int(r['data']['reply_seq'])>last for r in second_rows if r['record_type']=='frame'))
  summaries=[json.loads(p.read_text()) for p in parent.glob('raw-*.json')];self.assertEqual(len(summaries),2);self.assertEqual(sum(x['sender_complete'] for x in summaries),1);self.assertIn(b'raw=0,0',out)
 def test_bind_conflict_has_no_port_fallback(self):
  with socket.socket() as guard:
   guard.bind(('127.0.0.1',0));guard.listen();port=guard.getsockname()[1];parent,proc=start(port,1);out,err=proc.communicate(timeout=5)
   self.assertEqual(proc.returncode,5);self.assertIn(b'bind/listen failed',err);self.assertTrue(list(parent.glob('raw-startup-*.json')))
 def test_immediate_restart_same_configured_port(self):
  port=free_port()
  for _ in range(2):
   parent,proc=start(port,2);s=connect(port,proc);stream,rows=read_all(s);out,err=proc.communicate(timeout=5);self.assertEqual(proc.returncode,0,(out,err));self.assertTrue(stream.complete)
if __name__=='__main__':unittest.main(verbosity=2)
