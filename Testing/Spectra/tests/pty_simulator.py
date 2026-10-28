"""Synthetic owned-PTY peer; no physical serial device is opened."""
import os,select,threading,time
from bx_fixtures import packet,tool,crc16,error_reply
class Simulator:
    def __init__(self,master,firmware=1,faults=False,reject_markers=False,reset_fd=None,reset_reply=b'late-old-bytes\rRESETBE6F\r',reject_recovery_init=False):
        self.reset_reply=reset_reply;self.reject_recovery_init=reject_recovery_init;self.reset_fd=reset_fd;self.resets=0;self.master=master;self.firmware=firmware;self.faults=faults;self.reject_markers=reject_markers;self.done=threading.Event()
        self.handles={};self.commands=[];self.frames=[];self.index=0;self.error=None
        self.thread=threading.Thread(target=self.run);self.thread.start()
    def ascii(self,text):
        body=text.encode();os.write(self.master,body+('%04X'%crc16(body)).encode()+b'\r')
    def run(self):
        buffer=b''
        try:
            while not self.done.is_set():
                ready=select.select([self.master]+([] if self.reset_fd is None else [self.reset_fd]),[],[],0.05)[0]
                if self.reset_fd in ready:
                    os.read(self.reset_fd,4096);self.resets+=1;self.handles={};buffer=b''
                    self.commands.append({'command':'SERIAL_BREAK','argument':'SYNTHETIC_PTY_ONLY'})
                    # Full reset response with deliberately stale/noisy leading bytes.
                    os.write(self.master,self.reset_reply)
                if self.master not in ready:continue
                buffer+=os.read(self.master,4096)
                while b'\r' in buffer:
                    raw,buffer=buffer.split(b'\r',1)
                    if not raw:continue
                    if b':' in raw:
                        body=raw[:-4]
                        if crc16(body)!=int(raw[-4:],16):raise ValueError('host sent bad CRC')
                    else:body=raw
                    text=body.decode();cmd,_,arg=text.partition(':')
                    self.commands.append({'command':cmd.strip(),'argument':arg if cmd not in ['PVWR'] else 'ROM_BYTES_OMITTED','wire_bytes':len(raw)+1})
                    if cmd=='INIT':self.handles={};self.ascii('ERROR07' if self.resets and self.reject_recovery_init else 'OKAY')
                    elif cmd=='GETINFO':self.ascii('ERROR01')
                    elif cmd=='VER':self.ascii('SYNTHETIC Polaris Spectra firmware fixture')
                    elif cmd=='GET':self.ascii('ERROR34')
                    elif cmd.strip()=='APIREV':self.ascii('G.%03d.005'%self.firmware)
                    elif cmd=='PHRQ':
                        h=max(self.handles,default=0)+1;self.handles[h]={'init':False,'enabled':False};self.ascii('%02X'%h)
                    elif cmd=='PVWR':self.ascii('OKAY')
                    elif cmd=='PINIT':self.handles[int(arg[:2],16)]['init']=True;self.ascii('OKAY')
                    elif cmd=='PENA':self.handles[int(arg[:2],16)]['enabled']=True;self.ascii('OKAY')
                    elif cmd=='PDIS':self.handles[int(arg[:2],16)]['enabled']=False;self.ascii('OKAY')
                    elif cmd=='PHF':self.handles.pop(int(arg[:2],16),None);self.ascii('OKAY')
                    elif cmd=='PHSR':
                        mode=int(arg[:2],16)
                        selected=[(h,v) for h,v in self.handles.items() if mode==0 or (mode==2 and not v['init']) or (mode==3 and v['init'] and not v['enabled']) or (mode==4 and v['enabled'])]
                        self.ascii('%02X'%len(selected)+''.join('%02X%03X'%(h,1+(16 if v['init'] else 0)+(32 if v['enabled'] else 0)) for h,v in selected))
                    elif cmd=='PHINF':
                        h=int(arg[:2],16);flags=int(arg[2:],16)
                        basic='00000000'+'NDI'.ljust(12)+'001'+('FIX%05d'%h)+('31' if self.handles[h]['enabled'] else '11')
                        result=basic if flags&1 else ''
                        if flags&4:result+='SYNTHETIC-PART'.ljust(20)
                        if flags&0x20:result+='MOCK0001'+'010000'
                        self.ascii(result)
                    elif cmd in ['COMM','TSTART','TSTOP']:
                        if self.reset_fd is not None and cmd=='TSTART' and not self.resets:time.sleep(.5)
                        self.ascii('OKAY')
                    elif cmd=='BX':
                        flags=int(arg,16);self.index+=1
                        if self.reject_markers:
                            self.frames.append({'frame':self.index,'flags':flags,'wire_hex':error_reply(1).hex()})
                            self.ascii('ERROR01');continue
                        # Deliberately independent measured XYZ; not generated from any ROM.
                        tools=[tool(h,self.index,status=4 if h==2 and self.index%15==0 else (2 if h==2 and self.index%5==0 else 1),
                                    markers={} if h==2 and self.index%10==0 else {0:(3,[self.index/100,20+h,1000+h],0),2:(3,[30+h,40,1100+h],0)},
                                    pose=[1,0,0,0,100*h,200,300,0.1]) for h in sorted(self.handles)]
                        wire=packet(tools,flags)
                        if self.faults and self.index%7==0:
                            wire=wire[:-1]+bytes([wire[-1]^1])
                        self.frames.append({'frame':self.index,'flags':flags,'wire_hex':wire.hex()})
                        for at in range(0,len(wire),17):
                            os.write(self.master,wire[at:at+17]);time.sleep(0.0001)
                    else:self.ascii('ERROR01')
        except Exception as e:
            if not self.done.is_set():self.error=repr(e)
    def close(self):self.done.set();self.thread.join(timeout=3)
