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
        support = primary.ingest(claim("support", "main", "support"))["episode_id"]
        secondary = resident.main_for("s1")
        refute = secondary.ingest(claim("refute", "secondary", "refute"))["episode_id"]
        primary.consolidate(cycles=4)
        secondary.consolidate(cycles=4)

        sharded = ShardedMain(primary, resident)
        status = sharded.status()
        assert status["memory_ready"] and status["complete_vrs_shards_ready"]
        assert status["hot_episode_count"] == 2 and len(status["shards"]) == 2
        root = sharded.recall("교차 샤드 명제 증거 정산", status["pair_snapshot_id"])
        activation = root["receipt"]["activation"]
        assert activation.stage_order == ("deja_vu", "recall", "replay", "re_evidence")
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


def test_cold_shard_is_named_unready_until_complete_generation_is_prepared(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"s1": tmp_path / "s1"}, hot_limit=1)
    try:
        secondary = resident.main_for("s1")
        secondary.ingest(claim("refute", "secondary", "refute"))
        resident.evict("s1")
        resident.settle()
        sharded = ShardedMain(primary, resident)
        status = sharded.status()
        assert status["memory_ready"] is False
        assert status["incomplete_shards"] == [{"id": "s1", "state": "preparing"}]
        prepared = resident.prepare_all()
        assert prepared[0]["complete_vrs"] is True
        assert sharded.status()["memory_ready"] is True
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
            resident.prepare_all()
            status = sharded.status()
        root = sharded.recall("자동 분할 경험", status["pair_snapshot_id"])
        assert root["record_count"] == 8
        assert len(root["receipt"]["activation"].replay.episodes) == 8

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
