from pathlib import Path
import shlex,subprocess
T=Path(__file__).resolve().parents[1]
def build(component,source,name,extra=()):
 b=T/'build-pluslib/src'/component/'CMakeFiles'/('vtk'+component+'.dir');f={k:v.strip() for line in (b/'flags.make').read_text().splitlines() if ' = ' in line for k,v in [line.split(' = ',1)]};link=shlex.split((b/'link.txt').read_text());after=max(i for i,x in enumerate(link) if x.endswith('.o'))+1
 subprocess.run(['c++',*shlex.split(f['CXX_INCLUDES']),*shlex.split(f['CXX_FLAGS']),'-pthread',*extra,'-I'+str(T/'src/PlusLib/src/PlusServer'),str(T/'tests'/source),'-L'+str(T/'bin'),'-lvtk'+component,*link[after:],'-o',str(T/'bin'/name)],check=True)
build('PlusDataCollection','broadcast_cursor_fixture.cxx','broadcast_cursor_fixture')
build('PlusServer','reusable_socket_fixture.cxx','socket_candidate')
build('PlusServer','reusable_socket_fixture.cxx','socket_baseline',('-DBASELINE_SOCKET',))
