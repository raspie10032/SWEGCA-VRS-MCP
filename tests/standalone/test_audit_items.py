# -*- coding: utf-8 -*-
"""The 32-item audit, worked in order (2026-09-21 「순서대로 고치자」): 4 thinking in the turn, 14 the project wall
opens for a named project, 16 snapshot ↔ partial row, 24 a kind-routed bundle, 27 the per-log lock, the reindex
conflict the item-22 measurement exposed, 31 the premise is checkable, 33 the bridge refreshes a stale snapshot."""
import io
import json
import os
import threading
import time

import pytest

from swegca_vrs2.harness import precompact
from swegca_vrs2.harness import recall
from swegca_vrs2.harness import reindex
from swegca_vrs2.harness import transcripts as t
from swegca_vrs2.loopback import Daemon   # 2.2 note: these tests cover the tail and main's store directly (session_layer=False); the
                                          # layer's own path (tail -> proposal journal -> merge) is in test_session_layer.py
from swegca_vrs2.store import Main
from tests.standalone.test_transcripts import Via, rec, write


@pytest.fixture
def sandbox(tmp_path, monkeypatch):
    monkeypatch.setattr(t, "STATE_DIR", str(tmp_path / "tail"))
    monkeypatch.setattr(t, "RECEIPT", str(tmp_path / "tail.log"))
    return tmp_path


def thinking_rec(text, thought):
    r = json.loads(rec("assistant", text))
    r["message"]["content"].insert(0, dict(type="thinking", thinking=thought, signature="sig"))
    r["message"]["content"].append(dict(type="thinking", thinking="", signature="only-a-signature"))
    return json.dumps(r, ensure_ascii=False)


def test_item4_the_turn_keeps_a_bounded_excerpt_of_the_reasoning(sandbox):
    log = str(sandbox / "think.jsonl")
    write(log, [rec("user", "왜 그 파일을 골랐나", ts="2026-09-21T03:00:00.000Z"),
                thinking_rec("chunk_7 이 가장 최근이라 골랐다", "The user asks why chunk_7. It was the newest file and the only one with twelve lines. " * 12)])
    out = t.run(log, trigger="stop", project="proj", dry_run=True)
    text = out["texts"][0]
    assert "\n사고: The user asks why chunk_7." in text and text.index("어시스턴트:") < text.index("사고:")
    line = next(l for l in text.splitlines() if l.startswith("사고: "))
    assert len(line) <= len("사고: ") + t.THINKING_CHARS                       # bounded, not the whole reasoning
    d = Daemon(sandbox / "store", allow_ingest=True, idle_seconds=3600, session_layer=False)
    try:
        sent = t.run(log, trigger="stop", project="proj", client=Via(d))
        assert sent["rows"] == 1
        row = d.handle(dict(command="hook_recall", query="왜 chunk_7 골랐나", limit=5, snippet=700))["memories"][0]
        assert row["metadata"]["thinking"] > t.THINKING_CHARS and "사고:" in row["text"]   # the full length is counted
    finally:
        d.close()


def test_item14_a_project_the_prompt_names_passes_the_wall_with_a_slot_of_its_own():
    here, t2m, sqlite = "C--Users-asm-Desktop-----", "C--Users-asm-Desktop-T2M--------", "C--Users-asm-Desktop-SQLITE"
    assert recall.project_names(t2m) == {"t2m"} and recall.project_names(here) == set()
    def row(project, kind, text, source):
        return dict(source=source, text=text, text_chars=len(text), matched_cues=["캐시", "세션", "압축"], asks="",
                    metadata=dict(project=project, kind=kind), evidence=[])
    packet = dict(query="T2M 캐시 세션 압축 뒤 어땠나", record_count=100, fanout={},
                  memories=[row(t2m, "transcript", "캐시 세션 압축 이야기", "transcript:claude-code/aaa#1-9"),
                            row(here, "transcript", "캐시 세션 압축 이야기 여기", "transcript:claude-code/bbb#1-9"),
                            row(sqlite, "transcript", "캐시 세션 압축 sqlite", "transcript:claude-code/ccc#1-9"),
                            row(t2m, "log_entry", "캐시 세션 압축 로그", "C--T2M/session-log.md#1")])
    stems = recall.prompt_stems(packet["query"])
    assert recall.named_projects(packet["memories"], stems, here) == {t2m}
    _, records = recall.choose(packet, here, stems)
    sources = [r["source"] for r in records]
    assert "transcript:claude-code/bbb#1-9" in sources and "transcript:claude-code/aaa#1-9" in sources   # this project's turn and the named one
    assert "transcript:claude-code/ccc#1-9" not in sources                                            # SQLITE was not named
    assert sources.index("transcript:claude-code/bbb#1-9") < sources.index("transcript:claude-code/aaa#1-9")
    text = recall.render(packet, [], records)
    assert "프로젝트 t2m" in text


def test_item16_the_snapshot_entry_and_the_cut_row_name_each_other(sandbox, monkeypatch):
    log = str(sandbox / "s.jsonl")
    write(log, [rec("user", "첫 질문", ts="2026-09-21T03:00:00.000Z"), rec("assistant", "첫 답"),
                rec("user", "둘째 질문 압축 직전", ts="2026-09-21T03:10:00.000Z"), rec("assistant", "둘째 답 진행 중")])
    memory = sandbox / "proj" / "memory"
    memory.mkdir(parents=True)
    session_log = memory / "session-log.md"
    session_log.write_text("- 2026-09-21 02:0x: 앞선 항목\n", encoding="utf-8")
    monkeypatch.setattr(precompact, "resolve", lambda cwd: ("proj", str(memory)))
    monkeypatch.setattr(precompact, "RECEIPT", str(sandbox / "pre.log"), raising=False)
    monkeypatch.setattr("sys.stdin", io.StringIO(json.dumps(dict(transcript_path=log, cwd=str(sandbox), trigger="auto", session_id="s1"))))
    precompact.main([])
    entry = session_log.read_text(encoding="utf-8").splitlines()[-1]
    assert "로그 위치: s.jsonl#3-4 (턴 2)" in entry                        # the entry names the turn the tail cuts
    assert os.path.isfile(t.snapshot_marker(log))
    d = Daemon(sandbox / "store", allow_ingest=True, idle_seconds=3600, session_layer=False)
    try:
        out = t.run(log, trigger="precompact", project="proj", client=Via(d), force_cut=True)
        assert out["parts"] == {"whole": 1, "partial": 1}
        rows = d.handle(dict(command="turns", session=out["session"], limit=5))["rows"]
        cut = next(r for r in rows if r["part"] == "partial")
        assert cut["snapshot"]["line"] == 2 and cut["snapshot"]["log"].endswith("session-log.md")
        assert "[압축 스냅샷: session-log.md 2행]" in cut["text"]
        assert not os.path.isfile(t.snapshot_marker(log))                  # consumed
    finally:
        d.close()
    # the other order: the tail cut first and left its own marker; the snapshot hook reads the position from it
    write(log, [rec("user", "셋째", ts="2026-09-21T03:20:00.000Z"), rec("assistant", "셋째 답")], mode="a")
    out = t.run(log, trigger="precompact", project="proj", dry_run=False, force_cut=True, client=type("C", (), dict(request=lambda self, c, **a: dict(status="observations_recorded", results=[{}] * len(a.get("rows", []))), close=lambda self: None))())
    assert precompact.cut_position(log)[0] == [5, 6]


def test_item24_turns_route_to_their_own_bundle_when_configured(sandbox, monkeypatch):
    log = str(sandbox / "b.jsonl")
    write(log, [rec("user", "질문", ts="2026-09-21T03:00:00.000Z"), rec("assistant", "답")])
    seen = []

    class Client:
        def request(self, command, **a):
            seen.append((command, a.get("bundle")))
            return dict(status="observations_recorded", results=[{}] * len(a.get("rows", [])))

        def close(self):
            pass
    monkeypatch.setattr(t, "BUNDLE_OF", {"kind:transcript": "turns", "proj": "projbundle"})
    t.run(log, trigger="stop", project="proj", client=Client())
    assert seen == [("ingest_many", "turns")]
    monkeypatch.setattr(t, "BUNDLE_OF", {"proj": "projbundle"})
    write(log, [rec("user", "둘", ts="2026-09-21T03:01:00.000Z"), rec("assistant", "답2")], mode="a")
    t.run(log, trigger="stop", project="proj", client=Client())
    assert seen[-1] == ("ingest_many", "projbundle")


def test_item27_one_log_one_run_at_a_time(sandbox):
    log = str(sandbox / "l.jsonl")
    write(log, [rec("user", "질문", ts="2026-09-21T03:00:00.000Z"), rec("assistant", "답")])
    lock = t.acquire_lock(log)
    assert lock and os.path.isfile(lock)
    out = t.run(log, trigger="stop", project="proj", dry_run=True)
    assert out.get("skip") == "locked"                                       # the second runner steps aside
    t.release_lock(lock)
    assert t.run(log, trigger="stop", project="proj", dry_run=True)["rows"] == 1
    assert not os.path.isfile(t.state_path(log) + ".lock")                     # released after the run
    stale = t.acquire_lock(log)
    os.utime(stale, (time.time() - 600, time.time() - 600))
    assert t.run(log, trigger="stop", project="proj", dry_run=True)["rows"] == 1   # a dead run's lock is taken over


def test_reindex_reissues_a_request_id_the_daemon_holds_with_other_content(tmp_path):
    main = Main(tmp_path / "store", allow_ingest=True)
    d = type("D", (), {})()
    try:
        first = main.ingest(dict(request_id="log:proj/session-log.md#abc@r1", text="항목 본문 [코드 원장] 첫 틱", source="proj/session-log.md#abc",
                                 revision="r1", metadata=dict(kind="log_entry", project="proj")))
        args = dict(request_id="log:proj/session-log.md#abc@r1", text="항목 본문 [코드 원장] 다른 틱", source="proj/session-log.md#abc",
                    revision="r1", metadata=dict(kind="log_entry", project="proj"))
        with pytest.raises(ValueError, match="request_id_reused_with_different_content"):
            main.ingest(args)

        class Client:
            def request(self, command, **a):
                if command == "operation":
                    existing = main.operations.get(a["request_id"])
                    return dict(status="ok", episode_id=existing[1] if existing else None)
                return main.ingest(a)
        again = reindex.reissue(Client(), args)
        assert again["request_id"].startswith("log:proj/session-log.md#abc@r1+") and again["supersedes"] == first["episode_id"]
        out = main.ingest(again)
        assert out["episode_id"] != first["episode_id"] and first["episode_id"] in main.memory.superseded
    finally:
        main.close()


def test_item31_the_premise_is_checkable_from_the_settings_file(tmp_path, monkeypatch):
    monkeypatch.setattr(t, "RECEIPT", str(tmp_path / "none.log"))
    settings = tmp_path / "settings.json"
    hooks = {e: [dict(hooks=[dict(type="command", command="python transcript_tail.py")])] for e in t.PREMISE_EVENTS}
    hooks["PostToolUse"] = [dict(matcher="Read|Bash|PowerShell|mcp__x", hooks=[dict(type="command", command="python memory_use_log.py")])]
    settings.write_text(json.dumps(dict(hooks=hooks)), encoding="utf-8")
    p = t.premise(str(settings))
    assert p["ok"] and all(p["events"].values()) and p["use_log"] is True and p["last_stop"] is None
    hooks["PreCompact"] = []
    hooks["PostToolUse"][0]["matcher"] = "Read"
    settings.write_text(json.dumps(dict(hooks=hooks)), encoding="utf-8")
    p = t.premise(str(settings))
    assert not p["ok"] and p["events"]["PreCompact"] is False and p["use_log"] is False
    # the installer wires exactly this
    import re
    src = io.open(os.path.join(os.path.dirname(t.__file__), "..", "..", "..", "local", "tools", "vrs2-install.py"), encoding="utf-8").read()
    assert re.search(r'"transcript_tail.py": \("transcripts", "main\(\)", \("Stop", ?"SubagentStop", ?"PreCompact", ?"SessionStart"\)', src)
    assert '"Read|Bash|PowerShell|' in src


def test_item33_the_bridge_refreshes_a_snapshot_that_real_time_ingestion_made_stale(tmp_path):
    from swegca_vrs2.server import LocalResident, LoopbackMCP
    main = Main(tmp_path / "store", allow_ingest=True)
    try:
        main.ingest(dict(request_id="a", text="퀘이사 정렬 결과는 chunk_7", source="s:a", revision="r"))
        bridge = LoopbackMCP(LocalResident(main), True)
        stale = bridge.call_tool("memory_status", {})["pair_snapshot_id"]
        main.ingest(dict(request_id="b", text="정렬 뒤 열두 줄", source="s:b", revision="r"))    # the tail's row, between the two calls
        assert main.status()["pair_snapshot_id"] != stale
        result = bridge.call_tool("memory_context", dict(request_id="q1", query="퀘이사 정렬", expected_pair_snapshot_id=stale))
        assert result["status"] == "memory_context_ready"
        assert result["snapshot_refreshed"]["expected"] == stale and result["snapshot_refreshed"]["used"] == main.status()["pair_snapshot_id"]
        fresh = bridge.call_tool("memory_context", dict(request_id="q2", query="퀘이사 정렬", expected_pair_snapshot_id=main.status()["pair_snapshot_id"]))
        assert "snapshot_refreshed" not in fresh                             # nothing to refresh: silent
    finally:
        main.close()
