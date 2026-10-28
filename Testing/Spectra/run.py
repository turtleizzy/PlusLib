"""Synthetic/socketpair regressions; --network adds owned loopback/guarded PTY tests."""
from pathlib import Path
import argparse,os,subprocess,sys
T=Path(__file__).resolve().parent;a=argparse.ArgumentParser(description=__doc__);a.add_argument('--network',action='store_true');a.add_argument('--rmt-root',type=Path);args=a.parse_args()
assert (T/'bin/PlusServer').is_file(),'Run build.py first'
env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',LD_LIBRARY_PATH=str(T/'bin'));env.pop('LD_PRELOAD',None)
def run(script):
 with (T/'audit'/(script+'.log')).open('w') as out:r=subprocess.run([sys.executable,'-B',str(T/'tests'/script)],env=env,cwd=T,stdout=out,stderr=subprocess.STDOUT)
 if r.returncode:raise SystemExit('FAIL '+str(T/'audit'/(script+'.log')))
 print('PASS',script,flush=True)
for script in ['build_smokes.py','build_raw_test.py','build_broadcast_test.py','test_recorder.py','test_consumer.py','test_lifecycle_faults.py','test_raw.py','test_broadcast_cursor.py']:run(script)
with (T/'audit/client-info-xml.log').open('w') as out:subprocess.run([str(T/'bin/test_client_info_xml')],env=env,check=True,stdout=out,stderr=subprocess.STDOUT)
if args.network:
 for script in ['test_raw_tcp.py','test_endpoint_startup.py','test_failed_server_cleanup.py']:run(script)
if args.rmt_root:
 assert args.network,'Interop needs --network for owned temporary endpoints';env['RMT_ROOT']=str(args.rmt_root.resolve());run('test_raw_recovery_pty.py')
