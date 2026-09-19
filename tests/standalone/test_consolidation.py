"""Derived navigation retains exact parents and current opposing evidence."""
import json
import sqlite3
import subprocess
import sys

import pytest

from swegca_vrs2.store import Main


def test_manual_group_is_idempotent_and_originals_survive_revision_restart(tmp_path):
    main = Main(tmp_path / 'grouped', allow_ingest=True, allow_maintenance=True)
    try:
        a = main.ingest(dict(request_id='a', text='원문 A 청색', source='test:a',
            revision='1', outcome='success', proposition='Color:Blue', polarity='support'))
        b = main.ingest(dict(request_id='b', text='원문 B 청색 아님', source='test:b',
            revision='1', outcome='failure', proposition='Color:Blue', polarity='refute'))
        original_pair = main.pair.snapshot_id
        request = dict(parent_episode_ids=[b['episode_id'], a['episode_id']],
                       expected_pair_snapshot_id=original_pair)
        group = main.consolidate(request)
        assert group['status'] == 'consolidation_recorded'
        assert main.consolidate(request)['status'] == 'consolidation_unchanged'
        assert main.pair.snapshot_id == original_pair
        assert len(main.memory.records) == 2
        identifier = group['derived_experience_id']
        first = main.recall(identifier, original_pair)
        assert {row.episode_id for row in first['receipt']['activation'].recall.candidates} == {
            a['episode_id'], b['episode_id']}
        navigation = first['consolidated_experiences'][0]
        assert navigation['current_conflicting_propositions'] == ['Color:Blue']
        assert navigation['source_expansion_required'] is True
        assert navigation['independent_observation_added'] is False
        assert {tuple(parent['source_addresses']) for parent in navigation['parents']} == {
            ('test:a',), ('test:b',)}
        main.ingest(dict(request_id='c', text='원문 B 정정 청색', source='test:b',
            revision='2', outcome='uncertain', proposition='Color:Blue',
            polarity='support', supersedes=b['episode_id']))
        current_pair = main.pair.snapshot_id
        fresh = main.recall(identifier, current_pair)
        navigation = fresh['consolidated_experiences'][0]
        assert navigation['current_conflicting_propositions'] == []
        assert next(p for p in navigation['parents'] if p['episode_id'] == b['episode_id'])['superseded_by']
        assert main.memory.episode(b['episode_id']).steps[0].observation['text'] == '원문 B 청색 아님'
    finally:
        main.close()
    reopened = Main(tmp_path / 'grouped', allow_maintenance=True)
    try:
        assert reopened.status()['consolidated_experience_count'] == 1
        restored = reopened.recall(identifier, reopened.pair.snapshot_id)
        assert restored['consolidated_experiences'][0]['parent_episode_ids'] == tuple(sorted([
            a['episode_id'], b['episode_id']]))
        assert reopened.memory.episode(a['episode_id']).steps[0].observation['text'] == '원문 A 청색'
        assert reopened.memory.episode(b['episode_id']).steps[0].observation['text'] == '원문 B 청색 아님'
    finally:
        reopened.close()


def test_group_write_requires_maintenance_and_corruption_fails_closed(tmp_path):
    main = Main(tmp_path / 'grouped', allow_ingest=True)
    try:
        a = main.ingest(dict(request_id='a', text='원문 A', source='test:a', revision='1'))
        b = main.ingest(dict(request_id='b', text='원문 B', source='test:b', revision='1'))
        request = dict(parent_episode_ids=[a['episode_id'], b['episode_id']],
                       expected_pair_snapshot_id=main.pair.snapshot_id)
        with pytest.raises(ValueError, match='maintenance_disabled'):
            main.consolidate(request)
        main.allow_maintenance = True
        main.consolidate(request)
        main.db.execute("UPDATE consolidations SET checksum='bad'")
    finally:
        main.close()
    with pytest.raises(ValueError, match='consolidation_checksum_integrity_failed'):
        Main(tmp_path / 'grouped')


def test_offline_maintenance_compacts_storage_without_losing_originals(tmp_path):
    state = tmp_path / 'offline'
    main = Main(state, allow_ingest=True)
    try:
        a = main.ingest(dict(request_id='a', text='원문 A ' * 200,
            source='test:a', revision='1', outcome='failure',
            proposition='Thing:OK', polarity='support', metadata={'condition': '원본'}))
        b = main.ingest(dict(request_id='b', text='원문 B ' * 200,
            source='test:b', revision='1', outcome='conflict',
            proposition='Thing:OK', polarity='refute'))
        pair = main.pair.snapshot_id
    finally:
        main.close()
    command = [sys.executable, '-m', 'swegca_vrs2.maintenance', '--state-dir', str(state)]
    grouped = subprocess.run([*command, 'consolidate', '--expected-pair', pair,
        a['episode_id'], b['episode_id']], capture_output=True, text=True, timeout=15)
    assert grouped.returncode == 0, grouped.stderr
    assert json.loads(grouped.stdout)['status'] == 'consolidation_recorded'
    compacted = subprocess.run([*command, 'compact'], capture_output=True, text=True, timeout=15)
    assert compacted.returncode == 0, compacted.stderr
    assert json.loads(compacted.stdout)['original_episodes_deleted'] == 0
    with sqlite3.connect(state / 'memory.sqlite3') as db:
        assert db.execute('SELECT COUNT(*) FROM observations').fetchone()[0] == 2
        assert {row[0] for row in db.execute('SELECT typeof(body) FROM observations')} == {'blob'}
    restored = Main(state)
    try:
        assert restored.pair.snapshot_id == pair
        assert restored.status()['consolidated_experience_count'] == 1
        assert restored.memory.episode(a['episode_id']).steps[0].observation['text'] == '원문 A ' * 200
        assert restored.memory.episode(b['episode_id']).steps[0].observation['text'] == '원문 B ' * 200
        assert restored.memory.episode(a['episode_id']).steps[0].observation['metadata'] == {'condition': '원본'}
        assert restored.memory.outcome_counts['failure'] == restored.memory.outcome_counts['conflict'] == 1
        assert restored.recall(a['episode_id'], pair)['receipt']['activation'].re_evidence.unresolved_conflict
    finally:
        restored.close()


def test_failed_group_commit_does_not_publish_derived_navigation(tmp_path):
    main = Main(tmp_path / 'state', allow_ingest=True, allow_maintenance=True)
    try:
        a = main.ingest(dict(request_id='a', text='원문 A', source='test:a', revision='1'))
        b = main.ingest(dict(request_id='b', text='원문 B', source='test:b', revision='1'))
        request = dict(parent_episode_ids=[a['episode_id'], b['episode_id']],
                       expected_pair_snapshot_id=main.pair.snapshot_id)
        original_db = main.db
        class FailingCommit:
            def __getattr__(self, key):
                return getattr(original_db, key)
            def execute(self, query, *args):
                if query == 'COMMIT':
                    raise sqlite3.OperationalError('injected group failure')
                return original_db.execute(query, *args)
        main.db = FailingCommit()
        with pytest.raises(sqlite3.OperationalError, match='injected group failure'):
            main.consolidate(request)
        main.db = original_db
        assert len(main.consolidations) == len(main.parent_groups) == 0
        assert original_db.execute('SELECT COUNT(*) FROM consolidations').fetchone()[0] == 0
        assert len(main.memory.records) == 2
    finally:
        main.db = original_db
        main.close()
