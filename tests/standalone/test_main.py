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
        assert result['vrs_event']['status'] == 'appended_pending_consolidation'
        assert result['vrs_event']['pending_edges'] > 0
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


def test_consolidation_follows_swegca_evidence_and_conflict_abstains(main):
    """vrs-regions (SWEGCA): a record's edge base is the arbiter weight w = confidence x (1 - contradiction)
    x (1 - uncertainty) from its proposition's evidence accumulator; strengths rise one generation at a time
    while the endpoints agree, so promotion (>= 1.0) needs both evidence weight and repeated consolidation;
    the accumulator's decision is reported as such (one producer -> abstain); an opposing claim from another
    source is the arbiter's directional conflict: the proposition is unresolved and nothing reinforces."""
    a=main.ingest(dict(request_id='a',text='첫 기록 명제 P',source='test:a',revision='1',proposition='P',polarity='support'))
    assert main.graph.strength(a['episode_id'])==0.75 and main.status()['consolidation_stale'] and main.graph.stable is None
    main.consolidate()
    stable=main.graph.stable
    decision=stable.decisions['P']
    assert decision['status']=='abstain' and decision['reason']=='minimum_effective_samples'
    w=float(stable.record_weight[main.graph.nodes[a['episode_id']]])
    assert abs(w-0.25)<1e-6                    # confidence 1, no contradiction, uncertainty 1 - 1/4
    assert not main.status()['consolidation_stale']
    first=main.graph.strength(a['episode_id'])
    assert first<1.0 and 0.25*0.25<=first<=1.0
    # agreeing evidence from another source raises w (uncertainty falls) and the edges keep climbing
    main.ingest(dict(request_id='b',text='다른 출처 보고 명제 P',source='test:b',revision='1',proposition='P',polarity='support'))
    main.consolidate(cycles=160)
    stable=main.graph.stable
    assert stable.decisions['P']['effective_samples']>1 and stable.decisions['P']['status']=='abstain'
    assert float(stable.record_weight[main.graph.nodes[a['episode_id']]])>w
    assert main.graph.strength(a['episode_id'])>=1.0 and main.recall('첫 기록',main.pair.snapshot_id)['current_promotions'][a['episode_id']]
    judgments={j.episode_id:j.verdict for j in main.recall('첫 기록',main.pair.snapshot_id)['receipt']['activation'].re_evidence.judgments}
    assert judgments[a['episode_id']]=='retained'
    # an opposing claim from a third source: unresolved proposition, no reinforcement, conflict reported
    main.ingest(dict(request_id='c',text='반대 보고 명제 P',source='test:c',revision='1',proposition='P',polarity='refute'))
    before=main.graph.strength(a['episode_id'])
    main.consolidate(cycles=32)
    assert main.graph.stable.decisions['P']['unresolved'] is True
    assert main.graph.strength(a['episode_id'])<before
    activation=main.recall('첫 기록',main.pair.snapshot_id)['receipt']['activation']
    assert activation.re_evidence.unresolved_conflict
    # the journal carries the consolidations; a restart replays them to the same version id
    version=main.graph.stable.version_id; pair=main.pair.snapshot_id
    rows=main.db.execute("SELECT COUNT(*) FROM observations WHERE request_id LIKE 'consolidation:%'").fetchone()[0]
    assert rows>=3
    main.close()
    restored=Main(main.directory,allow_ingest=True)
    try:
        assert restored.pair.snapshot_id==pair and restored.graph.stable.version_id==version
        restored.db.execute('DELETE FROM checkpoint')
    finally:
        restored.close()
    replayed=Main(main.directory,allow_ingest=True)          # no checkpoint: the whole journal is replayed
    try:
        assert replayed.pair.snapshot_id==pair and replayed.graph.stable.version_id==version
    finally:
        replayed.close()


def test_explicit_source_retraction_decays_without_deleting_original(main):
    a=main.ingest(dict(request_id='a',text='과거 보고',source='test:a',revision='1',proposition='P',polarity='support'))
    main.consolidate(cycles=160)
    promoted=main.graph.strength(a['episode_id'])
    assert promoted>=1.0
    main.ingest(dict(request_id='b',text='원출처 정정',source='test:a',revision='2',proposition='P',polarity='refute',supersedes=a['episode_id']))
    main.consolidate(cycles=32)
    assert main.graph.strength(a['episode_id'])<promoted          # superseded: no evidence weight, its edges decay
    assert main.graph.vrs_of(a['episode_id'])['promoted'] is False
    assert main.memory.episode(a['episode_id']).revision=='1'
    assert not main.recall('과거 보고',main.pair.snapshot_id)['receipt']['activation'].re_evidence.unresolved_conflict


def test_pending_observations_are_never_promoted_and_stay_addressable(main):
    for i in range(3):
        record(main,f'p{i}')
    main.consolidate()
    root=main.recall('한국어 기억',main.pair.snapshot_id)
    assert len(root['receipt']['activation'].recall.candidates)==3
    assert not any(root['current_promotions'].values())
    assert all(v['pending'] is False and v['promoted'] is False for v in (main.graph.vrs_of(i) for i in root['current_promotions']))


def test_rebuild_from_journal_rederives_consolidations(main):
    a=main.ingest(dict(request_id='a',text='재구축 명제',source='test:a',revision='1',proposition='P',polarity='support'))
    main.consolidate(cycles=160)
    record(main,'later')
    main.consolidate()
    before=main.pair.snapshot_id; version=main.graph.stable.version_id
    rows,_=main.rebuild_from_journal()
    assert rows>=4 and main.pair.snapshot_id==before and main.graph.stable.version_id==version
    assert main.graph.strength(a['episode_id'])>=1.0


def test_portals_between_regions_are_navigation_receipts_not_gates(main):
    """G6: after consolidation every region pair sharing connector edges has a portal object; a pair with a
    promoted bridge is a candidate; recall reports each candidate's path (local / portal / unbridged) and
    the rejected paths, without dropping any record."""
    for i in range(4):
        main.ingest(dict(request_id=f'log{i}',text=f'루프백 데몬 유휴 체크포인트 기록 {i} 원문',source=f'test:log{i}',revision='r1'))
    v=main.ingest(dict(request_id='v',text='판정 데몬 체크포인트 락 밖 직렬화 asks: 체크포인트 락',source='verdict/v',revision='1',outcome='failure',proposition='P',polarity='refute'))
    main.consolidate(cycles=160)
    stable=main.graph.stable
    counts=stable.portal_counts()
    assert counts['pairs']>=1 and counts['candidates']<=counts['pairs']
    for (a,b),portal in stable.portals.items():
        assert a<b and portal['bridges']>=1 and 0.0<=portal['score']<=1.0 and portal['status'] in ('candidate','weak')
        assert len(portal['provenance'])>=1 and all(len(p)==3 for p in portal['provenance'])
    root=main.recall('데몬 체크포인트 락',main.pair.snapshot_id)
    nav=root['region_navigation']
    ids=[c.episode_id for c in root['receipt']['activation'].recall.candidates]
    assert v['episode_id'] in ids and len(nav['active_regions'])>=1
    assert all(nav['paths'][i]['path'] in ('local','portal','unbridged','pending') for i in ids)
    assert all(r['path']=='unbridged' and r['reason'] for r in nav['rejected'])
    assert nav['restricts_memory_access'] is False and nav['unbridged_factor']==1.0
    assert main.graph.region_of(v['episode_id']) is not None


def test_region_scope_restricts_candidates_only_when_asked_and_never_loses_addressability(main):
    """G6 scope: 'all' keeps every matching record; 'regions' keeps the active regions plus candidate-portal
    partners (falling back to the whole store below the floor); 'auto' restricts only above a candidate
    threshold. Every record stays addressable by id whatever the scope."""
    for i in range(6):
        main.ingest(dict(request_id=f'r{i}',text=f'루프백 데몬 유휴 체크포인트 기록 {i} 원문',source=f'test:r{i}',revision='r1'))
    v=main.ingest(dict(request_id='v',text='판정 데몬 체크포인트 락 밖 직렬화 asks: 체크포인트 락',source='verdict/v',revision='1',outcome='failure',proposition='P',polarity='refute'))
    main.consolidate(cycles=160)
    full=main.recall('데몬 체크포인트 락',main.pair.snapshot_id,region_scope='all')
    scoped=main.recall('데몬 체크포인트 락',main.pair.snapshot_id,region_scope='regions')
    auto=main.recall('데몬 체크포인트 락',main.pair.snapshot_id,region_scope='auto')
    ids_full={c.episode_id for c in full['receipt']['activation'].recall.candidates}
    ids_scoped={c.episode_id for c in scoped['receipt']['activation'].recall.candidates}
    assert full['region_navigation']['scope']['applied']=='all' and not full['region_navigation']['restricts_memory_access']
    sc=scoped['region_navigation']['scope']
    assert sc['requested']=='regions' and sc['applied'] in ('regions','all')
    assert ids_scoped<=ids_full and (sc['applied']=='all' or len(ids_scoped)+sc['excluded_rows']>=len(ids_full))
    assert auto['region_navigation']['scope']['requested']=='auto' and auto['region_navigation']['scope']['applied']=='all'
    # exact address access ignores scope: the verdict is found by id even under the strictest scope
    direct=main.recall(v['episode_id'],main.pair.snapshot_id,region_scope='regions')
    assert [c.episode_id for c in direct['receipt']['activation'].recall.candidates]==[v['episode_id']]
