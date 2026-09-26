"""Opt-in local Linux/systemd check, using only an isolated 64MiB test group."""
import pathlib
import subprocess
import sys
exe=str(pathlib.Path(sys.argv[1]).resolve())
unguarded=subprocess.run([exe,'bounded-verify'],capture_output=True,text=True,timeout=20)
assert unguarded.returncode!=0 and 'limits do not match' in unguarded.stderr
normal=subprocess.run([exe,'verify'],capture_output=True,text=True,timeout=20)
assert normal.returncode==0,(normal.stdout,normal.stderr)
assert 'verified memory.max=67108864 memory.swap.max=0 CPUs=6,7' in normal.stdout
print(normal.stdout,end='')
exceeded=subprocess.run([exe,'oom'],capture_output=True,text=True,timeout=20)
assert exceeded.returncode!=0,exceeded.stdout
assert 'verified memory.max=67108864' in exceeded.stdout
assert 'touching 128MiB' in exceeded.stdout
unit=next(line.rsplit('/',1)[-1] for line in exceeded.stdout.splitlines() if line.startswith('cgroup='))
journal=subprocess.check_output(['journalctl','--user','--unit',unit,'--no-pager','-n','20','-o','cat'],text=True)
assert 'oom-kill' in journal or 'OOM killer' in journal,(exceeded.returncode,journal)
print(f'PASS: isolated cgroup OOM confirmed for {unit}; launcher exit={exceeded.returncode}')
