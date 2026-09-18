# -*- coding: utf-8 -*-
"""Recall columns (2026-09-18): recall reads per-row columns and cue ids instead of building every candidate."""
import pytest

from swegca_vrs2.compact_index import LightView
from swegca_vrs2.engine.mosaic_memory_activation import detect_deja_vu, recall_memory, replay_memory
from swegca_vrs2.store import Main, keys, asks_and_description


@pytest.fixture
def main(tmp_path):
    owner = Main(tmp_path / 'cols', allow_ingest=True)
    yield owner
    owner.close()


DOC = ('---\nname: setup\ndescription: "훅 설치와 데몬 재시작 절차"\n---\n\n본문 정산 배치 훅 데몬.\n\n'
       '## 찾을 때 묻는 말\n\n- 훅이 왜 느려졌나 · 데몬 락\n')


def fill(main):
    main.ingest_many([
        dict(request_id='d', text=DOC, source='doc:setup', revision='1', metadata=dict(kind='doc', project='t')),
        dict(request_id='l1', text='정산 배치 감사 훅 로그 하나', source='log:1', revision='1', metadata=dict(kind='log_entry', project='t')),
        dict(request_id='l2', text='압축 회수 데몬 로그 둘', source='log:2', revision='1', metadata=dict(kind='log_entry', project='t')),
        dict(request_id='f', text='바탕화면 목록 정산 훅 파일', source='fs:1', revision='1', metadata=dict(kind='fs_listing', project='t')),
        dict(request_id='p1', text='정산 배치 명제 지지', source='v:1', revision='1', proposition='P', polarity='support', outcome='success'),
        dict(request_id='p2', text='배치 명제 반박', source='v:2', revision='1', proposition='P', polarity='refute', outcome='failure'),
    ])


def test_columns_agree_with_the_episodes(main):
    fill(main)
    memory = main.memory
    for identifier in memory.iter_episode_ids():
        episode = memory.episode(identifier)
        text = episode.steps[0].observation['text']
        assert memory.proposition_of(identifier) == episode.steps[0].observation.get('proposition_id')
        a, d = asks_and_description(text)
        assert memory.asks_of(identifier) == (a.casefold(), d.casefold())
    assert memory.asks_of(memory.episode_ids_for_cue('설치')[0])[1] == '훅 설치와 데몬 재시작 절차'


@pytest.mark.parametrize('exclude', [(), ('fs_listing',)])
def test_id_based_candidates_equal_the_engine_recall(main, exclude):
    fill(main)
    for query in ('정산 배치 훅', '데몬 로그', '명제', '없는말'):
        memory = main.memory.masked(exclude)
        signal = detect_deja_vu(memory, query=query, current_cues=keys(query))
        engine = recall_memory(memory, signal)
        ours = memory.recall_candidates(signal)
        assert ours.snapshot_id == engine.snapshot_id and len(ours.candidates) == len(engine.candidates)
        for a, b in zip(engine.candidates, ours.candidates):
            assert (a.episode_id, a.matched_cues, a.cue_overlap, a.revision, a.verification_state, a.historical_outcomes) == \
                   (b.episode_id, b.matched_cues, b.cue_overlap, b.revision, b.verification_state, b.historical_outcomes)
        light = replay_memory(LightView(memory), ours)
        full = replay_memory(memory, engine)
        assert [(e.episode_id, e.steps, e.source_addresses) for e in light.episodes] == \
               [(e.episode_id, e.steps, e.source_addresses) for e in full.episodes]


def test_columns_are_backfilled_for_a_checkpoint_without_them(main):
    fill(main)
    main.checkpoint()
    store = main.memory._store
    saved = {c: store.pop(c) for c in ('props', 'asks', 'revs', 'outs')}
    main.checkpoint()                                   # a checkpoint written without the columns
    directory = main.directory
    main.close()
    reopened = Main(directory, allow_ingest=True)
    try:
        store = reopened.memory._store
        assert all(store[c] == saved[c] for c in saved)
        out = reopened.recall('정산 배치', reopened.pair.snapshot_id)
        assert out['current_propositions'] and 'P' in out['current_propositions'].values()
    finally:
        reopened.close()


def test_closure_and_gate_use_the_columns_not_episode_builds(main, monkeypatch):
    fill(main)
    memory = main.memory
    built = []
    original = type(memory)._build
    monkeypatch.setattr(type(memory), '_build', lambda self, row: (built.append(row), original(self, row))[1])
    store = memory._store
    store['cache'].clear()                              # no resident full episodes: any build would show
    out = main.recall('정산 배치 명제', main.pair.snapshot_id)
    assert out['current_propositions'] and not built     # closure, asks gate, replay: columns and light episodes only
