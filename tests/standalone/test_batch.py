# -*- coding: utf-8 -*-
"""Batch generations (2026-09-18): K observations enter as one generation; replay regroups them."""
import pytest

import swegca_vrs2.store as st
from swegca_vrs2.store import Main


def rows(prefix, n, text='정산 배치 감사 훅 세션 로그 압축 회수'):
    return [dict(request_id=f'{prefix}{i}', text=f'{text} {i} 원문', source=f'test:{prefix}{i}', revision='r1',
                 metadata=dict(kind='log_entry', project='t')) for i in range(n)]


@pytest.fixture
def main(tmp_path):
    owner = Main(tmp_path / 'batch store', allow_ingest=True)
    yield owner
    owner.close()


def test_batch_of_one_is_the_old_single_record_generation(tmp_path):
    a = Main(tmp_path / 'a', allow_ingest=True); b = Main(tmp_path / 'b', allow_ingest=True)
    try:
        b.db.execute('UPDATE identity SET value=? WHERE id=1', (a.identity,)); b.identity = a.identity
        b.close(); b = Main(tmp_path / 'b', allow_ingest=True)
        for r in rows('x', 3):
            one = a.ingest(r)
            many = b.ingest_many([r])
            assert many['results'][0]['episode_id'] == one['episode_id']
            assert many['pair_snapshot_id'] == one['pair_snapshot_id']
        assert a.graph.snapshot_id == b.graph.snapshot_id and a.pair.snapshot_id == b.pair.snapshot_id
    finally:
        a.close(); b.close()


def test_batch_holds_the_same_records_edges_and_journals_one_pair(main):
    batch = rows('k', 12)
    out = main.ingest_many(batch)
    assert out['count'] == out['journaled'] == out['added'] == 12
    assert {r['pair_snapshot_id'] for r in out['results']} == {main.pair.snapshot_id}
    pairs = [p for (p,) in main.db.execute('SELECT pair FROM observations ORDER BY seq')]
    assert len(set(pairs[-12:])) == 1 and main.memory.episode_count == 12
    assert main.graph.last_receipt['source_episode_count_added'] == 12
    assert all(main.graph.region_of(r['episode_id']) is not None for r in out['results'])


def test_batch_is_idempotent_per_row_and_rejects_reused_ids_whole(main):
    same = rows('s', 1)[0]
    main.ingest_many([same])
    out = main.ingest_many([same, rows('n', 1)[0]])
    assert [r['idempotent_replay'] for r in out['results']] == [True, False] and out['journaled'] == 1
    with pytest.raises(ValueError, match='request_id_reused_with_different_content'):
        main.ingest_many([rows('n', 1)[0], dict(same, text='다른 내용')])
    assert main.memory.episode_count == 2           # all-or-nothing: the good row was not kept


def test_supersedes_inside_one_batch_marks_the_earlier_record(main):
    first = main.ingest(dict(request_id='d1', text='갱신 전 본문 정산', source='doc:x', revision='1',
                             outcome='success', metadata=dict(kind='doc', project='t')))
    second = dict(request_id='d2', text='갱신 후 본문 정산', source='doc:x', revision='2', outcome='success',
                  supersedes=first['episode_id'], metadata=dict(kind='doc', project='t'))
    other = rows('o', 1)[0]
    out = main.ingest_many([second, other])
    assert out['added'] == 2 and out['results'][0]['vrs_event']['superseded_marked'] == 1
    assert first['episode_id'] in main.memory.superseded


def test_crash_replay_regroups_batches_and_reproduces_the_consolidation(tmp_path, monkeypatch):
    monkeypatch.setattr(st, 'CHECKPOINT_EVERY', 10 ** 9)          # no checkpoint: everything replays
    m = Main(tmp_path / 'c', allow_ingest=True)
    batch = rows('c', 30)
    m.ingest_many(batch[:20]); m.ingest_many(batch[20:25]); m.ingest(batch[25]); m.ingest_many(batch[26:])
    m.consolidate(cycles=16)
    m.ingest_many(rows('after', 3))
    pair, count, version = m.pair.snapshot_id, m.memory.episode_count, m.graph.stable.version_id
    m.db.close(); m.lock.release(); m.closed = True                # crash: no closing checkpoint
    r = Main(tmp_path / 'c', allow_ingest=True)
    try:
        assert r.restore['replayed_after_checkpoint'] == 34            # 30 + consolidation row + 3
        assert r.pair.snapshot_id == pair and r.memory.episode_count == count
        assert r.graph.stable.version_id == version
    finally:
        r.close()


def test_append_keeps_usage_and_aliases_across_generations(main):
    main.ingest_many(rows('u', 2))
    main.ingest_many([dict(request_id='p1', text='명제 하나 지지', source='v:p1', revision='1', proposition='P', polarity='support'),
                      dict(request_id='p2', text='명제 별칭 지지', source='v:p2', revision='1', proposition='P-alias', polarity='support')])
    main.usage_update({'test:u0': [3, 1]})
    main.alias_update('P', ['P-alias'])
    main.ingest_many(rows('v', 2))                                  # before 2026-09-18 this append dropped both
    assert main.graph.usage.get('test:u0') == [3, 1] and main.graph.aliases.get('P-alias') == 'P'
