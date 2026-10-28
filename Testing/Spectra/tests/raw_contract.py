"""Independent OFFLINE contract oracle. Not an RMT implementation or recorder."""
from pathlib import Path
import json,math,hashlib,struct,re
import jsonschema
C=Path(__file__).resolve().parents[1]
P=C/'frozen' if (C/'frozen/FROZEN.json').is_file() else C.parent/'spec-bundle'
V=jsonschema.Draft202012Validator(json.loads((P/'SCHEMA.json').read_text()),format_checker=jsonschema.FormatChecker())
def crc64(data):
 c=0
 for b in data:
  c^=b<<56
  for _ in range(8):c=((c<<1)^ (0x42f0e1eba9ea3693 if c>>63 else 0))&0xffffffffffffffff
 return c
def pairs(items):
 d={}
 for k,v in items:
  if k in d:raise ValueError('duplicate key')
  d[k]=v
 return d
def parse(payload):
 if not 1<=len(payload)<=65535:raise ValueError('size')
 if payload.startswith(b'\xef\xbb\xbf') or b'\0' in payload:raise ValueError('BOM/NUL')
 def constant(s):raise ValueError('nonfinite')
 r=json.loads(payload.decode('utf-8'),object_pairs_hook=pairs,parse_constant=constant)
 def tree(x,n=0):
  if n>16:raise ValueError('depth')
  if isinstance(x,float) and not math.isfinite(x):raise ValueError('nonfinite')
  if isinstance(x,str) and re.fullmatch(r'0|[1-9][0-9]*',x) and int(x)>2**64-1:raise ValueError('u64')
  if isinstance(x,dict):
   for v in x.values():tree(v,n+1)
  if isinstance(x,list):
   for v in x:tree(v,n+1)
 tree(r);V.validate(r)
 if r['record_type']=='frame':
  d=r['data'];reply=d['reply'];wire=bytes.fromhex(reply['raw_reply_hex'])
  assert reply['raw_reply_bytes']==len(wire)
  assert hashlib.sha256(wire).hexdigest()==reply['raw_reply_sha256']
  assert d['frame_id']==r['acquisition_id']+':'+d['reply_seq']
  hs=set();ids=set()
  for t in d['tools']:
   assert t['handle'] not in hs;hs.add(t['handle'])
   if t['tool_id'] is not None:assert t['tool_id'] not in ids;ids.add(t['tool_id'])
   slots=t['marker_slots'];assert len(slots)==(20 if t['additional_info_available'] else 0)
   assert len({m['slot_index'] for m in slots})==len(slots)
   sync=d['recovery_state']=='SYNCHRONIZED' and t['recovery_state']=='SYNCHRONIZED'
   for m in slots:
    assert m['slot_label']==chr(65+m['slot_index'])
    expected=bool(reply['valid'] and sync and t['reported'] and m['identity_known'] and m['coordinate_available'] and m['finite'] is True and m['xyz_mm'] is not None and m['out_of_volume'] is False and m['device_status']==3)
    assert m['sample_valid']==expected
   p=t['raw_pose']
   if p is not None:
    unit=p['quaternion_wxyz'] is not None and abs(sum(x*x for x in p['quaternion_wxyz'])-1)<=1e-3
    expected=bool(reply['valid'] and sync and t['reported'] and p['finite'] and unit and t['tool_status']=='OK' and t['tool_status_origin']=='PLUS_CURRENT_BX_STATUS')
    assert p['sample_valid']==expected
 return r
class IGTL:
 def __init__(self):self.buffer=bytearray();self.records=[]
 def feed(self,data):
  self.buffer.extend(data)
  while len(self.buffer)>=58:
   version,typ,dev,ts,n,crc=struct.unpack('!H12s20sQQQ',self.buffer[:58])
   if version!=1 or typ.rstrip(b'\0')!=b'STRING' or dev.rstrip(b'\0')!=b'NDI_RAW_V1' or ts!=0 or not 5<=n<=65539:raise ValueError('header')
   if len(self.buffer)<58+n:return
   body=bytes(self.buffer[58:58+n]);del self.buffer[:58+n]
   if crc64(body)!=crc:raise ValueError('CRC64')
   encoding,length=struct.unpack('!HH',body[:4])
   if encoding!=106 or length!=len(body)-4:raise ValueError('STRING length/encoding')
   self.records.append(body[4:])
 def finish(self):
  if self.buffer:raise ValueError('truncated IGTL')
  return self.records
class Stream:
 def __init__(self):self.rows=[];self.hash=hashlib.sha256();self.frames=0;self.gaps=False;self.last_reply=None;self.complete=False
 def add(self,payload):
  r=parse(payload);seq=int(r['seq'])
  if not self.rows:
   assert r['record_type']=='session' and seq==0
  else:
   first=self.rows[0];last=self.rows[-1];assert last['record_type']!='end'
   for key in ['source_id','acquisition_id','session_id']:assert r[key]==first[key]
   assert seq>int(last['seq']);self.gaps|=seq!=int(last['seq'])+1
  self.rows.append(r)
  if r['record_type']=='frame':
   reply=int(r['data']['reply_seq']);first=int(self.rows[0]['data']['first_reply_seq'])
   assert reply>=first
   if self.last_reply is not None:assert reply>self.last_reply
   self.gaps|=reply!=(first if self.last_reply is None else self.last_reply+1)
   self.last_reply=reply;self.frames+=1;self.hash.update(len(payload).to_bytes(4,'big')+payload)
  if r['record_type']=='end':
   d=r['data'];c={k:int(v) for k,v in d['counters'].items()};assert c['frames_considered']==c['frames_enqueued']+c['frames_dropped'];assert c['frames_enqueued']==c['frames_sent']+c['frames_discarded'];assert c['frames_sent']==self.frames;assert d['sent_frame_payload_sha256']==self.hash.hexdigest()
   assert d['first_reply_seq']==self.rows[0]['data']['first_reply_seq'];assert (d['last_reply_seq'] is None)==(c['frames_considered']==0)
   if d['last_reply_seq'] is not None:assert int(d['last_reply_seq'])-int(d['first_reply_seq'])+1==c['frames_considered']
   self.complete=bool(d['sender_complete'] and not self.gaps and not c['frames_dropped'] and not c['frames_discarded'])
   if d['sender_complete']:assert self.complete
  return r

class Streams:
 """All sessions remain accountable; a later complete END cannot repair a prior one."""
 def __init__(self):self.streams=[];self.rows=[];self.frames=0
 @property
 def complete(self):return bool(self.streams) and all(s.complete for s in self.streams)
 def add(self,payload):
  row=json.loads(payload) # dispatch only; Stream.add performs the full validation once
  if row['record_type']=='session':
   if self.streams:
    previous=self.streams[-1]
    assert previous.rows[-1]['record_type']=='end'
    assert row['data']['previous_session_id']==previous.rows[0]['session_id']
    assert row['source_id']==previous.rows[0]['source_id']
    if row['data']['resume_reason']=='RECOVERY':assert row['acquisition_id']!=previous.rows[0]['acquisition_id']
   self.streams.append(Stream())
  assert self.streams
  result=self.streams[-1].add(payload);self.rows.append(result)
  self.frames+=int(row['record_type']=='frame');return result
