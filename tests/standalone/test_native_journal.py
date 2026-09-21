"""Native VRS persistence has one checksummed journal and checkpoint path."""
import ast
import json
from pathlib import Path

import pytest

from swegca_vrs2.native_journal import FILE_MAGIC, MANIFEST, NativeJournal
from swegca_vrs2.store import Main


def record(number):
    return dict(request_id=f'native-{number}', text=f'원경험 {number}',
                source=f'conversation:{number}', revision='1')


def test_native_store_files_restart_and_four_stage_replay(tmp_path):
    main = Main(tmp_path, allow_ingest=True)
    result = main.ingest(record(1))
    pair = main.pair.snapshot_id
    main.checkpoint()
    main.close()

    assert (tmp_path / MANIFEST).is_file()
    assert not list(tmp_path.rglob('*.sqlite*'))
    restored = Main(tmp_path, allow_ingest=True)
    try:
        recalled = restored.recall(result['episode_id'], pair)
        activation = recalled['receipt']['activation']
        assert activation.stage_order == ('deja_vu', 'recall', 'replay', 're_evidence')
        assert activation.replay.episodes[0].episode_id == result['episode_id']
    finally:
        restored.close()


def test_incomplete_tail_is_discarded_but_committed_frame_survives(tmp_path):
    main = Main(tmp_path, allow_ingest=True, defer_checkpoints=True)
    result = main.ingest(record(1))
    head = main.journal.head_path
    main.lock.release(); main.closed = True
    with open(head, 'ab') as stream:
        stream.write(b'\x20\x00\x00')

    restored = Main(tmp_path, allow_ingest=True, defer_checkpoints=True)
    try:
        assert restored.memory.episode_count == 1
        assert restored.memory.episode(result['episode_id']).steps[0].observation['text'] == '원경험 1'
        assert head.stat().st_size > len(FILE_MAGIC)
    finally:
        restored.close()


def test_checksum_corruption_fails_closed_and_releases_owner(tmp_path):
    main = Main(tmp_path, allow_ingest=True)
    main.ingest(record(1)); main.close()
    head = main.journal.head_path
    body = bytearray(head.read_bytes()); body[-33] ^= 1; head.write_bytes(body)
    for _ in range(2):
        with pytest.raises(ValueError, match='integrity_failed'):
            Main(tmp_path, allow_ingest=True)


def test_batch_has_one_pair_and_native_sequences_are_contiguous(tmp_path):
    main = Main(tmp_path, allow_ingest=True)
    try:
        main.ingest_many([record(i) for i in range(20)])
        rows = list(main.journal.rows())
        assert [row[0] for row in rows] == list(range(1, 21))
        assert len({row[4] for row in rows}) == 1
        manifest = json.loads((tmp_path / MANIFEST).read_text(encoding='utf-8'))
        assert manifest['schema'] == 'swegca-vrs2-native-store-v1'
    finally:
        main.close()


def test_runtime_package_has_no_sqlite_import_or_database_artifact_names():
    root = Path(__file__).resolve().parents[2] / 'src' / 'swegca_vrs2'
    offenders = []
    for path in root.rglob('*.py'):
        tree = ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
        for node in ast.walk(tree):
            names = ([alias.name for alias in node.names]
                     if isinstance(node, ast.Import) else
                     [node.module or ''] if isinstance(node, ast.ImportFrom) else [])
            if any(name == 'sqlite3' or name.startswith('sqlite3.') for name in names):
                offenders.append(f'{path.relative_to(root)}:{node.lineno}')
    assert offenders == []
    forbidden_suffixes = ('.sqlite', '.sqlite3', '.db')
    assert [str(path.relative_to(root)) for path in root.rglob('*')
            if path.is_file() and path.name.casefold().endswith(forbidden_suffixes)] == []


def test_distribution_and_runtime_versions_match():
    import swegca_vrs2
    pyproject = (Path(__file__).resolve().parents[2] / 'pyproject.toml').read_text(encoding='utf-8')
    assert f'version = "{swegca_vrs2.__version__}"' in pyproject
