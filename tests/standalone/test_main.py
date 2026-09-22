"""Behavioral acceptance for real standalone main, never a protocol stub."""
import builtins
import hashlib
import json
import socket
from dataclasses import replace
from fractions import Fraction
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

from swegca_vrs2.store import Graph, Main, OUTCOMES, EDGE, frozen, full_current_pair
from swegca_vrs2 import store as store_module
from swegca_vrs2.server import StandaloneMCP
from swegca_vrs2.engine.mosaic_vrs_event_kernel import EventVrsInputs
from swegca_vrs2.engine.mosaic_vrs_event_signal import settle_event_signal
from swegca_vrs2.engine.mosaic_vrs_dependency_index import EndpointDependencyIndex
from swegca_vrs2.engine.mosaic_memory_promotion import assess_vrs_experience_promotion
from swegca_vrs2.engine.mosaic_memory_activation import (
    FullCurrentMemoryVrsSnapshot, current_experience_verdict, detect_deja_vu)
from swegca_vrs2.engine.mosaic_vrs_portal_activation import (
    activate_with_portals, preactivate_regions)
from swegca_vrs2.engine.mosaic_vrs_portal_lifecycle import PortalPolicy
from swegca_vrs2 import vrs_evidence, vrs_refine


def record(main, i='one', **kwargs):
    return main.ingest(dict(request_id=i, text='한국어 기억 원문 ' + i,
        source='test:' + i, revision='r1', **kwargs))


def test_component_region_source_uses_original_cues_and_stays_generation_bound(main):
    first = record(main, 'component-first', cues=['component-first-anchor'])
    graph = main.graph.rebuild_regions()
    pair = full_current_pair(main.memory, graph)
    assert pair.snapshot_id == FullCurrentMemoryVrsSnapshot(
        main.memory, graph.snapshot_id).snapshot_id
    assert pair.memory.episode(first['episode_id']) is main.memory.episode(first['episode_id'])
    matches = []
    for _, (region, _) in graph.regions.items():
        region.require_pair(pair)
        source = region.source
        for node in source.address_index.term_ids('component-first-anchor'):
            matches.append((region, node))
            assert region.terms[node] == 'component-first-anchor'
            assert source.terms is region.terms
            assert source.vrs_strength is region.strengths
            assert not hasattr(source, 'episode')
    assert matches
    assert 'component-first-anchor' in main.memory.episode(first['episode_id']).cues
    region, _ = matches[0]
    signal = detect_deja_vu(pair.memory, query='component-first-anchor',
                            current_cues=('component-first-anchor',))
    activated = preactivate_regions(pair, topology=region, signal=signal)
    assert activated.matched_term_count == 1 and activated.regions
    assert activated.memory_identifiers_exposed is False

    record(main, 'component-second', cues=['component-second-anchor'])
    assert all(not region.source.address_index.term_ids('component-second-anchor')
               for region, _ in matches)
    with pytest.raises(ValueError, match='different VRS generation'):
        matches[0][0].require_pair(main.pair)


def test_disconnected_components_store_only_their_own_node_positions(main):
    main.ingest(dict(request_id='disjoint-a', text='zzzzq', source='probe:a', revision='1'))
    main.ingest(dict(request_id='disjoint-b', text='xxxxr', source='probe:b', revision='1'))
    graph = main.graph.rebuild_regions()
    assert len(graph.regions) == 2
    stored = sum(positions.members.nbytes for _, positions in graph.regions.values())
    assert stored == graph.flat.count * np.dtype(np.uint32).itemsize
    assert stored < len(graph.regions) * graph.flat.count * np.dtype(np.int32).itemsize
    assert np.all(graph.labels() >= 0)


def test_component_edge_partition_preserves_order_when_new_record_joins_regions(main):
    for name, text, cue in (('partition-a', 'zzzzq', 'alphaanchor'),
                            ('partition-b', 'xxxxr', 'betaanchor')):
        main.ingest(dict(request_id=name, text=text, source='probe:' + name,
                         revision='1', cues=[cue]))
    initial = main.graph.rebuild_regions()
    assert len(initial.regions) == 2
    main.ingest(dict(request_id='partition-bridge', text='nnnnt',
                     source='probe:bridge', revision='1',
                     cues=['alphaanchor', 'betaanchor']))
    joined = main.graph.rebuild_regions()
    assert len(joined.regions) == 1
    region, positions = next(iter(joined.regions.values()))
    assert np.array_equal(positions.members[region.edge_source], joined.flat.src)
    assert np.array_equal(positions.members[region.edge_target], joined.flat.dst)
    assert np.array_equal(region.edge_sign, joined.flat.sign)
    assert np.array_equal(region.strengths, joined.flat.strength)


def test_navigation_publication_uses_current_vrs_without_changing_numeric_graph(main):
    first = main.ingest(dict(request_id='navigation-original', text='navsourceword',
                             source='probe:navigation', revision='1',
                             cues=['navigation-anchor']))
    main.consolidate()
    numeric_graph, original_pair = main.graph, main.pair
    assert any(region.vrs_snapshot_id != numeric_graph.snapshot_id
               for region, _ in numeric_graph.regions.values())
    prepared = main.navigation_prepare()
    assert prepared.pair.snapshot_id == original_pair.snapshot_id
    assert prepared.pair.memory.episode(first['episode_id']) is main.memory.episode(first['episode_id'])
    assert main.graph is numeric_graph
    for region, _ in prepared.topology.values():
        region.require_pair(prepared.pair)
        assert region.vrs_snapshot_id == numeric_graph.snapshot_id
    binding = main.navigation_commit(prepared)
    assert binding.pair is main.pair and binding.topology is prepared.topology
    assert main.graph is numeric_graph
    assert main.owner.snapshot() is main.pair
    signal = detect_deja_vu(main.pair.memory, query='navigation-anchor',
                            current_cues=('navigation-anchor',))
    assert any(preactivate_regions(main.pair, topology=region, signal=signal).regions
               for region, _ in binding.topology.values())
    stale = main.navigation_prepare()
    main.ingest(dict(request_id='navigation-next', text='othernavword',
                     source='probe:navigation-next', revision='1'))
    assert main.navigation_commit(stale) is None
    assert main._region_binding is None


def test_checkpoint_rejects_region_without_generation_bound_cue_source(main):
    record(main, 'checkpoint-region-source')
    main.consolidate()
    graph = main.graph
    component, (region, positions) = next(iter(graph.regions.items()))
    old_region = replace(region, source=SimpleNamespace(terms=region.terms))
    old_graph = Graph(graph.snapshot_id, graph.flat, graph.nodes, graph.components,
        graph.regions.set(component, (old_region, positions)), graph.last_receipt,
        stable=graph.stable, usage=graph.usage, aliases=graph.aliases)
    prepared = dict(main.checkpoint_prepare(), graph=old_graph)
    _, blob = Main.checkpoint_serialize(prepared)
    with pytest.raises(ValueError, match='checkpoint_region_source_invalid'):
        Main.checkpoint_deserialize(blob, main.identity, main.pair.snapshot_id)


def test_light_evidence_inputs_equal_full_original_episodes(main):
    """A faster episode view must preserve evidence, revisions and usage semantics."""
    original = main.ingest(dict(request_id='original', text='원 주장', source='case:one',
        revision='1', outcome='success', proposition='P', polarity='support',
        metadata={'axes': ['intervention'], 'project': 'one'}))
    main.ingest(dict(request_id='opponent', text='반대 주장', source='case:two',
        revision='1', outcome='failure', proposition='P', polarity='refute',
        metadata={'axes': ['counterfactual'], 'project': 'two'}))
    main.ingest(dict(request_id='pending-claim', text='미결 주장', source='case:three',
        revision='1', outcome='pending', proposition='P', polarity='support'))
    main.ingest(dict(request_id='revision', text='정정 주장', source='case:one',
        revision='2', outcome='pending', proposition='P', polarity='refute',
        supersedes=original['episode_id']))
    main.ingest(dict(request_id='usage', text='미결 참고', source='case:usage',
        revision='1', outcome='pending'))

    class FullEpisodes:
        def __init__(self, memory):
            self.memory = memory

        def __getattr__(self, name):
            if name == 'episode_light':
                raise AttributeError(name)
            return getattr(self.memory, name)

    memory = main.memory
    full = FullEpisodes(memory)
    graph = main.graph.with_usage({'case:usage': (1, 2)})
    light_evidence = vrs_evidence.build(graph, memory)
    full_evidence = vrs_evidence.build(graph, full)
    for name in ('record_weight', 'record_primary', 'record_polarity',
                 'resolved_count', 'observation_count'):
        assert getattr(light_evidence, name) == getattr(full_evidence, name)
    assert {key: value.summary() for key, value in light_evidence.hypotheses.items()} == {
        key: value.summary() for key, value in full_evidence.hypotheses.items()}

    labels = np.zeros(graph.flat.count, dtype=np.int64)
    light_inputs = vrs_refine.build_inputs(graph, memory, labels)
    full_inputs = vrs_refine.build_inputs(graph, full, labels)
    for name in light_inputs:
        first, second = light_inputs[name], full_inputs[name]
        assert np.array_equal(first, second) if isinstance(first, np.ndarray) else first == second


@pytest.fixture
def main(tmp_path):
    owner = Main(tmp_path / '한글 경로 with spaces', allow_ingest=True)
    yield owner
    owner.close()


def test_serialized_prefix_checkpoint_commits_during_continuous_ingest(tmp_path):
    owner = Main(tmp_path / "continuous", allow_ingest=True,
                 defer_checkpoints=True)
    try:
        record(owner, "one")
        record(owner, "two")
        prepared = owner.checkpoint_prepare()
        serialized = owner.checkpoint_serialize(prepared)
        record(owner, "three")
        receipt = owner.checkpoint_commit(prepared, serialized)
        assert receipt["current"] is False
        assert receipt["seq"] == 2 and receipt["replay_tail"] == 1
        assert owner.dirty == 1
        checkpoint = owner.journal.read_checkpoint()
        assert checkpoint.seq == 2 and checkpoint.pair == prepared["pair"]
        current = owner.checkpoint()
        assert current["current"] is True and current["replay_tail"] == 0
    finally:
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
    assert not any(root['region_memberships'].values())
    main.consolidate()
    settled = main.recall('한국어 기억', main.pair.snapshot_id)
    assert all(settled['region_memberships'].values())
    assert not any(root['current_promotions'].values())
    assert not any(main.status()['authority'].values())


def test_author_portal_fallback_replays_original_when_navigation_unavailable(main):
    original = record(main, 'portal-fallback', outcome='pending')
    pair = main.pair
    activated = activate_with_portals(pair, topology=None, associations=None,
        policy=PortalPolicy('test', 1, 1, Fraction(0)), observed_at_ns=0,
        query='한국어 기억', current_cues=('한국어', '기억'),
        judge=lambda replayed: current_experience_verdict(replayed,
            memory_snapshot_id=pair.memory.snapshot_id,
            vrs_snapshot_id=pair.vrs_snapshot_id), navigation_term_budget=1)
    receipt = activated.activation
    assert activated.navigation_failures == ((None,
        'portal topology or prepared associations unavailable'),)
    assert tuple(row.episode_id for row in receipt.replay.episodes) == (
        original['episode_id'],)
    assert receipt.stage_order == ('deja_vu', 'recall', 'replay', 're_evidence')
    assert dict(activated.elapsed_ns)['total'] >= 0


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


def test_opposing_originals_are_read_only_after_recall_replay(main, monkeypatch):
    main.ingest(dict(request_id='support', text='고유검색단서 청색', source='test:support',
        revision='1', proposition='object:color-blue', polarity='support', outcome='success'))
    main.ingest(dict(request_id='refute', text='별개의 문장', source='test:refute',
        revision='1', proposition='object:color-blue', polarity='refute', outcome='pending'))
    memory = main.memory
    phase = ['before_deja_vu']
    reads = []
    original_deja_vu = store_module.detect_deja_vu
    original_replay = store_module.replay_memory
    original_episode_light = type(memory).episode_light

    def detect(*args, **kwargs):
        phase[0] = 'deja_vu'
        return original_deja_vu(*args, **kwargs)

    def replay(*args, **kwargs):
        phase[0] = 'replay'
        result = original_replay(*args, **kwargs)
        phase[0] = 'after_replay'
        return result

    def episode_light(self, episode_id):
        if self is memory:
            reads.append(phase[0])
            assert phase[0] in ('replay', 'after_replay')
        return original_episode_light(self, episode_id)

    monkeypatch.setattr(store_module, 'detect_deja_vu', detect)
    monkeypatch.setattr(store_module, 'replay_memory', replay)
    monkeypatch.setattr(type(memory), 'episode_light', episode_light)
    result = main.recall('고유검색단서', main.pair.snapshot_id)
    assert result['receipt']['activation'].re_evidence.unresolved_conflict
    assert 'after_replay' in reads


def test_hot_cognition_no_disk_json_hash_network_or_model(main, monkeypatch):
    record(main)
    pair = main.pair.snapshot_id
    def forbidden(*args, **kwargs):
        raise AssertionError('cold I/O or model transport on hot cognition')
    for target in ((builtins,'open'),(Path,'open'),(json,'loads'),(json,'dumps'),
                   (hashlib,'sha256'),(socket,'socket')):
        monkeypatch.setattr(*target, forbidden)
    assert main.recall('한국어', pair)['receipt']['activation'].recall.candidates


def test_exact_address_recall_does_not_scan_every_records_cues(main):
    first = record(main, 'one')['episode_id']
    record(main, 'two')
    original = main.memory._store['cues']

    class RandomAccessOnly:
        def __len__(self):
            return len(original)

        def __getitem__(self, index):
            return original[index]

        def __iter__(self):
            raise AssertionError('recall scanned every record cue array')

    main.memory._store['cues'] = RandomAccessOnly()
    try:
        recalled = main.recall(first, main.pair.snapshot_id)
        assert [row.episode_id for row in recalled['receipt']['activation'].recall.candidates] == [first]
    finally:
        main.memory._store['cues'] = original


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
    original_append = main.journal.append
    before = main.memory, main.graph, main.pair, main.operations, main.owner.snapshot()
    def fail(_):
        raise OSError('injected disk failure')
    main.journal.append = fail
    with pytest.raises(OSError, match='injected disk failure'):
        record(main, 'two')
    assert before == (main.memory, main.graph, main.pair, main.operations, main.owner.snapshot())
    assert main.journal.row_count == 1
    main.journal.append = original_append
    assert record(main, 'two')['status'] == 'observation_recorded'


def test_corrupt_durable_record_rejected_and_owner_lock_released(main):
    record(main)
    main.close()
    path = main.journal.head_path
    body = bytearray(path.read_bytes())
    body[-33] ^= 1
    path.write_bytes(body)
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


def test_sixteen_worker_consolidation_is_bit_identical_to_serial(main, monkeypatch):
    for i, text in enumerate(('alpha', 'bravo', 'charlie', 'delta', 'echo', 'foxtrot')):
        main.ingest(dict(request_id=f'parallel-{i}', text=text,
                         source=f'test:parallel:{i}', revision='1', outcome='success'))
    prepared = main.consolidate_prepare()
    monkeypatch.setattr(vrs_refine, 'WORKERS', 1)
    serial = main.consolidate_run(prepared)
    monkeypatch.setattr(vrs_refine, 'WORKERS', 16)
    parallel = main.consolidate_run(prepared)
    assert parallel.stable.version_id == serial.stable.version_id
    np.testing.assert_array_equal(parallel.flat.strength, serial.flat.strength)
    np.testing.assert_array_equal(parallel.flat.score, serial.flat.score)
    np.testing.assert_array_equal(parallel.stable.state, serial.stable.state)
    np.testing.assert_array_equal(parallel.stable.stability, serial.stable.stability)


def test_unrelated_component_is_shared_and_all_sources_remain_addressable(main):
    a=main.ingest(dict(request_id='a',text='alpha',source='test:a',revision='r1'))
    main.consolidate()
    before=main.graph.regions
    b=main.ingest(dict(request_id='b',text='beta',source='test:b',revision='r1'))
    component=main.graph.components[main.graph.nodes[a['episode_id']]]
    assert main.graph.regions[component] is before[component]
    assert main.graph.nodes[b['episode_id']] not in main.graph.components
    assert main.graph.last_receipt['changed_component_nodes']==3
    assert main.recall(b['episode_id'],main.pair.snapshot_id)['receipt']['activation'].replay.episodes
    main.consolidate()
    assert main.graph.regions[component] is before[component]
    assert main.graph.nodes[b['episode_id']] in main.graph.components
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
    rows=sum(req.startswith('consolidation:') for _, req, *_ in main._journal_rows(0, None))
    assert rows>=3
    main.close()
    restored=Main(main.directory,allow_ingest=True)
    try:
        assert restored.pair.snapshot_id==pair and restored.graph.stable.version_id==version
        restored.journal.remove_checkpoint()
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


def test_open_counts_remain_provenance_without_resolving_pending_experience(main):
    # Opening a pending original is a read event, not a new support/refute result.
    used = record(main, 'used'); idle = record(main, 'idle')
    main.consolidate()
    labels = np.zeros(main.graph.flat.count, dtype=np.int64)
    before = vrs_refine.build_inputs(main.graph, main.memory, labels)
    before_strengths = (main.graph.strength(used['episode_id']), main.graph.strength(idle['episode_id']))
    assert main.usage_update({'test:used': [5, 3], 'test:nobody': [1, 1]})['sources'] == 1   # unknown source ignored
    after = vrs_refine.build_inputs(main.graph, main.memory, labels)
    for name in ('direct', 'unresolved', 'base'):
        np.testing.assert_array_equal(after[name], before[name])
    assert after['unresolved'][main.graph.nodes.episode_node[used['episode_id']]]
    assert not main.consolidation_stale()
    assert (main.graph.strength(used['episode_id']), main.graph.strength(idle['episode_id'])) == before_strengths
    assert main.graph.vrs_of(used['episode_id'], 'test:used')['usage'] == [5, 3]
    assert main.usage_update({'test:used': [5, 3]})['status'] == 'unchanged'   # same counts: no journal row
    before = main.pair.snapshot_id; version = main.graph.stable.version_id
    rows, _ = main.rebuild_from_journal()
    assert main.pair.snapshot_id == before and main.graph.stable.version_id == version   # usage rows replay exactly


def test_hypothesis_registry_folds_alias_propositions_into_one_and_replays(main):
    # two producers state the same claim in different words: without a binding they are two hypotheses
    # with one observation each; bound, they are one hypothesis with two source families
    a = main.ingest(dict(request_id='a', text='같은 말 첫 문장', source='verdict:a', revision='1',
                         proposition='the batch completes without failure', polarity='support',
                         metadata=dict(producer='asm-agent', project='P1')))
    b = main.ingest(dict(request_id='b', text='같은 말 둘째 문장', source='batch:daily#1', revision='1',
                         proposition='the daily audit batch finishes cleanly', polarity='support',
                         metadata=dict(producer='settlement-batch', project='P2')))
    main.consolidate()
    before = main.graph.stable.decisions
    assert len(before) == 2 and all(d['source_diversity'] == 1 for d in before.values())
    with pytest.raises(ValueError):
        main.alias_update('the batch completes without failure', ['no such proposition'])
    r = main.alias_update('the batch completes without failure', ['the daily audit batch finishes cleanly'])
    assert r['status'] == 'alias_recorded' and main.consolidation_stale()
    main.consolidate()
    after = main.graph.stable.decisions
    assert list(after) == ['the batch completes without failure']
    assert after['the batch completes without failure']['source_diversity'] == 2
    assert main.alias_update('the batch completes without failure', ['the daily audit batch finishes cleanly'])['status'] == 'unchanged'
    snapshot = main.pair.snapshot_id; version = main.graph.stable.version_id
    main.rebuild_from_journal()
    assert main.pair.snapshot_id == snapshot and main.graph.stable.version_id == version
    # both records still resolve and neither is superseded: a binding declares sameness, it deletes nothing
    assert a['episode_id'] not in main.memory.superseded and b['episode_id'] not in main.memory.superseded
