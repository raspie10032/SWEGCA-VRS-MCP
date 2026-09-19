# -*- coding: utf-8 -*-
"""G8 (2026-09-19): a judgment's hits and misses are counted and timed, never hidden — blob decodes per packet,
the preparer's prefetch makes the first judgments hot, a starting daemon is a named miss with a start marker."""
import time

from swegca_vrs2.loopback import hook_recall, starting_since, _wait_for_daemon, DaemonStarting, START_FILE
from swegca_vrs2.store import Main


def rows(n):
    return [dict(request_id=f"g8:{i}", text=f"정산 배치 훅 데몬 기록 {i} — 압축 스냅샷 영수증", source=f"g8/log#{i}",
                 revision="1", metadata=dict(kind="log_entry", project="g8")) for i in range(n)]


def test_packet_counts_blob_decodes_and_prefetch_makes_the_judgment_hot(tmp_path):
    m = Main(tmp_path / "s", allow_ingest=True)
    try:
        m.ingest_many(rows(12))
        store = m.memory._store
        store["cache"].clear(); store.get("light_cache", {}).clear()          # as after a checkpoint load
        cold = hook_recall(m, dict(query="정산 배치 훅", limit=5, snippet=80))
        assert cold["blob_decodes"] > 0 and any(x["kind"] == "blob_decodes" for x in cold["misses"])
        assert set(cold["timing"]) == {"judgment_ms", "rows_ms", "bundles_ms"}
        warm = hook_recall(m, dict(query="정산 배치 훅", limit=5, snippet=80))
        assert warm["blob_decodes"] == 0 and warm["blob_hits"] > 0 and not warm["misses"]
        store["cache"].clear(); store.get("light_cache", {}).clear()
        assert m.memory.prefetch_light() == 12                                 # the preparer's start-up pass
        first = hook_recall(m, dict(query="정산 배치 훅", limit=5, snippet=80))
        assert first["blob_decodes"] == 0 and not first["misses"]              # hot from the first judgment
        assert m.memory.prefetch_light() == 0
    finally:
        m.close()


def test_start_marker_names_a_starting_daemon_instead_of_blocking(tmp_path):
    assert starting_since(tmp_path) is None
    (tmp_path / START_FILE).write_text(f"12345 {time.time():.3f}", encoding="ascii")
    assert 0 <= starting_since(tmp_path) < 5
    began = time.perf_counter()
    try:
        _wait_for_daemon(tmp_path, wait_seconds=0.5)
        raise AssertionError("expected DaemonStarting")
    except DaemonStarting:
        pass
    assert time.perf_counter() - began < 2.0                                   # bounded: the prompt is not held
    (tmp_path / START_FILE).write_text(f"12345 {time.time() - 3600:.3f}", encoding="ascii")
    assert starting_since(tmp_path) is None                                    # a stale marker is ignored
