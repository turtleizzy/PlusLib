"""Build this PlusLib plus an explicit companion ndicapi against existing dependencies.
All outputs are confined to this ignored test workspace; no installation/launch.
"""
from pathlib import Path
import argparse,re,subprocess,xml.etree.ElementTree as ET
T=Path(__file__).resolve().parent;REPO=T.parents[1]
a=argparse.ArgumentParser(description=__doc__);a.add_argument('--dependencies',type=Path,required=True,help='Existing PlusBuild binary/dependency directory');a.add_argument('--ndicapi-source',type=Path,required=True);a.add_argument('--jobs',type=int,default=4);args=a.parse_args()
D=args.dependencies.resolve();N=args.ndicapi_source.resolve();assert (N/'ndicapi_bx_capture.h').is_file(),'Use the reviewed companion ndicapi patches';assert args.jobs>0
for name in ['audit','bin','lib','src','data','roms','recordings','config/generated']:(T/name).mkdir(parents=True,exist_ok=True)
for name,target in [('PlusLib',REPO),('ndicapi',N)]:
 p=T/'src'/name
 if p.is_symlink():assert p.resolve()==target
 else:assert not p.exists();p.symlink_to(target,target_is_directory=True)
def call(command,name):
 with (T/'audit'/name).open('w') as f:r=subprocess.run(command,stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise SystemExit('FAIL '+str(T/'audit'/name))
 print('PASS',name,flush=True)
call(['cmake','-S',str(N),'-B',str(T/'build-ndicapi'),'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_LIBRARY_OUTPUT_DIRECTORY='+str(T/'bin'),'-DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF'],'configure-ndicapi.log')
call(['cmake','--build',str(T/'build-ndicapi'),'--target','ndicapi','-j',str(args.jobs)],'build-ndicapi.log')
cache={}
for line in (D/'PlusLib-bin/CMakeCache.txt').read_text().splitlines():
 m=re.match(r'([^:#]+):(BOOL|STRING|PATH|FILEPATH)=(.*)',line)
 if m and (m[1].startswith('PLUS_USE_') or m[1] in ['CMAKE_BUILD_TYPE','CMAKE_CXX_COMPILER','CMAKE_CXX_STANDARD','CMAKE_CXX_FLAGS','VTK_DIR','ITK_DIR','IGSIO_DIR','OpenIGTLinkIO_DIR','OpenIGTLink_DIR','PLUS_BUILD_WIDGETS','PLUSBUILD_BUILD_PlusLib_TOOLS','BUILD_SHARED_LIBS']):cache[m[1]]=(m[2],m[3])
for k,v in {'BUILD_TESTING':('BOOL','OFF'),'BUILD_SHARED_LIBS':('BOOL','ON'),'PLUSBUILD_BUILD_PlusLib_TOOLS':('BOOL','ON'),'PLUS_OFFLINE_BUILD':('BOOL','ON'),'CMAKE_EXPORT_PACKAGE_REGISTRY':('BOOL','OFF'),'CMAKE_EXPORT_NO_PACKAGE_REGISTRY':('BOOL','ON'),'ndicapi_DIR':('PATH',str(T/'build-ndicapi')),'CMAKE_LIBRARY_OUTPUT_DIRECTORY':('PATH',str(T/'bin')),'CMAKE_RUNTIME_OUTPUT_DIRECTORY':('PATH',str(T/'bin')),'CMAKE_ARCHIVE_OUTPUT_DIRECTORY':('PATH',str(T/'lib')),'PLUSLIB_DATA_DIR':('PATH',str(T/'data'))}.items():cache[k]=v
initial=T/'initial-cache.cmake';initial.write_text('\n'.join('set('+k+' [==['+v+']==] CACHE '+typ+' "" FORCE)' for k,(typ,v) in cache.items())+'\n')
call(['cmake','-S',str(REPO),'-B',str(T/'build-pluslib'),'-C',str(initial)],'configure-pluslib.log')
call(['cmake','--build',str(T/'build-pluslib'),'--target','PlusServer','-j',str(args.jobs)],'build-pluslib.log')
config=ET.Element('PlusConfig',LogLevel='3',DeviceSetConfigurationDirectory=str(T/'config/generated'),OutputDirectory=str(T/'audit/PlusLogs'),ScriptsDirectory=str(REPO/'src/scripts'))
(T/'audit/PlusLogs').mkdir(exist_ok=True);ET.ElementTree(config).write(T/'bin/PlusConfig.xml',encoding='utf-8',xml_declaration=True)
# These opaque synthetic ROMs are only for the guarded PTY peer. Never use them on hardware.
for i in [1,2]:(T/f'roms/test{i}.rom').write_bytes(bytes(1024))
for name in ['spectra-markers-off.xml','spectra-markers-enabled.xml','spectra-raw-gui-test.xml']:
 tree=ET.parse(T/'config'/name)
 for node in tree.findall('.//Device[@MarkerSidecarDirectory]'):node.set('MarkerSidecarDirectory',str(T/'recordings'))
 for node in tree.findall('.//DataSource[@RomFile]'):node.set('RomFile',str(T/'roms'/node.get('RomFile').removeprefix('REPLACE_WITH_PRIVATE_')))
 tree.write(T/'config/generated'/name,encoding='utf-8',xml_declaration=True)
print('Built:',T/'bin/PlusServer')
