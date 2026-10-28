"""Independent synthetic protocol builder. These bytes are NOT device recordings."""
import struct
def crc16(data):
    c=0
    for b in data:
        c^=b
        for _ in range(8):c=(c>>1)^0xa001 if c&1 else c>>1
    return c
def error_reply(code=1):
    text=('ERROR%02X'%code).encode()
    return text+('%04X'%crc16(text)).encode()+b'\r'
def tool(handle=1,frame=123,status=1,markers=None,pose=None,port=0x31,tool_info=0,points=None):
    return dict(handle=handle,frame=frame,status=status,markers=markers or {},
                pose=pose or [1,0,0,0,100,200,300,0.1],port=port,tool_info=tool_info,points=points)
def packet(tools,flags=0x080b,system=0,extra=b''):
    data=bytearray([len(tools)])
    for t in tools:
        data+=bytes([t['handle'],t['status']])
        if t['status']==4:continue
        if t['status']!=2:data+=struct.pack('<8f',*t['pose'])
        data+=struct.pack('<II',t['port'],t['frame'])
        labels=[0]*20
        for label,(status,xyz,oov) in t['markers'].items():labels[label]=status
        if flags&2:
            packed=[labels[i*2]|(labels[i*2+1]<<4) for i in range(10)]
            data+=bytes([t['tool_info']]+list(reversed(packed)))
        if flags&8:
            points=t['points']
            if points is None:points=[(xyz,oov) for label,(status,xyz,oov) in sorted(t['markers'].items()) if status]
            data.append(len(points))
            bits=bytearray((len(points)+7)//8)
            for i,(xyz,oov) in enumerate(points):
                if oov:bits[i//8]|=1<<(i%8)
            data+=bits
            for xyz,oov in points:data+=struct.pack('<3f',*xyz)
    data+=struct.pack('<H',system)+extra
    head=b'\xc4\xa5'+struct.pack('<H',len(data))
    return head+struct.pack('<H',crc16(head))+data+struct.pack('<H',crc16(data))
