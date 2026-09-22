"""Verify the source runtime and its first-party lineage without wheel archives."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import tomllib


ENTRY_POINTS = {
    'swegca-vrs-mcp': 'swegca_vrs2.server:main',
    'swegca-vrs2-mcp': 'swegca_vrs2.server:main',
    'swegca-vrs2-codex': 'swegca_vrs2.layered:main',
    'swegca-vrs2-hook': 'swegca_vrs2.conversation_hooks:hook_main',
    'swegca-vrs2-codex-hooks': 'swegca_vrs2.codex_hooks:main',
}


def verify(root):
    root = Path(root)
    manifest = json.loads((root / 'NATIVE_VRS2_PORT.json').read_text(encoding='utf-8'))
    package = root / 'src/swegca_vrs2'
    files = sorted(package.rglob('*.py'))
    assert len(files) == 65, f'product source count changed: {len(files)}'
    ported = {path.stem for path in (package / 'engine').glob('*.py')} - {'__init__'}
    declared = {row['module'] for row in manifest['records']}
    assert len(declared) == len(manifest['records'])
    assert ported - declared == {'mosaic_evidence_accumulator',
                                'mosaic_semantic_family_directory'}
    for row in manifest['records']:
        raw = (package / 'engine' / (row['module'] + '.py')).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == row['port_sha256'], row['module']
        assert not raw.startswith(b'\xef\xbb\xbf'), row['module']
        if isinstance(row['definitions'], list):
            declared_names = []
            for node in ast.parse(raw, filename=row['module']).body:
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                    declared_names.append(node.name)
                elif isinstance(node, (ast.Assign, ast.AnnAssign)):
                    targets = node.targets if isinstance(node, ast.Assign) else (node.target,)
                    declared_names.extend(target.id for target in targets if isinstance(target, ast.Name))
            assert len(declared_names) == len(set(declared_names)), row['module']
            assert set(declared_names) == set(row['definitions']), row['module']

    project = tomllib.loads((root / 'pyproject.toml').read_text(encoding='utf-8'))
    assert project['project']['scripts'] == ENTRY_POINTS
    forbidden = []
    missing_local_modules = []
    for path in files:
        tree = ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom) and node.level and node.module:
                parent = path.parent
                for _ in range(node.level - 1):
                    parent = parent.parent
                imported = parent.joinpath(*node.module.split('.'))
                if not imported.with_suffix('.py').is_file() and not (imported / '__init__.py').is_file():
                    missing_local_modules.append(
                        f'{path.relative_to(root)}:{node.lineno} -> {node.module}')
            names = ([alias.name for alias in node.names] if isinstance(node, ast.Import)
                     else [node.module or ''] if isinstance(node, ast.ImportFrom)
                     else [])
            if any(name == 'sqlite3' or name.startswith('sqlite3.')
                   or name == 'filelock' or name.startswith('filelock.')
                   or 'hermes' in name.lower() for name in names):
                forbidden.append(f'{path.relative_to(root)}:{node.lineno}')
    assert not forbidden, forbidden
    assert not missing_local_modules, missing_local_modules
    wheel_files = sorted(str(path.relative_to(root)) for path in root.rglob('*.whl'))
    assert not wheel_files, wheel_files
    assert not list(package.rglob('*.sqlite3'))
    return dict(status='PASS', native_port_files=len(manifest['records']),
                product_source_files=len(files), product_wheel_files=0,
                forbidden_imports=0, entry_points=ENTRY_POINTS)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    print(json.dumps(verify(Path(__file__).resolve().parents[1]), indent=2))
