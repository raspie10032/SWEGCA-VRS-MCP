#!/usr/bin/env python3
"""CLI routing fixtures only; no account or live backend calls."""
import json,os,pathlib,subprocess,sys,tempfile
exe=pathlib.Path(sys.argv[1]).resolve()
checks=0
def check(value):
    global checks
    checks+=1
    assert value
with tempfile.TemporaryDirectory(prefix='swegca-wrapper-') as tmp:
    root=pathlib.Path(tmp)
    for role in ('backend','host'):
        p=root/(role+' fixture')
        p.write_text('#!'+sys.executable+'\nimport json,sys\nprint(json.dumps({"role":'+repr(role)+',"args":sys.argv[1:]}))\n')
        p.chmod(0o700)
    config={'backend':str(root/'backend fixture'),'host':str(root/'host fixture'),'proxy':'/fixture/proxy',
            'vrs':'/fixture/vrs','mode':'open','root':'/fixture/root','resourceConfig':'/fixture/resources',
            'proxyConfig':'/fixture/proxy-config'}
    path=root/'config.json';path.write_text(json.dumps(config))
    env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(path))
    def run(args):return subprocess.run([str(exe),*args],env=env,capture_output=True,timeout=5)
    for args in (['--version'],['--help'],['app-server','--help'],['app-server','daemon','version'],
                 ['app-server','generate-json-schema','--out','/unused'],['exec','app-server'],
                 ['-c','note="app-server"','--version']):
        result=run(args);check(result.returncode==0 and result.stderr==b'')
        check(json.loads(result.stdout)=={'role':'backend','args':args})
    prefix=[config[key] for key in ('proxy','vrs','mode','root','resourceConfig','proxyConfig','backend')]
    for args in (['app-server'],['-c','features.code_mode_host=true','app-server','--analytics-default-enabled'],
                 ['app-server','-c','note="spaces ; $(no shell)"','--listen','stdio://'],
                 ['--enable','feature','app-server','--stdio'],['--config=x=1','app-server']):
        result=run(args);check(result.returncode==0 and result.stderr==b'')
        check(json.loads(result.stdout)=={'role':'host','args':prefix+args})
    for args in (['app-server','--listen','ws://127.0.0.1:1234'],['app-server','--listen=off'],
                 ['app-server','proxy'],['app-server','daemon','start'],['--unknown','app-server'],
                 ['-c'],['app-server','--config']):
        result=run(args);check(result.returncode==1 and result.stdout==b'')
    config['backend']=str(exe);path.write_text(json.dumps(config))
    result=run(['--version']);check(result.returncode==1 and b'recursive' in result.stderr)
    path.write_text(' '*65537)
    result=run(['--version']);check(result.returncode==1 and b'too large' in result.stderr)
print(f'codex wrapper tests: {checks} checks passed')
