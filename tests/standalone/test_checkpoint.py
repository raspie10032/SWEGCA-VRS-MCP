"""Journal remains authoritative across checkpoint tails and corruption."""
import sqlite3

import pytest

from swegca_vrs2.store import Main


def _ingest(main, label):
    return main.ingest(dict(request_id=label, text='원문 ' + label,
        source='test:' + label, revision='1'))


def test_checkpoint_plus_unsealed_journal_tail_restores_exact_pair(tmp_path):
    main = Main(tmp_path / 'state', allow_ingest=True)
    try:
        first = _ingest(main, 'first')
        _ingest(main, 'second')
        assert main.checkpoint()['checkpoint_sequence'] == 2
        last = _ingest(main, 'last')
        expected = main.pair.snapshot_id
        # Simulate process loss after the durable observation commit but before
        # clean shutdown could write another checkpoint.
        main.ready = False
    finally:
        main.close()
    restored = Main(tmp_path / 'state')
    try:
        assert restored.pair.snapshot_id == expected
        assert restored.status()['checkpoint_sequence'] == 2
        assert restored.status()['journal_sequence'] == 3
        assert restored.memory.episode(first['episode_id']).steps[0].observation['text'] == '원문 first'
        assert restored.memory.episode(last['episode_id']).steps[0].observation['text'] == '원문 last'
    finally:
        restored.close()


def test_bad_checkpoint_fails_closed_and_rebuild_from_valid_journal(tmp_path):
    state = tmp_path / 'state'
    main = Main(state, allow_ingest=True)
    try:
        first = _ingest(main, 'first')
        expected = main.pair.snapshot_id
    finally:
        main.close()
    with sqlite3.connect(state / 'memory.sqlite3') as db:
        db.execute("UPDATE checkpoint SET checksum='bad'")
    with pytest.raises(ValueError, match='checkpoint_checksum_integrity_failed'):
        Main(state)
    recovered = Main(state, rebuild_checkpoint=True)
    try:
        assert recovered.pair.snapshot_id == expected
        assert recovered.memory.episode(first['episode_id']).steps[0].observation['text'] == '원문 first'
    finally:
        recovered.close()


def test_failed_checkpoint_commit_keeps_journal_and_main_unchanged(tmp_path):
    main = Main(tmp_path / 'state', allow_ingest=True)
    try:
        _ingest(main, 'first')
        original_db = main.db
        class FailingCommit:
            def __getattr__(self, key):
                return getattr(original_db, key)
            def execute(self, query, *args):
                if query == 'COMMIT':
                    raise sqlite3.OperationalError('injected checkpoint failure')
                return original_db.execute(query, *args)
        pair = main.pair.snapshot_id
        main.db = FailingCommit()
        with pytest.raises(sqlite3.OperationalError, match='injected checkpoint failure'):
            main.checkpoint()
        main.db = original_db
        assert main.pair.snapshot_id == pair
        assert main.checkpoint_sequence == 0
        assert original_db.execute('SELECT COUNT(*) FROM checkpoint').fetchone()[0] == 0
        assert original_db.execute('SELECT COUNT(*) FROM observations').fetchone()[0] == 1
    finally:
        main.db = original_db
        main.close()
