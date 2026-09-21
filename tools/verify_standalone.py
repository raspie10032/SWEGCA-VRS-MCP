"""Inspect actual release archives and first-party source integrity."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import tarfile
import zipfile


REQUIRED_ENTRY_POINTS = (
    'swegca-vrs-mcp = swegca_vrs2.server:main',
    'swegca-vrs2-mcp = swegca_vrs2.server:main',
    'swegca-vrs2-codex = swegca_vrs2.layered:main',
    'swegca-vrs2-hook = swegca_vrs2.conversation_hooks:hook_main',
    'swegca-vrs2-codex-hooks = swegca_vrs2.codex_hooks:main',
)


def verify(root, dist):
    manifest=json.loads((root/'NATIVE_VRS2_PORT.json').read_text(encoding='utf-8'))
    for row in manifest['records']:
        raw=(root/'src/swegca_vrs2/engine'/(row['module']+'.py')).read_bytes()
        assert hashlib.sha256(raw).hexdigest()==row['port_sha256'], row['module']
        assert not raw.startswith(b'\xef\xbb\xbf')
    results=[]
    for archive in sorted(dist.iterdir()):
        if archive.suffix=='.whl':
            with zipfile.ZipFile(archive) as z:
                names=z.namelist()
                entry=next(n for n in names if n.endswith('.dist-info/entry_points.txt'))
                entry_points=z.read(entry).decode()
                for required_entry in REQUIRED_ENTRY_POINTS:
                    assert required_entry in entry_points, required_entry
                package_python={n:z.read(n) for n in names
                    if '/swegca_vrs2/' in '/'+n and n.endswith('.py')}
        elif archive.name.endswith('.tar.gz'):
            with tarfile.open(archive) as tar:
                names=tar.getnames()
                package_python={n:tar.extractfile(n).read() for n in names
                    if '/src/swegca_vrs2/' in '/'+n and n.endswith('.py')}
        else:
            continue
        forbidden=[n for n in names if any(v in n.lower() for v in
            ('swegca_vrs_mcp/','swegca_vrs2/harness/','swegca_vrs2/adapter.py',
             'local-data','memory.sqlite','.sqlite3'))]
        assert not forbidden, forbidden
        sqlite_imports=[]
        for name, raw in package_python.items():
            tree=ast.parse(raw.decode('utf-8'), filename=name)
            for node in ast.walk(tree):
                imports=([alias.name for alias in node.names]
                    if isinstance(node, ast.Import) else
                    [node.module or ''] if isinstance(node, ast.ImportFrom) else [])
                if any(value == 'sqlite3' or value.startswith('sqlite3.') for value in imports):
                    sqlite_imports.append(f'{name}:{node.lineno}')
        assert not sqlite_imports, sqlite_imports
        for required in ('swegca_vrs2/store.py','swegca_vrs2/native_journal.py',
                         'swegca_vrs2/server.py','swegca_vrs2/layered.py',
                         'swegca_vrs2/conversation_hooks.py','swegca_vrs2/codex_hooks.py',
                         'swegca_vrs2/engine/mosaic_vrs_event_signal.py'):
            assert any(n.endswith(required) for n in names), required
        results.append(dict(file=archive.name,bytes=archive.stat().st_size,
            sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
            sqlite_imports=0, database_artifacts=0, retired_adapter_files=0))
    assert len(results)==2, results
    return dict(native_port_files=len(manifest['records']),artifacts=results,status='PASS')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--dist',type=Path,default=Path('dist'))
    args=p.parse_args()
    print(json.dumps(verify(Path(__file__).resolve().parents[1],args.dist),indent=2))
