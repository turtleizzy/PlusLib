"""Contract-artifact verification only; no serial/network/receiver implementation."""
from pathlib import Path
import sys,json,hashlib,math,copy,struct,uuid,importlib.metadata
import jsonschema
P=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parent
s=json.loads((P/'SCHEMA.json').read_text());jsonschema.Draft202012Validator.check_schema(s)
v=jsonschema.Draft202012Validator(s,format_checker=jsonschema.FormatChecker());checks=0

def check(x,msg):
 global checks
 checks+=1
 if not x: raise AssertionError(msg)

def finite_tree(x,depth=0):
 check(depth<=16,'depth')
 if isinstance(x,float):check(math.isfinite(x),'nonfinite')
 if isinstance(x,dict):
  for y in x.values():finite_tree(y,depth+1)
 if isinstance(x,list):
  for y in x:finite_tree(y,depth+1)

def crc(b):
 c=0
 for x in b:
  c^=x
  for _ in range(8):c=(c>>1)^((c&1)*0xa001)
 return c

summary={}
for name in ['GOLDEN.jsonl','GOLDEN-VALID-REPEAT.jsonl']:
 lines=(P/name).read_bytes().splitlines();rows=[json.loads(x) for x in lines];h=hashlib.sha256();nframes=0
 for i,(line,r) in enumerate(zip(lines,rows)):
  v.validate(r);finite_tree(r);check(len(line)<=65535,'payload bytes');check(r['seq']==str(i),'seq');check(r['session_id']==rows[0]['session_id'],'session')
  if r['record_type']=='frame':
   nframes+=1;d=r['data'];w=bytes.fromhex(d['reply']['raw_reply_hex']);check(d['frame_id']==r['acquisition_id']+':'+d['reply_seq'],'frame id');check(len(w)==d['reply']['raw_reply_bytes'],'wire length');check(hashlib.sha256(w).hexdigest()==d['reply']['raw_reply_sha256'],'wire sha')
   h.update(len(line).to_bytes(4,'big')+line)
   if d['reply']['valid']:
    check(len(w)>=8,'wire nonempty');check(crc(w[:4])==int.from_bytes(w[4:6],'little'),'header crc');check(crc(w[6:-2])==int.from_bytes(w[-2:],'little'),'payload crc');check(len(w)==int.from_bytes(w[2:4],'little')+8,'length header')
   for t in d['tools']:
    check(len(t['marker_slots'])==(20 if t['additional_info_available'] else 0),'all status slots')
    for m in t['marker_slots']:
     check(m['slot_label']==chr(65+m['slot_index']),'slot label')
     expected=d['reply']['valid'] and d['recovery_state']=='SYNCHRONIZED' and t['recovery_state']=='SYNCHRONIZED' and t['reported'] and m['identity_known'] and m['coordinate_available'] and m['finite'] is True and m['xyz_mm'] is not None and m['out_of_volume'] is False and m['device_status']==3
     check(m['sample_valid']==expected,'sample validity')
 e=rows[-1]['data'];c={k:int(x) for k,x in e['counters'].items()};check(rows[0]['record_type']=='session','start');check(rows[-1]['record_type']=='end','end');check(c['frames_sent']==nframes,'sent count');check(c['frames_considered']==c['frames_enqueued']+c['frames_dropped'],'enqueue conservation');check(c['frames_enqueued']==c['frames_sent']+c['frames_discarded'],'send conservation');check(h.hexdigest()==e['sent_frame_payload_sha256'],'exact bytes digest')
 summary[name]={'records':len(rows),'frames':nframes,'maximum_utf8_bytes':max(map(len,lines)),'digest_verified':True}
v.validate(json.loads((P/'ERROR-SHAPE.json').read_text()));check(True,'error shape')
valid=json.loads((P/'GOLDEN-VALID-REPEAT.jsonl').read_bytes().splitlines()[1]);m=valid['data']['tools'][0]['marker_slots'][0]
# Schema-only negative mutations (semantic gates are additionally documented in PROTOCOL).
negatives=[]
for label,mutate in [('schema major',lambda r:r.update(schema='ndi.raw.v2')),('numeric seq',lambda r:r.update(seq=1)),('unknown key',lambda r:r.update(unexpected=True)),('invalid slot',lambda r:r['data']['tools'][0]['marker_slots'][0].update(slot_index=20)),('finite XYZ wrong length',lambda r:r['data']['tools'][0]['marker_slots'][0].update(xyz_mm=[1,2]))]:
 q=copy.deepcopy(valid);mutate(q);check(not v.is_valid(q),label);negatives.append(label)
# Ensure newly specified nonfinite-pose representation is expressible, without asserting wire correspondence.
q=copy.deepcopy(valid);p=q['data']['tools'][0]['raw_pose'];p.update(finite=False,quaternion_wxyz=None,sample_valid=False,invalid_reasons=['NONFINITE_DEVICE_POSE']);v.validate(q);check(True,'nonfinite pose shape representable')
result={'artifact_checks_passed':True,'checks':checks,'schema_validator':'jsonschema '+importlib.metadata.version('jsonschema'),'python':sys.version,'streams':summary,'schema_negative_cases':negatives,'limits':'Synthetic contract fixtures; no driver/receiver implementation, serial/network/hardware or actual Plus binary tests.'}
(P/'CHECKS.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
