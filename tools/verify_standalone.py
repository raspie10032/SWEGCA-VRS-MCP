"""Inspect actual release archives and first-party source integrity."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile
import zipfile


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
                assert 'swegca_vrs2.server:main' in z.read(entry).decode()
        elif archive.name.endswith('.tar.gz'):
            with tarfile.open(archive) as tar:
                names=tar.getnames()
        else:
            continue
        forbidden=[n for n in names if any(v in n.lower() for v in ('hermes','agent_service','agent_client','swegca_vrs_mcp/','local-data','memory.sqlite'))]
        assert not forbidden, forbidden
        for required in ('swegca_vrs2/store.py','swegca_vrs2/server.py','swegca_vrs2/engine/mosaic_vrs_event_signal.py'):
            assert any(n.endswith(required) for n in names), required
        results.append(dict(file=archive.name,bytes=archive.stat().st_size,sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),hermes_files=0))
    assert len(results)==2, results
    return dict(native_port_files=len(manifest['records']),artifacts=results,status='PASS')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--dist',type=Path,default=Path('dist'))
    args=p.parse_args()
    print(json.dumps(verify(Path(__file__).resolve().parents[1],args.dist),indent=2))
