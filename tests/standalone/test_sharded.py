# -*- coding: utf-8 -*-
"""The native MCP sees storage shards as one complete VRS main."""
from swegca_vrs2.resident import Resident
from swegca_vrs2.server import LocalResident
from swegca_vrs2.sharded import ShardedMain
from swegca_vrs2.store import Main


def claim(request_id, source, polarity):
    return dict(request_id=request_id,
                text=f"교차 샤드 명제 증거 정산 {polarity}",
                source=source, revision="1", outcome="success",
                proposition="cross-shard-claim", polarity=polarity,
                metadata=dict(kind="observation", project=source))


def test_global_four_stages_and_cross_shard_reevidence(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"s1": tmp_path / "s1"}, hot_limit=1)
    try:
        support = resident.ingest(claim("support", "main", "support"))["episode_id"]
        secondary = resident.main_for("s1")
        refute = resident.ingest(claim("refute", "secondary", "refute"), "s1")["episode_id"]
        primary.consolidate(cycles=4)
        secondary.consolidate(cycles=4)
        resident.refresh_pair("main", primary)
        resident.refresh_pair("s1", secondary)

        sharded = ShardedMain(primary, resident)
        status = sharded.status()
        assert status["memory_ready"] and status["complete_vrs_shards_ready"]
        assert status["hot_episode_count"] == 2 and len(status["shards"]) == 2
        root = sharded.recall("교차 샤드 명제 증거 정산", status["pair_snapshot_id"])
        loaded = sharded.recall("교차 샤드 명제 증거 정산", status["pair_snapshot_id"],
                                region_scope="loaded-all")
        activation = root["receipt"]["activation"]
        loaded_activation = loaded["receipt"]["activation"]
        assert activation.stage_order == ("deja_vu", "recall", "replay", "re_evidence")
        assert [row.episode_id for row in activation.recall.candidates] == [
            row.episode_id for row in loaded_activation.recall.candidates]
        assert activation.re_evidence == loaded_activation.re_evidence
        assert {row.episode_id for row in activation.recall.candidates} == {support, refute}
        assert {row.episode_id for row in activation.replay.episodes} == {support, refute}
        assert {row.episode_id for row in activation.re_evidence.judgments} == {support, refute}
        assert activation.re_evidence.conflicting_propositions == ("cross-shard-claim",)
        assert activation.re_evidence.unresolved_conflict and activation.re_evidence.should_abstain
        assert {root["region_navigation"]["paths"][identifier]["shard"]
                for identifier in (support, refute)} == {"main", "s1"}
        assert all(root["current_strengths"][identifier] > 0 for identifier in (support, refute))
        portals = root["region_navigation"]["cross_shard_portals"]
        assert portals and all(portal["rule"] == "original_experience_strength_mass_membership"
                               for portal in portals)
        assert {key["episode_id"] for portal in portals for key in portal["keys"]} <= {support, refute}
        assert all(key["weights"][0] >= 0.2 and key["weights"][1] >= 0.2
                   for portal in portals for key in portal["keys"])

        # LocalResident pages the same combined receipt; it does not silently
        # fall back to the primary shard.
        native = LocalResident(sharded)
        admitted = native.request("cognitive_dialogue_start", profile="memory-only-no-provider",
            request_id="combined", query="교차 샤드 명제 증거 정산",
            expected_pair_snapshot_id=status["pair_snapshot_id"])
        opened = native.request("cognitive_dialogue_evidence_open",
            request_id="combined", view_id=admitted["view_id"])
        assert opened["snapshot_id"] == status["pair_snapshot_id"]
        native.request("cognitive_dialogue_release", request_id="combined", view_id=admitted["view_id"])
    finally:
        resident.close()
        primary.close()


def test_cold_shard_status_uses_complete_current_projection_without_loading(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"s1": tmp_path / "s1"}, hot_limit=1)
    try:
        secondary = resident.main_for("s1")
        resident.ingest(claim("refute", "secondary", "refute"), "s1")
        resident.evict("s1")
        resident.settle()
        sharded = ShardedMain(primary, resident)
        status = sharded.status()
        assert status["memory_ready"] is True
        assert status["incomplete_shards"] == []
        assert status["shards"][1]["state"] == "cold"
        assert status["shards"][1]["read_projection_ready"] is True
        assert not resident.wanted and not resident.hot and not resident.warm
    finally:
        resident.close()
        primary.close()


def test_automatic_split_preserves_source_and_supersedes_lineage(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=1, bundle_limit=3)
    try:
        rows = [dict(request_id=f"r{i}", text=f"자동 분할 경험 {i}",
                     source=f"source:{i}", revision="1") for i in range(7)]
        receipt = resident.ingest_many(rows)
        assert [group["count"] for group in receipt["shards"]] == [3, 3, 1]
        assert [group["shard"] for group in receipt["shards"]] == ["main", "shard-000001", "shard-000002"]
        assert resident.auto_ids == ["shard-000001", "shard-000002"]
        resident.settle()

        first = receipt["results"][0]["episode_id"]
        revised = dict(request_id="r0-v2", text="자동 분할 경험 0 수정",
                       source="source:0", revision="2", supersedes=first)
        revision = resident.ingest(revised)
        assert revision["source"] == "source:0"
        assert revision["episode_id"] in primary.memory.records
        assert primary.memory.episode_count == 4       # lineage may exceed the split target

        sharded = ShardedMain(primary, resident)
        status = sharded.status()
        if not status["memory_ready"]:
            for shard in resident.ids():
                resident.ready(shard)
            resident.prepare_all()
            status = sharded.status()
        root = sharded.recall("자동 분할 경험", status["pair_snapshot_id"])
        assert root["record_count"] == 8
        activation = root["receipt"]["activation"]
        assert len(activation.recall.candidates) == 8
        assert len(activation.replay.episodes) == 1
        exact_id = receipt["results"][-1]["episode_id"]
        exact = sharded.recall(exact_id, status["pair_snapshot_id"])
        assert exact["receipt"]["activation"].stage_order == (
            "deja_vu", "recall", "replay", "re_evidence")
        assert exact["receipt"]["activation"].replay.episodes[0].episode_id == exact_id
        excluded = sharded.recall(exact_id, status["pair_snapshot_id"],
                                  exclude_kinds=("", "conversation"))
        # These rows have no kind, so excluding the empty kind follows the
        # same masked-index rule as Main and must not bypass it via the capsule.
        assert not excluded["receipt"]["activation"].replay.episodes

        # Ended-session assimilation walks every automatic shard through one
        # monotonic cursor and preserves exact content-derived addresses.
        exported, cursor = [], 0
        while True:
            page = resident.export_observations(cursor, max_records=2)
            exported.extend(page["rows"])
            assert page["next_sequence"] >= cursor
            cursor = page["next_sequence"]
            if page["complete"]:
                break
        assert len(exported) == 8
        destination = Main(tmp_path / "assimilated", allow_ingest=True)
        try:
            merged = destination.ingest_many([item["observation"] for item in exported])
            assert [item["episode_id"] for item in exported] == [
                item["episode_id"] for item in merged["results"]]
        finally:
            destination.close()
    finally:
        resident.close()
        primary.close()


def test_persistent_source_route_keeps_revision_in_cold_original_shard(tmp_path):
    directory = tmp_path / "main"
    primary = Main(directory, allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=1, bundle_limit=2)
    try:
        receipt = resident.ingest_many([
            dict(request_id=f"seed-{number}", text=f"재시작 계보 {number}",
                 source=f"source:{number}", revision="1")
            for number in range(3)
        ])
        original = receipt["results"][2]["episode_id"]
        assert resident.exact.source_shard("source:2") == "shard-000001"
    finally:
        resident.close()
        primary.close()

    primary = Main(directory, allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=1, bundle_limit=2)
    try:
        assert not resident.hot and "shard-000001" in resident.auto_ids
        revised = resident.ingest(dict(request_id="seed-2-v2", text="재시작 계보 2 수정",
            source="source:2", revision="2", supersedes=original))
        assert revised["episode_id"] in resident.hot["shard-000001"].memory.records
        assert revised["episode_id"] not in primary.memory.records
    finally:
        resident.close()
        primary.close()


def test_exact_replay_opens_only_target_and_same_proposition_shards(tmp_path, monkeypatch):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"target": tmp_path / "target", "unrelated": tmp_path / "unrelated"},
                        hot_limit=1)
    try:
        target = resident.ingest(claim("target", "target-source", "support"), "target")["episode_id"]
        resident.evict("target"); resident.settle()
        projected = resident.projection("target")
        exact_location = resident.exact_replay(target)
        assert projected.current(target, exact_location["shard_row"])["pair_snapshot_id"] == \
            resident.pair_ids["target"]
        resident.ingest(dict(request_id="other", text="완전히 무관한 차가운 샤드",
            source="unrelated-source", revision="1"), "unrelated")
        resident.evict("unrelated"); resident.settle()
        assert not resident.hot and not resident.warm

        import swegca_vrs2.sharded as sharded_module
        real_result = sharded_module.RecallResult
        real_replay = resident.exact_replay
        reached_recall = False

        def recall_result(*args, **kwargs):
            nonlocal reached_recall
            result = real_result(*args, **kwargs)
            reached_recall = True
            return result

        def replay_after_recall(identifier):
            assert reached_recall
            return real_replay(identifier)

        monkeypatch.setattr(sharded_module, 'RecallResult', recall_result)
        monkeypatch.setattr(resident, 'exact_replay', replay_after_recall)
        sharded = ShardedMain(primary, resident)
        exact = sharded.recall(target, resident.logical_snapshot())
        assert exact["receipt"]["activation"].replay.episodes[0].episode_id == target
        assert "target" not in resident.hot
        assert "target" not in resident.warm
        assert "unrelated" not in resident.hot
        assert "unrelated" not in resident.warm
        assert "unrelated" not in resident.wanted
    finally:
        resident.close()
        primary.close()


def test_exact_replay_materializes_only_incident_portals_once_per_generation(tmp_path, monkeypatch):
    primary = Main(tmp_path / "main", allow_ingest=True)
    primary.ingest(dict(request_id="exact", text="포털 국소 직렬화 검증",
                        source="portal:test", revision="1"))
    resident = Resident(primary, {}, hot_limit=0)
    try:
        while not resident.backfill_exact(16)["complete"]:
            pass
        identifier = primary.memory._store["ids"][0]
        calls = []
        import swegca_vrs2.resident as module
        original = module.projected_portals

        def observed(owner, active_regions=None, portal_pairs=None):
            calls.append(portal_pairs)
            return original(owner, active_regions, portal_pairs)

        monkeypatch.setattr(module, "projected_portals", observed)
        logical = ShardedMain(primary, resident)
        first = logical.recall(identifier, resident.logical_snapshot())
        second = logical.recall(identifier, resident.logical_snapshot())
        assert first["receipt"]["activation"].stage_order == (
            "deja_vu", "recall", "replay", "re_evidence")
        assert second["receipt"]["activation"].replay.episodes[0].episode_id == identifier
        assert len(calls) == 1 and calls[0] is not None
    finally:
        resident.close()
        primary.close()


def test_natural_recall_reads_cold_cue_and_proposition_shards_without_checkpoints(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {
        "support": tmp_path / "support",
        "refute": tmp_path / "refute",
        "unrelated": tmp_path / "unrelated",
    }, hot_limit=3)
    try:
        support = resident.ingest(dict(request_id="support", text="희귀앵커알파 관측",
            source="support-source", revision="1", proposition="route-claim",
            polarity="support", outcome="success"), "support")["episode_id"]
        refute = resident.ingest(dict(request_id="refute", text="겹치지않는 반대 관측",
            source="refute-source", revision="1", proposition="route-claim",
            polarity="refute", outcome="failure"), "refute")["episode_id"]
        resident.ingest(dict(request_id="other", text="무관샤드오메가",
            source="other-source", revision="1"), "unrelated")
        for shard in tuple(resident.hot):
            resident.evict(shard)
        resident.settle()
        sharded = ShardedMain(primary, resident)

        root = sharded.recall("희귀앵커알파", resident.logical_snapshot())
        activation = root["receipt"]["activation"]
        assert {row.episode_id for row in activation.replay.episodes} == {support, refute}
        assert activation.re_evidence.unresolved_conflict
        assert not resident.wanted
        assert not resident.hot and not resident.warm
        assert "unrelated" not in resident.warm and "unrelated" not in resident.hot
    finally:
        resident.close()
        primary.close()


def test_projected_natural_replay_starts_after_recall_result(tmp_path, monkeypatch):
    import swegca_vrs2.projected_recall as projection

    primary = Main(tmp_path / 'main', allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=0)
    try:
        resident.ingest(dict(request_id='stage-source', text='stageanchor observation',
                             source='probe:stage', revision='1'))
        real_result = projection.RecallResult
        real_replay = resident.exact_replay
        reached_recall = False
        replayed = []

        def recall_result(*args, **kwargs):
            nonlocal reached_recall
            result = real_result(*args, **kwargs)
            reached_recall = True
            return result

        def replay_after_recall(identifier):
            assert reached_recall
            replayed.append(identifier)
            return real_replay(identifier)

        monkeypatch.setattr(projection, 'RecallResult', recall_result)
        monkeypatch.setattr(resident, 'exact_replay', replay_after_recall)
        root = ShardedMain(primary, resident).recall('stageanchor',
                                                      resident.logical_snapshot())
        assert replayed
        assert {row.episode_id for row in root['receipt']['activation'].replay.episodes} == set(replayed)
    finally:
        resident.close()
        primary.close()


def test_projected_recall_opens_one_original_without_conflict(tmp_path, monkeypatch):
    from swegca_vrs2.loopback import hook_recall

    primary = Main(tmp_path / 'main', allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=0)
    try:
        for index in range(5):
            resident.ingest(dict(request_id=f'candidate-{index}',
                text=f'shared_anchor distinct_{index}', source=f'original:{index}',
                revision='1'))
        opened = []
        real_replay = resident.exact_replay

        def record_replay(identifier):
            opened.append(identifier)
            return real_replay(identifier)

        monkeypatch.setattr(resident, 'exact_replay', record_replay)
        activation = ShardedMain(primary, resident).recall(
            'shared_anchor', resident.logical_snapshot())['receipt']['activation']
        assert len(activation.recall.candidates) == 5
        assert len(activation.replay.episodes) == 1
        assert opened == [activation.replay.episodes[0].episode_id]
        opened.clear()
        packet = hook_recall(primary, {'query': 'shared_anchor'}, resident)
        assert packet['candidate_count'] == 5
        assert packet['returned'] == 1
        assert opened == [packet['memories'][0]['episode_id']]
    finally:
        resident.close()
        primary.close()


def test_default_recall_enters_deja_vu_before_capsule_columns(tmp_path, monkeypatch):
    import swegca_vrs2.projected_recall as projection
    import swegca_vrs2.sharded as sharded_module

    primary = Main(tmp_path / 'main', allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=0)
    try:
        identifier = resident.ingest(dict(request_id='stage-order',
            text='deja first capsule later', source='probe:order', revision='1'))['episode_id']
        real_columns = resident.exact_recall_columns
        real_matches = resident.cue_shards.matches_for
        real_contains = resident.exact.contains
        stage = ['before_deja_vu']
        reads = []
        match_phases = []

        def matches_during_deja_vu(cue):
            match_phases.append(stage[0])
            return real_matches(cue)

        def contains_during_deja_vu(address):
            assert stage[0] == 'deja_vu'
            return real_contains(address)

        def columns_after_deja_vu(address):
            assert stage[0] == 'recall'
            reads.append(address)
            return real_columns(address)

        def wrap_detect(real_detect):
            def detect(*args, **kwargs):
                assert stage[0] == 'before_deja_vu'
                stage[0] = 'deja_vu'
                signal = real_detect(*args, **kwargs)
                stage[0] = 'recall'
                return signal
            return detect

        monkeypatch.setattr(resident, 'exact_recall_columns', columns_after_deja_vu)
        monkeypatch.setattr(resident.cue_shards, 'matches_for', matches_during_deja_vu)
        monkeypatch.setattr(resident.exact, 'contains', contains_during_deja_vu)
        monkeypatch.setattr(projection, 'detect_deja_vu',
                            wrap_detect(projection.detect_deja_vu))
        root = ShardedMain(primary, resident).recall('deja first',
                                                      resident.logical_snapshot())
        assert root['receipt']['activation'].deja_vu.triggered
        assert identifier in reads
        assert match_phases and match_phases[0] == 'deja_vu'

        stage[0] = 'before_deja_vu'
        reads.clear()
        monkeypatch.setattr(resident.cue_shards, 'matches_for', real_matches)
        monkeypatch.setattr(sharded_module, 'detect_deja_vu',
                            wrap_detect(sharded_module.detect_deja_vu))
        exact = ShardedMain(primary, resident).recall(identifier,
                                                       resident.logical_snapshot())
        assert exact['receipt']['activation'].deja_vu.triggered
        assert reads == [identifier]
    finally:
        resident.close()
        primary.close()


def test_cold_projected_recall_preserves_local_shared_experience_portal(tmp_path, monkeypatch):
    topic_a = "루프백 데몬 체크포인트 저널 재생 락"
    topic_b = "정산 배치 엑셀 헤더 스프레드시트 매핑"
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"cold": tmp_path / "cold"}, hot_limit=1)
    try:
        owner = resident.main_for("cold")
        for number in range(14):
            resident.ingest(dict(request_id=f"a{number}",
                text=f"{topic_a} 기록 {number} 데몬 저널 체크포인트",
                source=f"a/log#{number}", revision="1"), "cold")
            resident.ingest(dict(request_id=f"b{number}",
                text=f"{topic_b} 기록 {number} 정산 헤더 매핑" + (" 저널" if number < 4 else ""),
                source=f"b/log#{number}", revision="1"), "cold")
        bridge = resident.ingest(dict(request_id="bridge", text=f"{topic_a} 그리고 {topic_b}",
            source="shared/log#1", revision="r7", outcome="success"), "cold")["episode_id"]
        owner.consolidate(cycles=16)
        resident.refresh_pair("cold", owner)
        sharded = ShardedMain(primary, resident)
        loaded = sharded.recall(topic_a, resident.logical_snapshot(), region_scope="loaded-all")
        locally_scoped = owner.recall(topic_a, owner.pair.snapshot_id, region_scope="regions")
        resident.evict("cold"); resident.settle()

        projected = sharded.recall(topic_a, resident.logical_snapshot())
        projected_scoped = sharded.recall(topic_a, resident.logical_snapshot(),
                                          region_scope="regions")
        monkeypatch.setattr("swegca_vrs2.projected_recall.REGION_SCOPE_AUTO_CANDIDATES", 1)
        projected_auto = sharded.recall(topic_a, resident.logical_snapshot(), region_scope="auto")
        loaded_activation = loaded["receipt"]["activation"]
        projected_activation = projected["receipt"]["activation"]
        assert [row.episode_id for row in projected_activation.recall.candidates] == [
            row.episode_id for row in loaded_activation.recall.candidates]
        crossings = [path for path in projected["region_navigation"]["paths"].values()
                     if path["path"] == "portal" and path.get("via")]
        assert crossings and crossings[0]["via"]["episode_id"] == bridge
        assert crossings[0]["via"]["revision"] == "r7"
        assert [row.episode_id for row in
                projected_scoped["receipt"]["activation"].recall.candidates] == [
                    row.episode_id for row in
                    locally_scoped["receipt"]["activation"].recall.candidates]
        assert projected_scoped["region_navigation"]["shard_roots"]["cold"]["scope"][
            "requested"] == "regions"
        assert [row.episode_id for row in
                projected_auto["receipt"]["activation"].recall.candidates] == [
                    row.episode_id for row in
                    projected_scoped["receipt"]["activation"].recall.candidates]
        assert not resident.hot and not resident.warm and not resident.wanted
    finally:
        resident.close()
        primary.close()
