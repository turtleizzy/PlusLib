"""Deterministic SYNTHETIC wire input, not a physical NDI recording."""
from pathlib import Path
import json,shutil,subprocess,sys,tempfile
C=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(C/'tests'))
from bx_fixtures import packet,tool
def generate():
    cases=[('first',packet([tool(frame=0xfffffffe,markers={0:(3,[7,8,1000],0),2:(3,[11,12,1100],0)}),tool(2,17)])),
           ('repeat',packet([tool(frame=0xfffffffe,markers={0:(3,[7,8,1000],0),2:(3,[11,12,1100],0)}),tool(2,17)])),
           ('partial',packet([tool(frame=0xffffffff,status=2,markers={0:(3,[7.1,8.2,1000.3],0)}),tool(2,18,status=2)])),
           ('wrap',packet([tool(frame=0,markers={0:(4,[9,10,1001],1)},port=0x71)])),
           ('unknown',packet([tool(frame=1,markers={0:(3,[1,2,3],0),1:(3,[4,5,6],0)},points=[([19,20,21],0)])])),
           ('rebound',packet([tool(handle=3,frame=5)])),
           ('epoch2-reconnect',packet([tool(handle=3,frame=0)]))]
    bad=bytearray(cases[0][1]);bad[-1]^=1;cases.append(('bad_crc',bytes(bad)))
    with tempfile.TemporaryDirectory() as temp:
        parent=Path(temp);files=[]
        for name,raw in cases:
            filename='epoch2-bad_crc' if name=='bad_crc' else name
            f=parent/(filename+'.bin');f.write_bytes(raw);files.append(f)
        result=subprocess.run([str(C/'bin/recorder_fixture'),temp,'normal',*map(str,files)],capture_output=True,text=True,check=True)
        session=Path(result.stdout.splitlines()[0]);rows=[json.loads(s) for s in (session/'measurements.jsonl').read_text().splitlines()]
        # Replace run-specific ID only, preserving encoder-produced fields.
        for row in rows:row['session_id']='SYNTHETIC-DEMONSTRATION-v1'
        summary=json.loads((session/'summary.json').read_text());summary['session_id']='SYNTHETIC-DEMONSTRATION-v1'
        (C/'fixtures/synthetic-measurements.jsonl').write_text(''.join(json.dumps(r,separators=(',',':'))+'\n' for r in rows))
        (C/'fixtures/synthetic-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
        (C/'fixtures/expected.json').write_text(json.dumps({'source':'SYNTHETIC ONLY','cases':[n for n,_ in cases],
          'first_marker_A_mm':[7,8,1000],'first_tool_translation_mm':[100,200,300],
          'expected_reply_count':len(cases),'independent_marker_timestamp':None},indent=2)+'\n')
if __name__=='__main__':generate()
