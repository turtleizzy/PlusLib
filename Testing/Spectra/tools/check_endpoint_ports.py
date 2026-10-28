"""Launcher-only preflight. Bind probes only: no client connection or serial I/O."""
import socket,sys,xml.etree.ElementTree as ET
try:
 root=ET.parse(sys.argv[1]);ports=[]
 for server in root.findall('.//PlusOpenIGTLinkServer'):
  if server.get('ListeningPort'):ports.append(('TRANSFORM',int(server.get('ListeningPort'))))
 for device in root.findall('.//Device'):
  if device.get('EnableRawEndpoint','false').lower()=='true':ports.append(('raw',int(device.get('RawPort','18945'))))
 seen=set()
 for kind,port in ports:
  if port in seen:raise ValueError('Duplicate listening port '+str(port))
  seen.add(port)
  with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as probe:
   probe.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
   try:probe.bind(('0.0.0.0',port))
   except OSError as e:raise RuntimeError(f'{kind} port {port} unavailable: errno={e.errno} ({e.strerror}). PlusServer was not started; serial device untouched.') from e
except Exception as e:print('run_candidate.sh preflight:',str(e),file=sys.stderr);sys.exit(2)
