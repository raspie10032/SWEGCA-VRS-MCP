# -*- coding: utf-8 -*-
"""Every resident shard must retain the complete SWEGCA-VRS read path."""
import sqlite3
import threading
from collections import OrderedDict
from types import SimpleNamespace

import pytest

from swegca_vrs2.loopback import hook_recall, _consolidate_stale_shards
from swegca_vrs2.resident import Resident, WarmView, load_recall_generation
from swegca_vrs2.store import Main


def row(n, text, project, kind="log_entry"):
    return dict(request_id=f"{project}:{n}", text=text, source=f"{project}/session-log.md#{n}", revision="1",
                metadata=dict(kind=kind, project=project))


def test_checkpoint_is_one_complete_generation_without_index_only_table(tmp_path):
    m = Main(tmp_path / "b", allow_ingest=True)
    try:
        out = m.ingest_many([row(i, f"정산 배치 훅 데몬 이야기 {i}", "b") for i in range(5)])
        m.consolidate(cycles=4)
        m.checkpoint()
        db = sqlite3.connect(str(tmp_path / "b" / "memory.sqlite3"))
        assert db.execute("SELECT 1 FROM sqlite_master WHERE name='checkpoint_warm'").fetchone() is None
        generation, covered = load_recall_generation(db)
        exact = generation.recall(out["results"][0]["episode_id"], generation.pair.snapshot_id)
        activation = exact["receipt"]["activation"]
        assert activation.stage_order == ("deja_vu", "recall", "replay", "re_evidence")
        assert activation.replay.episodes[0].episode_id == out["results"][0]["episode_id"]
        assert generation.graph.stable.version_id == m.graph.stable.version_id
        assert covered == m.db.execute("SELECT MAX(seq) FROM observations").fetchone()[0]

        m.ingest(row(5, "체크포인트 뒤에 들어온 여섯째 기록 정산", "b"))
        with pytest.raises(ValueError, match="checkpoint_not_at_journal_head"):
            load_recall_generation(db)
        m.checkpoint()
        generation, _ = load_recall_generation(db)
        assert generation.memory.episode_count == 6
        db.close()
    finally:
        m.close()


def test_resident_recalls_every_ready_shard_through_all_four_stages(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"t2m": tmp_path / "t2m", "sq": tmp_path / "sq"}, hot_limit=1)
    try:
        primary.ingest_many([row(i, f"주 뭉치의 정산 배치 기록 {i}", "main") for i in range(3)])
        t2m = resident.main_for("t2m")
        t2m.ingest_many([row(i, f"티투엠 안드로이드 배포 기록 정산 {i}", "t2m") for i in range(4)])
        t2m.consolidate(cycles=4)
        assert list(resident.hot) == ["t2m"]
        resident.main_for("sq").ingest(row(0, "에스큐 월배치 정산 기록", "sq"))
        assert list(resident.hot) == ["sq"]
        resident.settle()

        packet = hook_recall(primary, dict(query="정산 기록", limit=10, snippet=80), resident)
        assert packet["bundles"][0] == dict(bundle="t2m", state="preparing", miss=True)
        prepared = resident.prepare_all()
        assert [p["bundle"] for p in prepared] == ["t2m"]
        assert prepared[0]["complete_vrs"] is True

        packet = hook_recall(primary, dict(query="정산 기록", limit=10, snippet=80), resident)
        assert not packet["misses"]
        by_bundle = {}
        for recalled in packet["memories"]:
            by_bundle.setdefault(recalled["bundle"], []).append(recalled)
        assert set(by_bundle) == {"main", "t2m", "sq"}
        assert len(by_bundle["main"]) == 3 and len(by_bundle["t2m"]) == 4 and len(by_bundle["sq"]) == 1
        assert {r["bundle_state"] for r in by_bundle["t2m"]} == {"warm"}
        assert all(r["vrs"] is not None and r["verdict"] in {"available", "retained"}
                   for r in by_bundle["t2m"])
        t2m_status = next(b for b in packet["bundles"] if b["bundle"] == "t2m")
        assert t2m_status["complete_vrs"] is True
        assert t2m_status["stage_order"] == ["deja_vu", "recall", "replay", "re_evidence"]

        t2m_id = by_bundle["t2m"][0]["episode_id"]
        assert resident.lookup(t2m_id) == "t2m"
        assert resident.lookup(by_bundle["main"][0]["episode_id"]) == "main"
        states = {b["id"]: b["state"] for b in resident.status()}
        assert states == {"main": "hot", "t2m": "warm", "sq": "hot"}
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
            bundles=SimpleNamespace(hot=OrderedDict((("s1", owners[1]), ("s2", owners[2])))))
        receipts = _consolidate_stale_shards(daemon, cycles=4)
        assert [r["shard"] for r in receipts] == ["main", "s1", "s2"]
        assert all(r["committed"] and r["parallel_shards"] == 3 for r in receipts)
        assert all(r["workers_per_shard"] >= 1 for r in receipts)
        assert all(owner.graph.stable is not None and not owner.consolidation_stale()
                   for owner in owners)
    finally:
        for owner in owners:
            owner.close()
