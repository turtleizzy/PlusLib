"""Create a new pinned dependency checkout and apply the reviewed patch series.
No existing checkout is modified. This stages a verified source tree without
requiring a global Git identity; use git am manually if separate dependency
commit history is desired instead.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess
T=Path(__file__).resolve().parent;m=json.loads((T/'DEPENDENCIES.json').read_text())['ndicapi'];a=argparse.ArgumentParser(description=__doc__);a.add_argument('--dest',type=Path,required=True);a.add_argument('--source',default=m['upstream_url'],help='Clone URL or existing local upstream checkout');args=a.parse_args();assert not args.dest.exists(),'Refusing to overwrite a dependency checkout'
subprocess.run(['git','clone','--no-checkout',args.source,str(args.dest)],check=True)
subprocess.run(['git','-C',str(args.dest),'checkout','-b','spectra-raw-v1-dependency',m['base_commit']],check=True)
for entry in m['patches']:
 p=T/entry['path'];assert hashlib.sha256(p.read_bytes()).hexdigest()==entry['sha256'];subprocess.run(['git','-C',str(args.dest),'apply','--index',str(p)],check=True)
tree=subprocess.check_output(['git','-C',str(args.dest),'write-tree'],text=True).strip();assert tree==m['result_tree'],'Patched source tree mismatch';print('Verified companion source tree:',tree)
