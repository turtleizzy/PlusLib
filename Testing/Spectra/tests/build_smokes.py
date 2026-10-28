from pathlib import Path
import shlex,subprocess
c=Path(__file__).resolve().parents[1]
def build(target,source,name):
    base=c/'build-pluslib/src'/target/'CMakeFiles'/('vtk'+target+'.dir')
    flags={k:v.strip() for line in (base/'flags.make').read_text().splitlines() if ' = ' in line for k,v in [line.split(' = ',1)]}
    link=shlex.split((base/'link.txt').read_text())
    after=max(i for i,x in enumerate(link) if x.endswith('.o'))+1
    command=['/usr/bin/c++']+shlex.split(flags['CXX_INCLUDES'])+shlex.split(flags['CXX_FLAGS'])+[
        '-I'+str(c/'src/PlusLib/src/PlusDataCollection/NDICAPITracking'),
        '-I'+str(c/'src/PlusLib/src/PlusOpenIGTLink'),
        str(source),'-L'+str(c/'bin'),'-lvtk'+target]+link[after:]+['-o',str(c/'bin'/name)]
    subprocess.run(command,check=True)
build('PlusDataCollection',c/'tests/recorder_fixture.cxx','recorder_fixture')
build('PlusOpenIGTLink',c/'tests/regression/test_client_info_xml.cxx','test_client_info_xml')
subprocess.run(['/usr/bin/c++','-shared','-fPIC',str(c/'tests/pty_only_shim.cxx'),'-ldl','-o',str(c/'bin/pty_only_shim.so')],check=True)

build('PlusDataCollection',c/'tests/recorder_lifecycle_fixture.cxx','recorder_lifecycle_fixture')
subprocess.run(['/usr/bin/c++','-std=c++17','-shared','-fPIC',str(c/'tests/fault_shim.cxx'),'-ldl','-pthread','-o',str(c/'bin/fault_shim.so')],check=True)
