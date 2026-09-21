# -*- coding: utf-8 -*-
"""2.2 session producer layer (2026-09-21): an active session writes its proposal journal only; a judgment reads it
first and main on a complete miss; the merge transaction commits the journal into main as one generation with a
receipt; a merge the daemon died inside recovers; the bounded top-K judgment is exact."""
import json
import os
import time

import numpy as np
import pytest

from swegca_vrs2.loopback import Daemon, hook_recall
from swegca_vrs2.merge import MergeTransaction, read_journal
from swegca_vrs2.session_producer import SessionLayer, safe_id
from swegca_vrs2.store import Main

WORDS = "회수 병합 세션 저널 판정 영수증 스토어 데몬 훅 프롬프트 생산자 제안 상태 증거 갈등 재발급 복구 단계 지연 규모".split()


def row(i, text, session="s1", **extra):
    return dict(request_id=f"{session}:{i}", text=text, source=f"{session}/log#{i}", revision=f"r{i}", outcome="pending", cues=[],
                metadata=dict(dict(kind="transcript", project="p", agent="claude-code", session=session, turn=i), **extra))


def daemon(tmp_path, **kw):
    return Daemon(tmp_path / "state", allow_ingest=True, idle_seconds=3600, session_idle=kw.pop("session_idle", 3600), **kw)


def test_an_active_session_writes_its_proposal_journal_never_main(tmp_path):
    d = daemon(tmp_path)
    try:
        before = d.main.memory.episode_count
        out = d.handle(dict(command="ingest_many", session="sess-A", rows=[row(1, "세션 A 첫 턴 임시 저널 기록"), row(2, "세션 A 둘째 턴 병합 전")]))
        assert out["layer"] == "session" and out["session"] == "sess-A" and out["added"] == 2
        one = d.handle(dict(command="ingest", session="sess-A", **row(3, "세션 A 셋째 턴 낱개 적재")))
        assert one["layer"] == "session" and one["status"] == "observation_recorded"
        assert d.main.memory.episode_count == before                      # main untouched
        assert d.sessions.main_for("sess-A").memory.episode_count == 3
        assert (tmp_path / "state" / "sessions" / "sess-A" / "memory.sqlite3").is_file()
        # no session named: main as before
        d.handle(dict(command="ingest", **row(9, "세션 없는 적재는 main 으로", session="none")))
        assert d.main.memory.episode_count == before + 1
        st = d.handle(dict(command="sessions"))
        assert st["enabled"] and [s["id"] for s in st["sessions"]] == ["sess-A"] and st["sessions"][0]["records"] == 3
    finally:
        d.close()


def test_judgment_reads_the_session_first_and_main_on_a_complete_miss(tmp_path):
    d = daemon(tmp_path)
    try:
        d.handle(dict(command="ingest_many", rows=[row(i, f"main 의 오래된 경험 {WORDS[i]} 항목 {i}", session="old") for i in range(6)]))
        d.handle(dict(command="ingest_many", session="sess-B", rows=[row(1, "세션 B 에서 정한 것: 병합은 세션 종료 뒤 한 세대로", session="sess-B"),
                                                                      row(2, "세션 B 둘째: 판정은 임시 저널 우선", session="sess-B")]))
        hit = d.handle(dict(command="hook_recall", session="sess-B", query="병합은 세션 종료 뒤 어떻게 하기로 했지", limit=5))
        assert hit["layer"] == "session" and hit["memories"] and all(r["layer"] == "session" for r in hit["memories"])
        assert hit["memories"][0]["source"].startswith("sess-B/") and hit["judged"] is not None
        assert not [m for m in hit["misses"] if m["kind"] == "session_miss"]
        miss = d.handle(dict(command="hook_recall", session="sess-B", query="오래된 경험 항목 갈등 재발급", limit=5))
        assert miss["layer"] == "main" and miss["memories"] and all(r["layer"] == "main" for r in miss["memories"])
        assert [m for m in miss["misses"] if m["kind"] == "session_miss"][0]["state"] == "complete_miss"
        absent = d.handle(dict(command="hook_recall", session="never-seen", query="오래된 경험 항목", limit=5))
        assert absent["layer"] == "main" and [m for m in absent["misses"] if m["kind"] == "session_miss"][0]["state"] == "absent"
        plain = d.handle(dict(command="hook_recall", query="오래된 경험 항목", limit=5))
        assert plain["layer"] == "main" and plain["session"] is None
    finally:
        d.close()


def test_session_end_merges_the_journal_into_main_as_one_generation_with_a_receipt(tmp_path):
    d = daemon(tmp_path)
    try:
        d.handle(dict(command="ingest", **row(0, "main 에 먼저 있던 것", session="base")))
        rows = [row(i, f"세션 C 턴 {i} {WORDS[i]} {WORDS[(i + 3) % len(WORDS)]}", session="sess-C") for i in range(1, 8)]
        d.handle(dict(command="ingest_many", session="sess-C", rows=rows[:4]))
        d.handle(dict(command="ingest_many", session="sess-C", rows=rows[4:]))     # two generations in the proposal
        before_pair = d.main.pair.snapshot_id
        out = d.handle(dict(command="session_end", session="sess-C", wait=True))
        receipt = out["merged"]
        assert receipt["status"] == "completed" and receipt["rows"] == 7 and receipt["added"] == 7 and receipt["replayed"] == 0
        assert receipt["before_pair"] == before_pair and receipt["after_pair"] == d.main.pair.snapshot_id != before_pair
        assert receipt["generations_in_proposal"] == 2 and receipt["dropped"] and receipt["reissued"] == [] and receipt["conflicts"] == []
        assert len(receipt["delta_digest"]) == 64 and len(receipt["evidence_refs"]) == 7
        # one generation in main: every merged row carries the same pair id
        pairs = {d.main.operations[r["request_id"]][2] for r in rows}
        assert len(pairs) == 1 and pairs == {receipt["after_pair"]}
        assert d.main.memory.episode_count == 8
        assert not (tmp_path / "state" / "sessions" / "sess-C").exists()
        journal = d.handle(dict(command="merge_receipts"))["receipts"]
        assert journal[0]["stage"] == "completed" and journal[0]["session"] == "sess-C" and journal[0]["receipt"]["rows"] == 7
        # the judgment now finds the session's rows in main
        got = d.handle(dict(command="hook_recall", session="sess-C", query=f"세션 C 턴 3 {WORDS[3]}", limit=5))
        assert got["layer"] == "main" and got["memories"][0]["source"] == "sess-C/log#3"
        # ending it again: nothing to do
        again = d.handle(dict(command="session_end", session="sess-C", wait=True))
        assert again["journal"] is False
    finally:
        d.close()


def test_held_slots_are_reissued_and_unknown_supersedes_unlinked(tmp_path):
    d = daemon(tmp_path)
    try:
        held = row(5, "다른 생산자가 먼저 적어 둔 내용", session="sess-D")
        d.handle(dict(command="ingest", **held))                                  # main holds request id sess-D:5
        same = row(6, "같은 내용 같은 id", session="sess-D")
        d.handle(dict(command="ingest", **same))                                  # and sess-D:6 with this very content
        mine = dict(row(5, "이 세션이 같은 id 로 적은 다른 내용", session="sess-D"))
        mine["source"] = held["source"]                                           # same source, later revision -> supersedes
        mine["revision"] = "r5b"
        stray = row(7, "main 에 없는 기록을 잇는다고 주장", session="sess-D", note="x")
        stray["supersedes"] = "memory:0000000000000000000000000000000000000000000000000000000000000000"
        d.handle(dict(command="ingest_many", session="sess-D", rows=[mine, same, stray]))
        receipt = d.handle(dict(command="session_end", session="sess-D", wait=True))["merged"]
        assert receipt["status"] == "completed" and receipt["rows"] == 3 and receipt["replayed"] == 1
        assert len(receipt["reissued"]) == 1
        re = receipt["reissued"][0]
        assert re["request_id"] == "sess-D:5" and re["as_request_id"].startswith("sess-D:5+") and re["supersedes"] == d.main.operations["sess-D:5"][1]
        assert receipt["unlinked_supersedes"] == [dict(request_id="sess-D:7", supersedes=stray["supersedes"])]
        assert d.main.operations["sess-D:5"][1] in d.main.memory.superseded         # the held record is superseded by the re-issued row
        assert d.main.memory.episode_count == 4
    finally:
        d.close()


def test_conflicting_propositions_are_listed_not_averaged(tmp_path):
    d = daemon(tmp_path)
    try:
        a = row(1, "명제 X 를 지지한다", session="sess-E1"); a.update(proposition="X", polarity="support")
        d.handle(dict(command="ingest", **a))
        b = row(1, "명제 X 를 반박한다", session="sess-E2"); b.update(proposition="X", polarity="refute")
        d.handle(dict(command="ingest_many", session="sess-E2", rows=[b]))
        receipt = d.handle(dict(command="session_end", session="sess-E2", wait=True))["merged"]
        assert receipt["conflicts"] == [dict(proposition="X", support=["sess-E1"], refute=["sess-E2"], producers=2, decision="abstain")]
        assert d.main.memory.episode_count == 2                                    # both kept: status-unfiltered experience
    finally:
        d.close()


def test_an_incomplete_merge_recovers_on_the_next_start(tmp_path):
    d = daemon(tmp_path)
    d.handle(dict(command="ingest_many", session="sess-F", rows=[row(i, f"세션 F 턴 {i} {WORDS[i]}", session="sess-F") for i in range(3)]))
    d.handle(dict(command="ingest_many", session="sess-G", rows=[row(i, f"세션 G 턴 {i} {WORDS[i + 4]}", session="sess-G") for i in range(2)]))
    # F: the daemon died right after 'prepared' (main untouched); G: after main committed ('memory_committed')
    d.sessions.close_session("sess-F"); d.sessions.close_session("sess-G")
    now = time.time()
    d.main.db.execute("INSERT INTO merge_journal VALUES (?,?,?,?,?,?,?,?,?,?)", ("tf", "sess-F", "prepared", 3, "x" * 64, d.main.pair.snapshot_id, None, now, now, None))
    g_rows = read_journal(d.sessions.path("sess-G"))
    d.main.ingest_many([r for _, r, _, _ in g_rows])
    d.main.db.execute("INSERT INTO merge_journal VALUES (?,?,?,?,?,?,?,?,?,?)", ("tg", "sess-G", "memory_committed", 2, "y" * 64, "p", d.main.pair.snapshot_id, now, now, None))
    d.main.db.execute("INSERT INTO merge_journal VALUES (?,?,?,?,?,?,?,?,?,?)", ("th", "sess-H", "prepared", 1, "z" * 64, "p", None, now, now, None))   # journal vanished
    d.close()
    d = daemon(tmp_path)
    try:
        stages = {r["session"]: r["stage"] for r in d.handle(dict(command="merge_receipts"))["receipts"]}
        assert stages == {"sess-F": "completed", "sess-G": "completed", "sess-H": "rolled_back"}
        assert {r.get("session") for r in d.recovered} == {"sess-F", "sess-G", "sess-H"}
        assert d.main.memory.episode_count == 5 and d.sessions.ids() == []
        assert d.main.operations["sess-F:2"] is not None and d.main.operations["sess-G:1"] is not None
    finally:
        d.close()


def test_idle_sessions_are_due_and_the_merger_takes_them(tmp_path):
    d = daemon(tmp_path, session_idle=0.5)
    try:
        d.handle(dict(command="ingest_many", session="sess-I", rows=[row(1, "유휴 세션의 한 턴", session="sess-I")]))
        assert d.sessions.due() == []
        time.sleep(0.6)
        assert d.sessions.due() == ["sess-I"]
        assert d.merger.submit_due() == ["sess-I"] and d.merger.wait_idle(20)
        assert d.merger.status()["done"][-1]["status"] == "completed" and d.sessions.ids() == []
        assert d.main.operations["sess-I:1"] is not None
    finally:
        d.close()


def test_a_proposal_journal_accepts_a_supersedes_it_cannot_see_and_main_validates_it(tmp_path):
    d = daemon(tmp_path)
    try:
        first = row(1, "문서 절 첫 판", session="doc"); first["source"] = "p/doc.md#0"; first["revision"] = "v1"
        eid = d.handle(dict(command="ingest", **first))["episode_id"]
        second = row(2, "문서 절 둘째 판 (이 세션이 고쳤다)", session="sess-J"); second["source"] = "p/doc.md#0"; second["revision"] = "v2"; second["supersedes"] = eid
        out = d.handle(dict(command="ingest", session="sess-J", **second))
        assert out["layer"] == "session"                                          # accepted unchecked in the proposal journal
        receipt = d.handle(dict(command="session_end", session="sess-J", wait=True))["merged"]
        assert receipt["unlinked_supersedes"] == [] and receipt["reissued"] == []
        assert d.main.memory.superseded[eid] == d.main.operations["sess-J:2"][1]  # bound in main by the successor rule
    finally:
        d.close()


def test_bounded_top_k_judgment_is_exact(tmp_path):
    rng = np.random.default_rng(7)
    m = Main(tmp_path / "bounded", allow_ingest=True)
    try:
        rows = []
        for i in range(300):
            words = " ".join(rng.choice(WORDS, size=int(rng.integers(3, 12))))
            rows.append(dict(request_id=f"b{i}", text=f"{words} 기록 {i}", source=f"b#{i}", revision=f"v{i}", outcome="pending", cues=[],
                             metadata=dict(kind="transcript", project="p")))
        for k in range(0, 300, 50):
            m.ingest_many(rows[k:k + 50])
        for q in ("회수 병합 세션 판정", "영수증 스토어 데몬", "복구 단계 지연 규모 갈등", "생산자 제안 상태 증거 훅 프롬프트"):
            for k in (1, 3, 10, 25):
                full = m.recall(q, None)
                bounded = m.recall(q, None, judge_limit=k)
                f = [c.episode_id for c in full["receipt"]["activation"].recall.candidates]
                b = [c.episode_id for c in bounded["receipt"]["activation"].recall.candidates]
                assert b[:k] == f[:k], (q, k)
                assert sorted(b + bounded["memory_selection"]["unjudged_ids"]) == sorted(f)       # nothing dropped: the rest stay addressable
                assert bounded["memory_selection"]["candidate_count"] == len(f)
                assert bounded["receipt"]["activation"].deja_vu.candidate_count == full["receipt"]["activation"].deja_vu.candidate_count
                assert bounded["receipt"]["activation"].deja_vu.matched_cues == full["receipt"]["activation"].deja_vu.matched_cues
                for x, y in zip(bounded["receipt"]["activation"].recall.candidates[:k], full["receipt"]["activation"].recall.candidates[:k]):
                    assert x == y                                                                   # same objects: matched cues in row order, overlap
                assert bounded["memory_selection"]["judged"] == min(k, len(f)) and full["memory_selection"]["judged"] == len(f)
                assert [j.episode_id for j in bounded["receipt"]["activation"].re_evidence.judgments] == f[:k]
                assert bounded["memory_selection"]["judge_bound"] == ("exact_top_k_under_bounded_gates" if len(f) > k else None)
    finally:
        m.close()


def test_safe_ids():
    assert safe_id("5e9f04f8-ba16-4daa-a148-2bac0eaf94e3") == "5e9f04f8-ba16-4daa-a148-2bac0eaf94e3"
    assert safe_id("a/b\\c d") == "a_b_c_d"
    with pytest.raises(ValueError):
        safe_id("")
    with pytest.raises(ValueError):
        safe_id("..")


def test_the_tail_writes_a_live_session_to_its_journal_and_session_end_merges_it(tmp_path, monkeypatch):
    """The production path end to end: the transcript tail of a live log -> the session's proposal journal (main
    untouched); the judgment with the session id answers from it; ``turns`` after a compaction comes from it; a dead
    log (older than SESSION_IDLE_S) goes to main as before; SessionEnd merges the journal into main."""
    from swegca_vrs2.harness import transcripts as t
    from tests.standalone.test_transcripts import Via, rec, write, boundary
    monkeypatch.setattr(t, "STATE_DIR", str(tmp_path / "tail"))
    monkeypatch.setattr(t, "RECEIPT", str(tmp_path / "tail.log"))
    d = daemon(tmp_path)
    try:
        live = str(tmp_path / "live.jsonl")
        write(live, [rec("user", "퀘이사 정렬 결과를 어디에 적었더라", session="live"),
                     rec("assistant", "퀘이사 정렬 결과는 chunk_7 파일에 적었다 — 열두 줄이다", session="live"),
                     rec("user", "다음은 축 단위를 고치자", session="live"), rec("assistant", "축 단위를 고쳤다", session="live")])
        out = t.run(live, trigger="stop", project="proj", client=Via(d))
        assert out["rows"] == 2 and out["sent"] == 2 and not out["errors"] and out["layer"] == "session"
        assert d.main.memory.episode_count == 0 and d.sessions.ids() == ["live"]
        hit = d.handle(dict(command="hook_recall", session="live", query="퀘이사 정렬 결과 어디 적었지", limit=5, snippet=400))
        assert hit["layer"] == "session" and "chunk_7" in hit["memories"][0]["text"]
        # after a compaction the SessionStart hook asks the store for the session's last turns: the journal answers
        write(live, [boundary()], mode="a")
        page = d.handle(dict(command="turns", session="live", limit=3))
        assert page["count"] == 2 and page["rows"][-1]["turn"] == 2 and page["rows"][-1]["layer"] == "session"
        # a log nobody wrote for longer than the idle bound is not a live session: main, as before
        dead = str(tmp_path / "dead.jsonl")
        write(dead, [rec("user", "옛 세션의 요청", session="dead"), rec("assistant", "옛 세션의 답", session="dead")])
        old = time.time() - 2 * t.SESSION_IDLE_S
        os.utime(dead, (old, old))
        out = t.run(dead, trigger="stop", project="proj", client=Via(d))
        assert out["rows"] == 1 and out["layer"] == "main" and d.main.memory.episode_count == 1 and d.sessions.ids() == ["live"]
        # the producer ends: its journal merges into main as one generation, and the judgment finds it there
        receipt = d.handle(dict(command="session_end", session="live", wait=True))["merged"]
        assert receipt["status"] == "completed" and receipt["rows"] == 2 and receipt["added"] == 2
        assert d.main.memory.episode_count == 3 and d.sessions.ids() == []
        got = d.handle(dict(command="hook_recall", session="live", query="퀘이사 정렬 결과 어디 적었지", limit=5, snippet=400))
        assert got["layer"] == "main" and "chunk_7" in got["memories"][0]["text"]
        assert [m for m in got["misses"] if m["kind"] == "session_miss"][0]["state"] == "absent"
    finally:
        d.close()


def test_the_mcp_admission_reads_a_hinted_agents_live_session_first(tmp_path):
    """2.2 for a hookless agent (Antigravity): its MCP bridge cannot name the cascade; it sends a hint (agent) and the
    daemon resolves it to that agent's live journal. memory_recall's admission then says which layer judged."""
    from swegca_vrs2.server import ReconnectingClient
    d = daemon(tmp_path)
    try:
        d.handle(dict(command="ingest_many", rows=[row(i, f"main 의 오래된 경험 {WORDS[i]} 항목 {i}", session="old") for i in range(6)]))
        d.handle(dict(command="ingest_many", session="cascade-1", rows=[row(1, "안티그래비티 대화: 청크 SQLITE 1201 을 썼다", session="cascade-1", agent="antigravity"),
                                                                        row(2, "안티그래비티 대화: 다음은 SQLITE 1401", session="cascade-1", agent="antigravity")]))
        pair = d.handle(dict(command="status"))["pair_snapshot_id"]
        start = dict(command="cognitive_dialogue_start", profile="memory-only-no-provider", expected_pair_snapshot_id=pair)
        hit = d.handle(dict(start, request_id="r1", query="청크 SQLITE 다음 시작줄", session_hint={"agent": "antigravity"}))
        assert hit["status"] == "queued" and hit["layer"] == "session" and hit["session"] == "cascade-1"
        d.handle(dict(command="cognitive_dialogue_release", request_id="r1", view_id=hit["view_id"]))
        # through the bridge's own tool: memory_context shows the layer in its cue selection and joins the session's rows
        from swegca_vrs2.server import LoopbackMCP
        class Direct:
            session_hint = {"agent": "antigravity"}
            def request(self, command, **arguments):
                if command == "cognitive_dialogue_start" and "session" not in arguments:
                    arguments = dict(arguments, session_hint=self.session_hint)
                return d.handle(dict(arguments, command=command))
            def close(self): pass
        bridge = LoopbackMCP(Direct(), False)
        ctx = bridge.call_tool("memory_context", dict(request_id="m1", query="청크 SQLITE 다음 시작줄", expected_pair_snapshot_id=pair, page_size=4))
        assert ctx["status"] == "memory_context_ready", ctx
        assert ctx["main_cue_selection"]["memory_selection"]["data"]["layer"] == "session"
        assert any("SQLITE" in str(m["replay"]["data"]) for m in ctx["memories"])
        bridge.call_tool("memory_release", dict(request_id="m1", view_id=ctx["view_id"]))
        miss = d.handle(dict(start, request_id="r2", query="오래된 경험 갈등 재발급", session_hint={"agent": "antigravity"}))
        assert miss["layer"] == "main" and miss["session_miss"] == "complete_miss"
        d.handle(dict(command="cognitive_dialogue_release", request_id="r2", view_id=miss["view_id"]))
        other = d.handle(dict(start, request_id="r3", query="청크 SQLITE 다음 시작줄", session_hint={"agent": "nobody"}))
        assert other["layer"] == "main" and "session_miss" not in other      # no live journal for that agent: plain main
        d.handle(dict(command="cognitive_dialogue_release", request_id="r3", view_id=other["view_id"]))
        d.handle(dict(command="session_end", session="cascade-1", wait=True))
        pair = d.handle(dict(command="status"))["pair_snapshot_id"]                 # the merge published a new generation
        ended = d.handle(dict(start, request_id="r4", query="청크 SQLITE 다음 시작줄", session_hint={"agent": "antigravity"}, expected_pair_snapshot_id=pair))
        assert ended["layer"] == "main" and "session_miss" not in ended      # ended and merged: the hint resolves to nothing, main has the rows
    finally:
        d.close()
    # the bridge's client adds the hint to the admission only
    class Fake:
        def __init__(self): self.seen = []
        def request(self, command, **arguments): self.seen.append((command, arguments)); return {}
        def close(self): pass
    c = ReconnectingClient.__new__(ReconnectingClient)
    c.client, c.session_hint, c._ensure = Fake(), {"agent": "antigravity"}, None
    c.request("cognitive_dialogue_start", request_id="x", query="q")
    c.request("status")
    assert c.client.seen[0][1]["session_hint"] == {"agent": "antigravity"} and "session_hint" not in c.client.seen[1][1]
