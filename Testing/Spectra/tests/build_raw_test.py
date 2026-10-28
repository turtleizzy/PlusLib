from pathlib import Path
import shlex,subprocess
c=Path(__file__).resolve().parents[1]
base=c/'build-pluslib/src/PlusDataCollection/CMakeFiles/vtkPlusDataCollection.dir'
f={k:v.strip() for line in (base/'flags.make').read_text().splitlines() if ' = ' in line for k,v in [line.split(' = ',1)]}
link=shlex.split((base/'link.txt').read_text());after=max(i for i,x in enumerate(link) if x.endswith('.o'))+1
cmd=['c++']+shlex.split(f['CXX_INCLUDES'])+shlex.split(f['CXX_FLAGS'])+['-I'+str(c/'src/PlusLib/src/PlusDataCollection/NDICAPITracking'),str(c/'tests/raw_fixture.cxx'),'-L'+str(c/'bin'),'-lvtkPlusDataCollection']+link[after:]+['-o',str(c/'bin/raw_fixture')]
subprocess.run(cmd,check=True)

subprocess.run(['c++','-std=c++11','-shared','-fPIC',str(c/'tests/raw_fault.cxx'),'-ldl','-o',str(c/'bin/raw_fault.so')],check=True)
