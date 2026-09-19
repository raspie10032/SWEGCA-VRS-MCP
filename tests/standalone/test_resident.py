# -*- coding: utf-8 -*-
"""Resident layer — minimal G7 (2026-09-19): one primary Main plus other bundles answered warm (index only) or
hot by use; hook_recall reaches every bundle; eviction beyond hot_limit checkpoints and closes; a warm view
follows the bundle's journal; the directory says which bundle holds a record id."""
import pickle
import sqlite3
import zlib

from swegca_vrs2.loopback import hook_recall
from swegca_vrs2.resident import Resident, WarmView, warm_recall, load_warm_memory
from swegca_vrs2.store import Main, CHECKPOINT_MAGIC


def row(n, text, project, kind="log_entry"):
    return dict(request_id=f"{project}:{n}", text=text, source=f"{project}/session-log.md#{n}", revision="1",
                metadata=dict(kind=kind, project=project))


def test_checkpoint_writes_the_warm_blob_and_a_warm_view_loads_it_without_the_graph(tmp_path):
    m = Main(tmp_path / "b", allow_ingest=True)
    try:
        m.ingest_many([row(i, f"정산 배치 훅 데몬 이야기 {i}", "b") for i in range(5)])
        m.checkpoint()
        db = sqlite3.connect(str(tmp_path / "b" / "memory.sqlite3"))
        seq, blob = db.execute("SELECT seq, blob FROM checkpoint_warm WHERE id=1").fetchone()
        full = db.execute("SELECT blob FROM checkpoint WHERE id=1").fetchone()[0]
        assert blob[:2] == CHECKPOINT_MAGIC and len(blob) < len(full)
        memory = pickle.loads(zlib.decompress(blob[2:]))
        assert memory.episode_count == 5 and not hasattr(memory, "csr")          # the index alone
        m.ingest(row(5, "체크포인트 뒤에 들어온 여섯째 기록 정산", "b"))          # journal tail after the checkpoint
        warm, covered = load_warm_memory(db)
        assert warm.episode_count == 6 and covered > seq
        db.close()
    finally:
        m.close()


def test_resident_recalls_across_bundles_and_evicts_to_warm(tmp_path):
    primary = Main(tmp_path / "main", allow_ingest=True)
    resident = Resident(primary, {"t2m": tmp_path / "t2m", "sq": tmp_path / "sq"}, hot_limit=1)
    try:
        primary.ingest_many([row(i, f"주 뭉치의 정산 배치 기록 {i}", "main") for i in range(3)])
        # ingest routed to other bundles opens them hot; the second opening evicts the first (hot_limit=1)
        resident.main_for("t2m").ingest_many([row(i, f"티투엠 안드로이드 배포 기록 정산 {i}", "t2m") for i in range(4)])
        assert list(resident.hot) == ["t2m"]
        resident.main_for("sq").ingest(row(0, "에스큐 월배치 정산 기록", "sq"))
        assert list(resident.hot) == ["sq"]                                     # t2m evicted: checkpoint + close in the background
        resident.settle()
        assert not resident.closing
        packet = hook_recall(primary, dict(query="정산 기록", limit=10, snippet=80), resident)
        by_bundle = {}
        for r in packet["memories"]:
            by_bundle.setdefault(r["bundle"], []).append(r)
        assert set(by_bundle) == {"main", "t2m", "sq"}
        assert len(by_bundle["main"]) == 3 and len(by_bundle["t2m"]) == 4 and len(by_bundle["sq"]) == 1
        assert {r["bundle_state"] for r in by_bundle["t2m"]} == {"warm"} and {r["bundle_state"] for r in by_bundle["sq"]} == {"hot"}
        assert all(r["vrs"] is None and r["region"] is None and r["fanout"] for r in by_bundle["t2m"])
        assert [b["bundle"] for b in packet["bundles"]] == ["t2m", "sq"] and packet["bundles"][0]["state"] == "warm"
        # the directory: which bundle holds an id
        t2m_id = by_bundle["t2m"][0]["episode_id"]
        assert resident.lookup(t2m_id) == "t2m" and resident.lookup(by_bundle["main"][0]["episode_id"]) == "main"
        assert resident.lookup("memory:" + "0" * 64) is None
        states = {b["id"]: b["state"] for b in resident.status()}
        assert states == {"main": "hot", "t2m": "warm", "sq": "hot"}
    finally:
        resident.close()
        primary.close()


def test_a_warm_view_follows_the_journal_of_another_owner(tmp_path):
    owner = Main(tmp_path / "b", allow_ingest=True)
    try:
        owner.ingest_many([row(i, f"소유자가 쓴 기록 정산 {i}", "b") for i in range(3)])
        owner.checkpoint()
        view = WarmView("b", tmp_path / "b")                                     # another process's read-only view
        memory = view.refresh()
        assert memory.episode_count == 3 and view.status()["state"] == "warm"
        owner.ingest(row(3, "체크포인트 뒤 소유자가 더 쓴 기록 정산", "b"))       # tail only: no reload
        assert view.refresh().episode_count == 4 and view.checkpoint_seq == 3
        owner.ingest(row(4, "다섯째 기록 정산", "b"))
        owner.checkpoint()                                                        # newer checkpoint: reload from its warm blob
        assert view.refresh().episode_count == 5 and view.checkpoint_seq == 5
        rows = warm_recall(view.memory, "정산 기록", limit=10)["rows"]
        assert len(rows) == 5 and all(r["vrs"] is None for r in rows)
        view.close()
    finally:
        owner.close()


def test_packet_rows_with_nested_metadata_encode(tmp_path):
    """A record's metadata is frozen in the store (nested maps become mappingproxy); the origin binding was the
    first nested map and the daemon's JSON reply crashed on it (2026-09-19, live) — packets carry plain data."""
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
        assert encode(dict(rows=warm_recall(m.memory, "정산 배치")["rows"]))
    finally:
        m.close()
