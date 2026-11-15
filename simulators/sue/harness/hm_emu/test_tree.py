"""Build and run independent tree causality/local service unit tests."""
import subprocess
import run
out=run.ROOT/'results/build';out.mkdir(parents=True,exist_ok=True)
cmd=['g++','-std=c++17','-O2','-include','stdint.h',str(run.HERE/'test_tree_collective.cc'),'-I'+str(run.SIM/'build/include'),'-L'+str(run.SIM/'build/lib'),'-Wl,-rpath,'+str(run.SIM/'build/lib'),'-lns3.36.1-core-default','-o',str(out/'test-hm-tree')]
subprocess.run(cmd,check=True);subprocess.run([str(out/'test-hm-tree'),str(out/'test-hm-tree.csv')],check=True)
print('PASS: fair service, independent subtrees, payload conservation and concurrent trees')
