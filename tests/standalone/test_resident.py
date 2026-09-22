# -*- coding: utf-8 -*-
"""Every resident shard must retain the complete SWEGCA-VRS read path."""
import threading
import json
import os
from collections import OrderedDict
from types import SimpleNamespace

import pytest

from swegca_vrs2.loopback import hook_recall, _consolidate_stale_shards
from swegca_vrs2.resident import (
    MAX_RSS_BYTES, MAX_STORAGE_BYTES, Resident, WarmView, load_recall_generation,
)
from swegca_vrs2.store import Main
from swegca_vrs2.sharded import ShardedMain
from swegca_vrs2.native_journal import NativeJournal
from swegca_vrs2.read_lease import (begin_engine_recall, end_engine_recall,
                                     engine_recall_active)


def row(n, text, project, kind="conversation_turn"):
    return dict(request_id=f"{project}:{n}", text=text, source=f"{project}/conversation#{n}", revision="1",
                metadata=dict(kind=kind, project=project))


def test_checkpoint_is_one_complete_generation_without_index_only_table(tmp_path):
    m = Main(tmp_path / "b", allow_ingest=True)
    try:
        out = m.ingest_many([row(i, f"정산 배치 훅 데몬 이야기 {i}", "b") for i in range(5)])
        m.consolidate(cycles=4)
        m.checkpoint()
        journal = NativeJournal(tmp_path / "b")
        generation, covered = load_recall_generation(journal)
        exact = generation.recall(out["results"][0]["episode_id"], generation.pair.snapshot_id)
        activation = exact["receipt"]["activation"]
        assert activation.stage_order == ("deja_vu", "recall", "replay", "re_evidence")
        assert activation.replay.episodes[0].episode_id == out["results"][0]["episode_id"]
        assert generation.graph.stable.version_id == m.graph.stable.version_id
        assert covered == m._journal_head()[0]

        m.ingest(row(5, "체크포인트 뒤에 들어온 여섯째 기록 정산", "b"))
        with pytest.raises(ValueError, match="checkpoint_not_at_journal_head"):
            load_recall_generation(journal)
        m.checkpoint()
        generation, _ = load_recall_generation(journal)
        assert generation.memory.episode_count == 6
        journal.close()
    finally:
        m.close()


def test_resident_recalls_every_ready_shard_through_all_four_stages(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"t2m": tmp_path / "t2m", "sq": tmp_path / "sq"}, hot_limit=1)
    try:
        resident.ingest_many([row(i, f"주 뭉치의 정산 배치 기록 {i}", "main")
                              for i in range(3)], "main")
        t2m = resident.main_for("t2m")
        resident.ingest_many([row(i, f"티투엠 안드로이드 배포 기록 정산 {i}", "t2m")
                              for i in range(4)], "t2m")
        t2m.consolidate(cycles=4)
        resident.refresh_pair("t2m", t2m)
        assert list(resident.hot) == ["t2m"]
        resident.ingest(row(0, "에스큐 월배치 정산 기록", "sq"), "sq")
        assert list(resident.hot) == ["sq"]
        resident.settle()

        packet = hook_recall(primary, dict(query="정산 기록", limit=10, snippet=80), resident)
        assert not packet["misses"]
        by_bundle = {}
        for recalled in packet["memories"]:
            by_bundle.setdefault(recalled["bundle"], []).append(recalled)
        assert set(by_bundle) == {"main", "t2m", "sq"}
        assert len(by_bundle["main"]) == 3 and len(by_bundle["t2m"]) == 4 and len(by_bundle["sq"]) == 1
        assert {r["bundle_state"] for r in by_bundle["t2m"]} == {"cold"}
        assert all(r["vrs"] is not None and r["verdict"] in {"available", "retained"}
                   for r in by_bundle["t2m"])
        t2m_status = next(b for b in packet["bundles"] if b["bundle"] == "t2m")
        assert t2m_status["complete_vrs"] is True and t2m_status["read_projection_ready"] is True
        assert t2m_status["stage_order"] == ["deja_vu", "recall", "replay", "re_evidence"]

        t2m_id = by_bundle["t2m"][0]["episode_id"]
        assert resident.lookup(t2m_id) == "t2m"
        assert resident.lookup(by_bundle["main"][0]["episode_id"]) == "main"
        states = {b["id"]: b["state"] for b in resident.status()}
        assert states == {"main": "hot", "t2m": "cold", "sq": "hot"}
    finally:
        resident.close()
        primary.close()


def test_warm_view_refuses_an_uncheckpointed_tail(tmp_path):
    owner = Main(tmp_path / "b", allow_ingest=True)
    try:
        owner.ingest_many([row(i, f"소유자가 쓴 기록 정산 {i}", "b") for i in range(3)])
        owner.checkpoint()
        view = WarmView("b", tmp_path / "b")
        generation = view.refresh()
        assert generation.memory.episode_count == 3 and view.status()["state"] == "warm"
        owner.ingest(row(3, "체크포인트 뒤 소유자가 더 쓴 기록 정산", "b"))
        assert view.moved()
        with pytest.raises(ValueError, match="checkpoint_not_at_journal_head"):
            view.refresh()
        owner.checkpoint()
        assert view.refresh().memory.episode_count == 4
        view.close()
    finally:
        owner.close()


def test_complete_exact_directory_routes_new_source_without_cold_scan(tmp_path, monkeypatch):
    primary = Main(tmp_path / 'main', allow_ingest=True)
    resident = Resident(primary, hot_limit=1, bundle_limit=1)
    try:
        resident.ingest(row(0, '첫 원경험', 'main'))
        resident.ingest(row(0, '분할 원경험', 'shard'))
        assert resident.read_directory_complete()
        monkeypatch.setattr(WarmView, 'refresh', lambda self: (_ for _ in ()).throw(
            AssertionError('complete_address_index_must_not_scan_cold_shards')))
        assert resident._owner_with_source('new/source#1') is None
        assert resident._owner_with_episode('memory:' + 'a' * 64) is None

        new = primary.ingest(row(1, '아직 주소 인덱스에 없는 원경험', 'main'))
        assert not resident.read_directory_complete()
        resident._register_exact_many(primary, [new['episode_id']])
        resident.refresh_pair('main', primary)
        assert resident.read_directory_complete()
    finally:
        resident.close()
        primary.close()


def test_incomplete_new_shard_routes_from_journal_before_first_checkpoint(tmp_path,
                                                                          monkeypatch):
    primary = Main(tmp_path / 'main', allow_ingest=True)
    resident = Resident(primary, hot_limit=1, bundle_limit=1)
    try:
        shard = resident._new_auto_shard()
        resident.main_for(shard)
        resident.evict(shard)
        resident.settle()
        resident.exact_backfill[shard]['complete'] = False
        assert not resident.read_directory_complete()
        monkeypatch.setattr(WarmView, 'refresh', lambda self: (_ for _ in ()).throw(
            AssertionError('uncheckpointed_shard_requires_native_journal_owner')))
        assert resident._owner_with_source('new/source#2') is None
        assert resident._owner_with_episode('memory:' + 'b' * 64) is None
    finally:
        resident.close()
        primary.close()


def test_graceful_close_publishes_current_hot_shard_replay_projection(tmp_path):
    root = tmp_path / 'main'
    primary = Main(root, allow_ingest=True)
    resident = Resident(primary, hot_limit=1, bundle_limit=1)
    try:
        resident.ingest(row(0, '주 경험', 'main'))
        inserted = resident.ingest(row(0, '종료 직전의 원경험', 'session'))
        identifier = inserted['episode_id']
        shard = resident.lookup(identifier)
        assert shard != 'main'
    finally:
        resident.close()
        primary.close()

    primary = Main(root, allow_ingest=False)
    resident = Resident(primary, hot_limit=1, bundle_limit=1)
    try:
        assert resident.read_ready(shard)
        recalled = ShardedMain(primary, resident).recall(identifier, None)
        activation = recalled['receipt']['activation']
        assert activation.stage_order == ('deja_vu', 'recall', 'replay', 're_evidence')
        assert activation.replay.episodes[0].episode_id == identifier
    finally:
        resident.close()
        primary.close()


def test_complete_shard_packet_with_nested_metadata_encodes(tmp_path):
    from swegca_vrs2.native_transport import encode
    from swegca_vrs2.loopback import origins
    m = Main(tmp_path / "n", allow_ingest=True)
    try:
        r = row(0, "원본 결속 기록 정산 배치", "n")
        r["metadata"]["origin"] = dict(bytes=[0, 10], lines=[1, 1], sha256="ab" * 32, size=10, mtime_ns=1)
        m.ingest(r)
        packet = hook_recall(m, dict(query="정산 배치", limit=5, snippet=80))
        assert encode(packet) and packet["memories"][0]["metadata"]["origin"]["bytes"] == [0, 10]
        assert encode(origins(m, dict(offset=0, limit=10)))
    finally:
        m.close()


def test_idle_consolidation_covers_main_and_every_hot_shard_with_one_cpu_budget(tmp_path):
    owners = [Main(tmp_path / name, allow_ingest=True, defer_checkpoints=True)
              for name in ("main", "s1", "s2")]
    try:
        for number, owner in enumerate(owners):
            owner.ingest_many([row(i, f"독립 샤드 병렬 경험 {number} {i}", f"p{number}")
                               for i in range(8)])
        daemon = SimpleNamespace(main=owners[0], lock=threading.Lock(),
            bundles=SimpleNamespace(hot=OrderedDict((("s1", owners[1]), ("s2", owners[2]))),
                                    refresh_pair=lambda shard, owner: None))
        receipts = _consolidate_stale_shards(daemon, cycles=4)
        assert [r["shard"] for r in receipts] == ["main", "s1", "s2"]
        assert all(r["committed"] and r["parallel_shards"] == 3 for r in receipts)
        assert all(r["scheduled_shards"] == 3 and r["wave"] == 0 for r in receipts)
        assert all(r["workers_per_shard"] >= 1 for r in receipts)
        assert all(owner.graph.stable is not None and not owner.consolidation_stale()
                   for owner in owners)
    finally:
        for owner in owners:
            owner.close()


def test_recall_lease_defers_consolidation_selected_before_or_during_read(tmp_path,
                                                                          monkeypatch):
    owner = Main(tmp_path / 'main', allow_ingest=True, defer_checkpoints=True)
    identifier = 'a' * 32
    try:
        owner.ingest_many([row(i, f'읽기 스냅샷 고정 경험 {i}', 'lease')
                           for i in range(8)])
        refreshed = []
        daemon = SimpleNamespace(main=owner, lock=threading.Lock(),
            bundles=SimpleNamespace(hot=OrderedDict(),
                                    refresh_pair=lambda shard, current: refreshed.append(shard)))
        before = owner.pair.snapshot_id

        begin_engine_recall(owner.directory, identifier)
        assert engine_recall_active(owner.directory)
        assert _consolidate_stale_shards(daemon, cycles=4) == []
        assert owner.pair.snapshot_id == before and refreshed == []
        end_engine_recall(owner.directory, identifier)

        original_run = owner.consolidate_run
        def lease_after_refinement(*args, **kwargs):
            graph = original_run(*args, **kwargs)
            begin_engine_recall(owner.directory, identifier)
            return graph
        monkeypatch.setattr(owner, 'consolidate_run', lease_after_refinement)
        receipts = _consolidate_stale_shards(daemon, cycles=4)
        assert receipts[0]['committed'] is False
        assert owner.pair.snapshot_id == before and refreshed == []

        end_engine_recall(owner.directory, identifier)
        monkeypatch.setattr(owner, 'consolidate_run', original_run)
        receipts = _consolidate_stale_shards(daemon, cycles=4)
        assert receipts[0]['committed'] is True
        assert owner.pair.snapshot_id != before and refreshed == ['main']
    finally:
        end_engine_recall(owner.directory, identifier)
        owner.close()


def test_primary_recall_lease_pins_every_hot_shard_in_logical_main(tmp_path):
    owners = [Main(tmp_path / name, allow_ingest=True, defer_checkpoints=True)
              for name in ('main', 'child')]
    identifier = 'b' * 32
    try:
        for number, owner in enumerate(owners):
            owner.ingest_many([row(i, f'전체 논리 메인 고정 {number} {i}', f'lease-{number}')
                               for i in range(4)])
        refreshed = []
        daemon = SimpleNamespace(main=owners[0], lock=threading.Lock(),
            bundles=SimpleNamespace(hot=OrderedDict((('child', owners[1]),)),
                                    refresh_pair=lambda shard, current: refreshed.append(shard)))
        before = [owner.pair.snapshot_id for owner in owners]
        begin_engine_recall(owners[0].directory, identifier)
        assert _consolidate_stale_shards(daemon, cycles=4) == []
        assert [owner.pair.snapshot_id for owner in owners] == before
        end_engine_recall(owners[0].directory, identifier)
        receipts = _consolidate_stale_shards(daemon, cycles=4)
        assert [receipt['shard'] for receipt in receipts] == ['main', 'child']
        assert all(receipt['committed'] for receipt in receipts)
        assert refreshed == ['main', 'child']
    finally:
        end_engine_recall(owners[0].directory, identifier)
        for owner in owners:
            owner.close()


def test_consolidation_fails_closed_when_transient_wave_exceeds_ram_cap(tmp_path):
    owner = Main(tmp_path / "main", allow_ingest=True, defer_checkpoints=True)
    try:
        owner.ingest_many([row(i, f"메모리 상한 정산 {i}", "bounded") for i in range(8)])
        bundles = SimpleNamespace(hot=OrderedDict(), refresh_pair=lambda shard, current: None,
            _rss_bytes=lambda: MAX_RSS_BYTES - 32 * 1024 ** 2)
        daemon = SimpleNamespace(main=owner, lock=threading.Lock(), bundles=bundles)
        receipts = _consolidate_stale_shards(daemon, cycles=4)
        assert receipts == [dict(shard="main", error="vrs_memory_budget_exceeded",
            estimated_transient_bytes=receipts[0]["estimated_transient_bytes"],
            available_transient_bytes=0)]
        assert owner.graph.stable is None
    finally:
        owner.close()


def test_resource_budget_is_reported_and_admission_is_fail_closed(tmp_path, monkeypatch):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=1)
    try:
        budget = resident.budget(force_storage=True)
        assert budget["rss_limit_bytes"] == 4 * 1024 ** 3
        assert budget["storage_limit_bytes"] == 500_000_000_000
        assert budget["storage_bytes"] < budget["storage_limit_bytes"]

        monkeypatch.setattr(resident, "_rss_bytes", lambda: MAX_RSS_BYTES + 1)
        with pytest.raises(ValueError, match="memory_budget_exceeded"):
            resident.ingest(row(0, "메모리 한도 거부", "budget"))

        monkeypatch.setattr(resident, "_rss_bytes", lambda: 0)
        monkeypatch.setattr(resident, "_storage_bytes", lambda **kwargs: MAX_STORAGE_BYTES)
        with pytest.raises(ValueError, match="storage_budget_exceeded"):
            resident.ingest(row(1, "저장소 한도 거부", "budget"))
    finally:
        resident.close()
        primary.close()


def test_storage_admission_rejects_incomplete_filesystem_scan(tmp_path, monkeypatch):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=1)
    try:
        def unreadable_directory(_root, *, onerror):
            onerror(OSError("unreadable directory"))
            yield from ()

        with monkeypatch.context() as patch:
            patch.setattr("swegca_vrs2.resident.os.walk", unreadable_directory)
            with pytest.raises(ValueError, match="vrs_storage_scan_failed"):
                resident.ingest(row(0, "저장소 스캔 오류", "budget"))

        def missing_file(_root, *, onerror):
            yield str(primary.directory), (), ("vanished.vrs",)

        original_stat = os.stat
        def failing_stat(path, *args, **kwargs):
            if str(path).endswith("vanished.vrs"):
                raise FileNotFoundError(path)
            return original_stat(path, *args, **kwargs)

        with monkeypatch.context() as patch:
            patch.setattr("swegca_vrs2.resident.os.walk", missing_file)
            patch.setattr("swegca_vrs2.resident.os.stat", failing_stat)
            with pytest.raises(ValueError, match="vrs_storage_scan_failed"):
                resident.ingest(row(1, "저장소 파일 오류", "budget"))
        assert primary.memory.episode_count == 0
    finally:
        resident.close()
        primary.close()


def test_logical_snapshot_update_cost_does_not_scan_all_shards(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=1)
    try:
        before = resident.logical_snapshot()
        primary.ingest(row(0, "논리 스냅샷 증분 갱신", "snapshot"))
        resident.refresh_pair("main", primary)
        after = resident.logical_snapshot()
        assert after != before
        assert resident.logical_record_count() == 1
        assert resident.logical_cue_total() == primary.memory.cue_total

        class NoIteration(dict):
            def items(self):
                raise AssertionError("logical_snapshot scanned every shard")

            def values(self):
                raise AssertionError("logical read totals scanned every shard")

        resident.pair_ids = NoIteration(resident.pair_ids)
        resident.record_counts = NoIteration(resident.record_counts)
        resident.cue_totals = NoIteration(resident.cue_totals)
        assert resident.logical_snapshot() == after
        assert resident.logical_record_count() == 1
        assert resident.logical_cue_total() == primary.memory.cue_total
        primary.ingest(row(1, "논리 스냅샷 추가 갱신", "snapshot"))
        resident.refresh_pair("main", primary)
        assert resident.logical_record_count() == 2
        assert resident.logical_cue_total() == primary.memory.cue_total
    finally:
        resident.close()
        primary.close()


def test_exact_backfill_progress_persists_and_covers_one_cold_shard_at_a_time(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=1, bundle_limit=2)
    try:
        receipt = resident.ingest_many([
            row(number, f"정확주소 백필 경험 {number}", f"backfill-{number}")
            for number in range(5)
        ])
        identifiers = [item["episode_id"] for item in receipt["results"]]
        resident.settle()
        # Simulate a pre-repair directory: keep experience stores and rebuild
        # the exact directory/progress from their complete VRS generations.
    finally:
        resident.close()
        primary.close()

    import shutil
    shutil.rmtree(tmp_path / "main" / "exact-replay")
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {}, hot_limit=0, bundle_limit=2)
    try:
        passes = []
        for _ in range(8):
            result = resident.backfill_exact(2)
            passes.append(result)
            if result["complete"]:
                break
        assert passes[-1]["complete"] is True
        assert sum(item["added"] for item in passes) == 5
        assert all(resident.exact_replay(identifier) is not None for identifier in identifiers)
        saved = json.loads((tmp_path / "main" / "exact-replay" / "backfill.json")
                           .read_text(encoding="utf-8"))
        assert saved["schema"] == "swegca-vrs2-read-index-backfill-v2"
        assert set(saved["shards"]) == {"main", "shard-000001", "shard-000002"}
    finally:
        resident.close()
        primary.close()


def test_projection_backfill_rebuilds_one_existing_cold_vrs_shard(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"cold": tmp_path / "cold"}, hot_limit=1)
    try:
        identifier = resident.ingest(row(1, "projection rebuild experience", "projection-source"),
                                     "cold")["episode_id"]
        owner = resident.hot["cold"]
        owner.consolidate(cycles=4)
        resident.refresh_pair("cold", owner)
        resident.evict("cold"); resident.settle()
        projection_root = tmp_path / "cold" / "read-projection"
        import shutil
        shutil.rmtree(projection_root)
        resident.projection_views.clear()

        result = resident.backfill_projections(1)
        assert result["scanned"] == 1 and result["complete"] is True
        assert result["projections"][0]["status"] == "projected"
        exact = resident.exact_replay(identifier)
        current = resident.current_vrs(exact)
        lean = resident.current_vrs(exact, include_cue_strengths=False)
        assert current["pair_snapshot_id"] == resident.pair_ids["cold"]
        assert len(current["cue_strengths"]) == len(exact["cues"])
        assert lean == dict(current, cue_strengths=())
        assert not resident.hot and not resident.warm
    finally:
        resident.close()
        primary.close()


def test_hot_main_natural_read_defers_only_cross_shard_cue_strengths(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, hot_limit=1)
    try:
        identifier = resident.ingest(row(1, "hot main original experience", "main-source"))[
            "episode_id"]
        exact = resident.exact_replay(identifier)
        full = resident.current_vrs(exact)
        lean = resident.current_vrs(exact, include_cue_strengths=False)
        assert len(full["cue_strengths"]) == len(exact["cues"])
        assert lean == dict(full, cue_strengths=())
        activation = ShardedMain(primary, resident).recall(
            "hot main original experience", resident.logical_snapshot())[
                "receipt"]["activation"]
        assert [episode.episode_id for episode in activation.replay.episodes] == [identifier]
        assert activation.re_evidence.judgments[0].episode_id == identifier
    finally:
        resident.close()
        primary.close()
