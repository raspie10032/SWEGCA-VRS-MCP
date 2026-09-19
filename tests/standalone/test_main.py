"""Behavioral acceptance for real standalone main, never a protocol stub."""
import builtins
import hashlib
import json
import socket
import sqlite3
from dataclasses import replace
from pathlib import Path

import numpy as np
import pytest

from swegca_vrs2.store import Main, OUTCOMES, EDGE, frozen
from swegca_vrs2.server import StandaloneMCP
from swegca_vrs2.engine.mosaic_vrs_event_kernel import EventVrsInputs
from swegca_vrs2.engine.mosaic_vrs_event_signal import settle_event_signal
from swegca_vrs2.engine.mosaic_vrs_dependency_index import EndpointDependencyIndex
from swegca_vrs2.engine.mosaic_memory_promotion import assess_vrs_experience_promotion


def record(main, i='one', **kwargs):
    return main.ingest(dict(request_id=i, text='한국어 기억 원문 ' + i,
        source='test:' + i, revision='r1', **kwargs))


@pytest.fixture
def main(tmp_path):
    owner = Main(tmp_path / '한글 경로 with spaces', allow_ingest=True)
    yield owner
    owner.close()


def test_six_outcomes_real_native_vrs_and_original_receipts(main):
    for outcome in sorted(OUTCOMES):
        result = record(main, outcome, outcome=outcome)
        assert result['vrs_event']['status'] == 'signal_fixed_point'
        assert result['vrs_event']['edge_evaluations'] > 0
        assert result['vrs_event']['historical_outcome'] == outcome
    root = main.recall('한국어 기억', main.pair.snapshot_id)
    activation = root['receipt']['activation']
    assert activation.stage_order == ('deja_vu', 'recall', 'replay', 're_evidence')
    assert len(activation.recall.candidates) == 6
    assert {e.steps[0].outcome for e in activation.replay.episodes} == OUTCOMES
    assert all(j.verdict == 'available' for j in activation.re_evidence.judgments)
    assert not activation.re_evidence.should_abstain
    assert all(root['region_memberships'].values())
    assert not any(root['current_promotions'].values())
    assert not any(main.status()['authority'].values())


def test_persistence_exact_identity_snapshot_original_and_idempotency(main):
    first = record(main)
    old_identity, old_pair = main.identity, main.pair.snapshot_id
    main.close()
    restored = Main(main.directory, allow_ingest=True)
    try:
        assert restored.identity == old_identity and restored.pair.snapshot_id == old_pair
        assert record(restored)['idempotent_replay']
        assert len(restored.memory.records) == 1
        episode = restored.memory.episode(first['episode_id'])
        assert episode.steps[0].observation['text'] == '한국어 기억 원문 one'
        assert episode.source_addresses == ('test:one',) and episode.revision == 'r1'
        with pytest.raises(ValueError, match='reused'):
            restored.ingest(dict(request_id='one', text='different', source='test:one', revision='r1'))
    finally:
        restored.close()


def test_conflict_closure_crosses_query_and_page_then_revision_preserves_history(main):
    a = main.ingest(dict(request_id='a', text='고유검색단서 청색', source='test:same', revision='1',
        proposition='object:color-blue', polarity='support', outcome='success'))
    b = main.ingest(dict(request_id='b', text='별개의 문장', source='test:other', revision='1',
        proposition='object:color-blue', polarity='refute', outcome='pending'))
    server = StandaloneMCP(main)
    result = server.call_tool('memory_context', dict(request_id='q',query='고유검색단서',
        expected_pair_snapshot_id=main.pair.snapshot_id,page_size=1))
    assert result['candidate_count'] == 2 and result['next_index'] == 1
    assert result['main_controls']['unresolved_conflict']['data'] is True
    assert result['main_controls']['conflicting_propositions']['data'] == ['object:color-blue']
    old_root = main.recall('고유검색단서', main.pair.snapshot_id)
    main.ingest(dict(request_id='c', text='정정된 기록', source='test:other', revision='2',
        proposition='object:color-blue', polarity='support', supersedes=b['episode_id']))
    # Existing handle still reads one immutable old generation, not mixed results.
    later_page = server.call_tool('memory_context', result['next_call']['arguments'])
    assert later_page['pair_snapshot_id'] == result['pair_snapshot_id']
    assert later_page['main_controls']['unresolved_conflict']['data'] is True
    fresh = main.recall('고유검색단서', main.pair.snapshot_id)
    assert not fresh['receipt']['activation'].re_evidence.unresolved_conflict
    assert len(fresh['receipt']['activation'].recall.candidates) == 3
    assert fresh['superseded_by'][b['episode_id']]
    assert old_root['receipt']['activation'].re_evidence.unresolved_conflict
    server.close()


def test_hot_cognition_no_disk_json_hash_network_or_model(main, monkeypatch):
    record(main)
    pair = main.pair.snapshot_id
    def forbidden(*args, **kwargs):
        raise AssertionError('cold I/O or model transport on hot cognition')
    for target in ((builtins,'open'),(Path,'open'),(json,'loads'),(json,'dumps'),
                   (hashlib,'sha256'),(socket,'socket'),(sqlite3,'connect')):
        monkeypatch.setattr(*target, forbidden)
    assert main.recall('한국어', pair)['receipt']['activation'].recall.candidates


def test_single_owner_readonly_forged_grants_and_stale_snapshot(main):
    with pytest.raises(ValueError, match='already_owned'):
        Main(main.directory)
    initial = main.pair.snapshot_id
    record(main)
    with pytest.raises(ValueError, match='snapshot_mismatch'):
        main.recall('한국어', initial)
    with pytest.raises(ValueError, match='invalid_observation_fields'):
        main.ingest(dict(request_id='attack',text='test',source='test',revision='r',authority={'world':True}))
    main.allow_ingest = False
    with pytest.raises(ValueError, match='disabled'):
        record(main, 'two')


def test_failed_durable_transaction_leaves_main_bit_exact(main):
    record(main)
    original_db = main.db
    class FailingCommit:
        def __getattr__(self, key):
            return getattr(original_db, key)
        def execute(self, query, *args):
            if query == 'COMMIT':
                raise sqlite3.OperationalError('injected disk failure')
            return original_db.execute(query, *args)
    before = main.memory, main.graph, main.pair, main.operations, main.owner.snapshot()
    main.db = FailingCommit()
    with pytest.raises(sqlite3.OperationalError):
        record(main, 'two')
    assert before == (main.memory, main.graph, main.pair, main.operations, main.owner.snapshot())
    assert original_db.execute('SELECT COUNT(*) FROM observations').fetchone()[0] == 1
    main.db = original_db
    assert record(main, 'two')['status'] == 'observation_recorded'


def test_corrupt_durable_record_rejected_and_owner_lock_released(main):
    record(main)
    main.db.execute("UPDATE observations SET fingerprint='bad'")
    main.close()
    with pytest.raises(ValueError, match='integrity_failed'):
        Main(main.directory)
    # Failure must not leak an ownership lock.
    with pytest.raises(ValueError, match='integrity_failed'):
        Main(main.directory)


def test_large_source_exact_deferred_access_and_cleanup(main):
    text = '오리지널 한국어 ' + '𐐀é가' * 15000
    main.ingest(dict(request_id='large',text=text,source='test:large',revision='r1'))
    server = StandaloneMCP(main)
    result = server.call_tool('memory_context',dict(request_id='q',query='오리지널',expected_pair_snapshot_id=main.pair.snapshot_id))
    assert result['status'] == 'memory_context_ready'
    assert result['memories'][0]['replay']['complete'] is False
    handle = {k:result[k] for k in ('request_id','view_id')}
    path=['receipt','activation','replay','episodes',0,'steps',0,'observation','text']
    parts, offset = [], 0
    while True:
        page = server.call_tool('memory_read_path',dict(handle,path=path,offset=offset))
        parts.append(page['content'])
        offset=page['next_offset']
        if offset is None: break
    assert ''.join(parts) == text
    assert server.close() == [] and not server.resident.sessions


def test_event_signal_matches_independent_dense_reference():
    import math
    edge = frozen(np.array([(0,1,1,.5),(1,0,-1,.7),(2,1,1,.2)],dtype=EDGE))
    direct = frozen(np.array([.3,-.4,.2],dtype=np.float32))
    score = frozen(np.zeros(3,dtype=np.float32))
    strength=frozen(edge['vrs_strength'].copy())
    inputs=EventVrsInputs('a'*64,direct,score,edge,strength,frozen(np.ones(3,dtype=bool)),EndpointDependencyIndex.build(edge))
    result=settle_event_signal(inputs,changed_nodes=(0,1,2),maximum_rounds=512)
    reference=np.zeros(3,dtype=np.float32)
    for _ in range(512):
        successor=reference.copy()
        for n in range(3):
            incoming=[i for i,e in enumerate(edge) if e['target']==n]
            signal=math.fsum(float(reference[int(edge['source'][i])])*int(edge['sign'][i])*float(strength[i]) for i in incoming)
            denominator=max(1.,math.fsum(abs(float(strength[i])) for i in incoming))
            successor[n]=np.float32(.8*float(reference[n])+.2*math.tanh(float(direct[n])+.2*signal/denominator))
        if np.array_equal(reference.view(np.uint32),successor.view(np.uint32)): break
        reference=successor
    assert not result.pending_nodes
    actual=np.array([result.scores.get(i,0.) for i in range(3)],dtype=np.float32)
    np.testing.assert_array_equal(actual.view(np.uint32),reference.view(np.uint32))
    assert not result.strengths and not inputs.score.any()


@pytest.mark.parametrize('old,new,action,promoted',[(.9,1.,'promote',True),(1.,.995,'revoke',False),(1.,1.01,'retain',True)])
def test_native_promotion_threshold_preserves_separate_authority(old,new,action,promoted):
    decision=assess_vrs_experience_promotion(snapshot_id='a'*64,connection_id='vrs-edge:0',previous_strength=old,current_strength=new)
    assert decision.action==action and decision.promoted is promoted
    assert not decision.action_authorized and not decision.persistent_write_authorized


def test_unrelated_component_is_shared_and_all_sources_remain_addressable(main):
    a=main.ingest(dict(request_id='a',text='alpha',source='test:a',revision='r1'))
    before=main.graph.regions
    main.ingest(dict(request_id='b',text='beta',source='test:b',revision='r1'))
    component=main.graph.components[main.graph.nodes[a['episode_id']]]
    assert main.graph.regions[component] is before[component]
    assert main.graph.last_receipt['changed_component_nodes']==3
    assert len(main.memory.records)==2


def test_exact_original_address_access_without_matching_source_text(main):
    first=record(main)
    result=main.recall(first['episode_id'],main.pair.snapshot_id)
    assert [r.episode_id for r in result['receipt']['activation'].recall.candidates]==[first['episode_id']]


def test_checkpoint_restores_mixed_script_retrieval_cues(main):
    first=main.ingest(dict(request_id='mixed',text='big_chunk270인지 기록',source='test:mixed',revision='r1'))
    pair=main.pair.snapshot_id
    before=main.recall('270',pair)
    assert [r.episode_id for r in before['receipt']['activation'].recall.candidates]==[first['episode_id']]
    main.close()
    restored=Main(main.directory)
    try:
        assert restored.status()['checkpoint_sequence']==1
        after=restored.recall('270',pair)
        assert [r.episode_id for r in after['receipt']['activation'].recall.candidates]==[first['episode_id']]
        assert restored.memory.episode(first['episode_id']).steps[0].observation['text']=='big_chunk270인지 기록'
    finally:
        restored.close()


def test_korean_mixed_script_ranking_preserves_conflict_closure(main):
    main.ingest(dict(request_id='short',text='270',source='test:short',revision='1'))
    rich=main.ingest(dict(request_id='rich',text='big_chunk270인지 단계 실행 결과 기록',
        source='test:rich',revision='1',proposition='P:result',polarity='support'))
    opponent=main.ingest(dict(request_id='opponent',text='별개 반대 근거',
        source='test:opponent',revision='1',proposition='P:result',polarity='refute'))
    root=main.recall('big_chunk 270인지 결과',main.pair.snapshot_id)
    ordered=[row.episode_id for row in root['receipt']['activation'].recall.candidates]
    assert ordered[0]==rich['episode_id']
    assert opponent['episode_id'] in ordered and len(ordered)==3
    activation=root['receipt']['activation']
    assert activation.re_evidence.unresolved_conflict
    assert [row.episode_id for row in activation.replay.episodes]==ordered
    assert [row.episode_id for row in activation.re_evidence.judgments]==ordered
    assert root['memory_selection']['semantic_acceptance_claimed'] is False


def test_duplicate_source_is_not_new_experience_or_reinforcement(main):
    row=dict(request_id='a',text='동일한 원문',source='test:same',revision='1')
    first=main.ingest(row)
    pair=main.pair.snapshot_id
    second=main.ingest(dict(row,request_id='b'))
    assert second['episode_id']==first['episode_id']
    assert second['distinct_source_episode_added']==0
    assert len(main.memory.records)==1 and main.pair.snapshot_id==pair
    main.close()
    restored=Main(main.directory)
    assert restored.pair.snapshot_id==pair and len(restored.memory.records)==1
    restored.close()


def test_ingress_re_evidence_updates_once_conflict_abstains_and_revision_weakens(main):
    a=main.ingest(dict(request_id='a',text='첫 기록',source='test:a',revision='1',proposition='P',polarity='support'))
    assert main.graph.strength(a['episode_id'])==.5
    main.ingest(dict(request_id='b',text='다른 출처 보고',source='test:b',revision='1',proposition='P',polarity='support'))
    strengthened=main.graph.strength(a['episode_id'])
    assert strengthened==float(np.float32(.5*1.01))
    # Numerical settling rounds must not multiply reinforcement again.
    assert main.graph.last_receipt['strength_updates_reapplied_during_iterations']==0
    main.ingest(dict(request_id='c',text='반대 보고',source='test:c',revision='1',proposition='P',polarity='refute'))
    assert main.graph.strength(a['episode_id'])==strengthened
    assert all(u['update_action']=='abstain_conflict' for u in main.graph.last_receipt['re_evidence_updates']['updates'])
    assert main.recall('첫 기록',main.pair.snapshot_id)['receipt']['activation'].re_evidence.unresolved_conflict


def test_explicit_source_retraction_weakens_without_deleting_original(main):
    a=main.ingest(dict(request_id='a',text='과거 보고',source='test:a',revision='1',proposition='P',polarity='support'))
    main.ingest(dict(request_id='b',text='원출처 정정',source='test:a',revision='2',proposition='P',polarity='refute',supersedes=a['episode_id']))
    assert main.graph.strength(a['episode_id'])==float(np.float32(.5*.995))
    assert main.memory.episode(a['episode_id']).revision=='1'
    assert not main.recall('과거 보고',main.pair.snapshot_id)['receipt']['activation'].re_evidence.unresolved_conflict
