# -*- coding: utf-8 -*-
"""Real-time transcript ingestion (2026-09-21): every turn of a conversation log enters the store as an
experience through the daemon, and comes back through the store with its exact place in the log.

The premise these tests keep: nothing here reads the log to answer — a question is answered by what the
store recalls, and the recalled experience names the log position (span-verified). A test that found the
turn by grepping the log would be discarded."""
import json
import os
import time

import pytest

from swegca_vrs2.harness import origin as origin_mod
from swegca_vrs2.harness import recall
from swegca_vrs2.harness import transcripts as t
from swegca_vrs2.loopback import Daemon


class Via:
    """The tail's client, straight into a Daemon (what the loopback client does over a socket)."""
    def __init__(self, daemon):
        self.daemon = daemon

    def request(self, command, **arguments):
        out = self.daemon.handle(dict(command=command, **arguments))
        if out.get("status") not in ("ok", "observation_recorded", "observations_recorded"):
            raise RuntimeError(out)
        return out

    def close(self):
        pass


def rec(kind, text=None, *, tools=(), ts="2026-09-21T03:00:00.000Z", session="s1", extra=None):
    row = dict(type=kind, sessionId=session, cwd="C:/work/proj", timestamp=ts, uuid=f"u{time.time_ns()}")
    if kind == "user":
        row["message"] = dict(role="user", content=[dict(type="text", text=text)] if text else [dict(type="tool_result", tool_use_id="x", content="…")])
    elif kind == "assistant":
        blocks = ([dict(type="text", text=text)] if text else []) + [dict(type="tool_use", id=f"t{i}", name=n, input=i_) for i, (n, i_) in enumerate(tools)]
        row["message"] = dict(role="assistant", model="claude-test", content=blocks, usage=dict(input_tokens=10, output_tokens=5))
    row.update(extra or {})
    return json.dumps(row, ensure_ascii=False)


def boundary(pre=500000, post=20000):
    return json.dumps(dict(type="system", subtype="compact_boundary", sessionId="s1", timestamp="2026-09-21T03:30:00.000Z",
                           compactMetadata=dict(trigger="auto", preTokens=pre, postTokens=post, durationMs=1200)))


def write(path, lines, mode="w"):
    with open(path, mode, encoding="utf-8", newline="\n") as out:
        out.write("".join(l + "\n" for l in lines))


FILLER = ["오늘 점심은 국수를 먹었다 배가 부르다", "근태 표 서식을 두 줄 고쳤다 서식 검토", "빌드 시간이 사 분 걸린다 캐시를 켰다",
          "휴대폰 재고 시트를 다시 정렬했다 색상 열", "회의록 초안을 팀에 보냈다 답장을 기다린다", "스프레드시트 수식이 순환 참조였다",
          "프린터 드라이버를 다시 깔았다 출력 정상", "시계를 맞췄다 오차 삼 초", "새 폴더를 만들어 사진을 옮겼다", "비밀번호 규칙을 문서에 적었다",
          "월정책 대시보드 색을 바꿨다", "출근 기록 열 개를 대조했다", "우편물 주소를 갱신했다", "달력 공유 설정을 검토했다",
          "정산 스크립트 주석을 정리했다", "테스트 이름을 바꿨다", "그래프 축 단위를 고쳤다", "메일 라벨을 두 개 만들었다",
          "백업 폴더 용량을 확인했다", "환경 변수 이름을 통일했다", "글꼴 크기를 키웠다", "커피 원두를 주문했다"]


def filler_lines(n):
    lines = []
    for i in range(n):
        lines.append(rec("user", f"{FILLER[i % len(FILLER)]} {i}", ts=f"2026-09-21T02:{i % 60:02d}:00.000Z"))
        lines.append(rec("assistant", f"알겠다 {FILLER[(i * 7) % len(FILLER)]} 처리했다 {i}", ts=f"2026-09-21T02:{i % 60:02d}:30.000Z"))
    return lines


@pytest.fixture
def sandbox(tmp_path, monkeypatch):
    monkeypatch.setattr(t, "STATE_DIR", str(tmp_path / "tail"))
    monkeypatch.setattr(t, "RECEIPT", str(tmp_path / "tail.log"))
    return tmp_path


def test_turns_enter_through_the_store_and_come_back_with_their_place(sandbox):
    log = str(sandbox / "s1.jsonl")
    lines = filler_lines(22)
    lines += [rec("user", "퀘이사 정렬 결과를 어디에 적었더라", ts="2026-09-21T03:00:00.000Z"),
              rec("assistant", "퀘이사 정렬 결과는 chunk_7 파일에 적었다 — 열두 줄이다", tools=[("Read", dict(file_path="C:/work/proj/chunk_7.csv"))]),
              rec("user", None),                                       # a tool result: part of the turn, never text
              rec("assistant", "확인했다 열두 줄 맞다")]
    write(log, lines)
    d = Daemon(sandbox / "store", allow_ingest=True, idle_seconds=3600)
    try:
        out = t.run(log, trigger="stop", project="proj", client=Via(d))
        assert out["rows"] == 23 and out["sent"] == 23 and not out["errors"] and out["at_end"] and not out["backlog"]
        # the log grows (a compaction boundary and a new turn): what was said before it is already in the store
        write(log, [boundary(), rec("user", "다음 일 하자", ts="2026-09-21T03:31:00.000Z"), rec("assistant", "그러자")], mode="a")
        packet = d.handle(dict(command="hook_recall", query="퀘이사 정렬 결과 어디 적었지", limit=10, snippet=700))
        turns = [m for m in packet["memories"] if (m.get("metadata") or {}).get("kind") == "transcript"]
        assert turns, packet.get("memories")
        row = turns[0]
        meta = row["metadata"]
        assert meta["project"] == "proj" and meta["producer"] == t.PRODUCER and meta["turn"] == 23 and meta["part"] == "whole"
        assert "퀘이사 정렬 결과는 chunk_7" in row["text"] and "도구: Read proj/chunk_7.csv" in row["text"]
        # the experience names its exact place, and the place is verified from the span alone
        lines_ = meta["origin"]["lines"]
        assert lines_ == [45, 48] and meta["origin"]["bytes"][1] > meta["origin"]["bytes"][0]
        assert origin_mod.verify_span(log, meta["origin"], meta["origin"]["sha256"])["state"] == "intact"
        with open(log, encoding="utf-8") as handle:
            raw = handle.read().split("\n")
        assert "퀘이사 정렬 결과를 어디에 적었더라" in raw[lines_[0] - 1] and "열두 줄 맞다" in raw[lines_[1] - 1]
        # the hook chooses it in the conversation slot and prints the call that opens that place
        verdicts, records = recall.choose(packet, "proj")
        assert any(r is row or r.get("episode_id") == row.get("episode_id") for r in records)
        text = recall.render(packet, verdicts, records)
        assert "대화 — claude-code 세션 s1 턴 23" in text
        assert f'원문 위치: Read file_path="{meta["path"]}" offset=45 limit=4' in text
        # the log rotated under the row: the place is reported changed, not silently trusted
        with open(log, "r+b") as handle:
            handle.seek(meta["origin"]["bytes"][0] + 5)
            handle.write(b"X")
        assert origin_mod.verify_span(log, meta["origin"], meta["origin"]["sha256"])["state"] == "changed"
    finally:
        d.close()


def test_precompact_cuts_the_open_turn_and_stop_brings_its_tail(sandbox):
    log = str(sandbox / "s2.jsonl")
    write(log, [rec("user", "첫 요청 그래프를 그려 줘"), rec("assistant", "그렸다 그래프 완성"),
                rec("user", "둘째 요청 축을 바꿔 줘"), rec("assistant", "축을 바꾸는 중이다", tools=[("Edit", dict(file_path="C:/work/proj/plot.py"))])])
    d = Daemon(sandbox / "store", allow_ingest=True, idle_seconds=3600)
    try:
        # PreCompact: the second turn is not finished — it is cut and sent now, before the context is lost
        out = t.run(log, trigger="precompact", project="proj", force_cut=True, client=Via(d))
        assert out["rows"] == 2 and out["parts"] == {"whole": 1, "partial": 1}
        state = t.load_state(log)
        assert state["turn"] == 2 and state["line"] == 5
        # after the compaction the turn ends, a third turn follows; Stop sends the tail and the new turn only
        write(log, [boundary(), rec("assistant", "축을 바꿨다 완료"), rec("user", "셋째 요청 저장해"), rec("assistant", "저장했다")], mode="a")
        out = t.run(log, trigger="stop", project="proj", client=Via(d))
        assert out["rows"] == 2 and out["parts"] == {"whole": 1, "tail": 1} and out["turns"] == [2, 3]
        page = d.handle(dict(command="origins", kinds=["transcript"], limit=100))
        rows = sorted(page["rows"], key=lambda r: r["source"])
        sources = [r["source"] for r in rows]
        assert sources == ["transcript:claude-code/s1#1-2", "transcript:claude-code/s1#3-4", "transcript:claude-code/s1#5-6", "transcript:claude-code/s1#7-8"]
        tail = d.main.memory.episode(next(r["episode_id"] for r in rows if r["source"].endswith("#5-6"))).steps[0].observation
        assert tail["metadata"]["part"] == "tail" and tail["metadata"]["turn"] == 2 and "[압축 경계 auto · 전 500,000 → 후 20,000 토큰]" in tail["text"]
        for r in rows:                                    # every span still verifies against the log
            meta = d.main.memory.episode(r["episode_id"]).steps[0].observation["metadata"]
            assert origin_mod.verify_span(log, meta["origin"], meta["origin"]["sha256"])["state"] == "intact"
        # nothing new: a run sends nothing and moves nothing
        out = t.run(log, trigger="stop", project="proj", client=Via(d))
        assert out["rows"] == 0 and t.load_state(log)["line"] == 9
    finally:
        d.close()


def test_budget_backlog_replay_and_a_generic_agent_log(sandbox, monkeypatch):
    monkeypatch.setattr(t, "ROWS_PER_RUN", 2)
    log = str(sandbox / "s3.jsonl")
    write(log, filler_lines(5))
    d = Daemon(sandbox / "store", allow_ingest=True, idle_seconds=3600)
    try:
        counts = []
        for _ in range(4):
            out = t.run(log, trigger="stop", project="proj", client=Via(d))
            counts.append((out["rows"], out["backlog"]))
        assert counts == [(2, True), (2, True), (1, False), (0, False)]
        # an agent without hooks: a role/content jsonl tailed by the watcher path, cut only when quiet
        other = str(sandbox / "codex" / "rollout.jsonl")
        os.makedirs(os.path.dirname(other))
        write(other, [json.dumps(dict(timestamp="2026-09-21T04:00:00Z", payload=dict(role="user", content=[dict(type="input_text", text="레이아웃 표를 만들어 줘")]))),
                      json.dumps(dict(payload=dict(type="function_call", name="shell", arguments='{"cmd": "ls"}'))),
                      json.dumps(dict(payload=dict(role="assistant", content=[dict(type="output_text", text="레이아웃 표를 만들었다 세 열")])))])
        assert t.detect_format(other) == "messages-jsonl"
        monkeypatch.setattr(t, "QUIET_S", 3600.0)
        out = t.run(other, trigger="watch", agent="codex", project="proj", client=Via(d))
        assert out["rows"] == 0                          # the open turn waits while the log may still be written
        monkeypatch.setattr(t, "QUIET_S", 0.0)
        out = t.run(other, trigger="watch", agent="codex", project="proj", client=Via(d))
        assert out["rows"] == 1 and out["sent"] == 1
        page = d.handle(dict(command="origins", kinds=["transcript"], limit=100))
        row = next(r for r in page["rows"] if r["source"].startswith("transcript:codex/"))
        obs = d.main.memory.episode(row["episode_id"]).steps[0].observation
        assert obs["metadata"]["agent"] == "codex" and "도구: shell" in obs["text"] and "레이아웃 표를 만들었다" in obs["text"]
    finally:
        d.close()


def test_hook_entry_routes_events_and_sweeps_a_dead_session(sandbox, monkeypatch):
    d = Daemon(sandbox / "store", allow_ingest=True, idle_seconds=3600)
    import swegca_vrs2.loopback as loopback
    monkeypatch.setattr(loopback, "ensure_daemon", lambda *a, **k: Via(d))
    project = str(sandbox / "proj")
    os.makedirs(os.path.join(project, "dead", "subagents"))
    live = os.path.join(project, "live.jsonl")
    dead = os.path.join(project, "dead.jsonl")
    sub = os.path.join(project, "dead", "subagents", "agent-1.jsonl")
    write(live, [rec("user", "살아 있는 세션 첫 요청"), rec("assistant", "답했다")])
    write(dead, [rec("user", "죽은 세션의 마지막 요청", session="dead"), rec("assistant", "답하다 말았다", session="dead")])
    write(sub, [rec("user", "위임 과제 표를 세어라", session="agent-1", extra=dict(isSidechain=True)), rec("assistant", "셌다 열둘", session="agent-1", extra=dict(isSidechain=True))])
    try:
        # Stop of the live session: its turn goes in; the dead session was never tailed here, so it is not swept
        t.hook(dict(hook_event_name="Stop", transcript_path=live, cwd=str(sandbox), session_id="live"))
        assert t.load_state(live)["turn"] == 1 and t.load_state(dead)["turn"] == 0
        # a SubagentStop names the agent's transcript; its turn is the delegate's, by agent id
        t.hook(dict(hook_event_name="SubagentStop", transcript_path=live, agent_transcript_path=sub, agent_id="agent-1", cwd=str(sandbox), session_id="live"))
        assert t.load_state(sub)["turn"] == 1
        # the dead session was tailed once (its first Stop), then died mid-turn; a later SessionStart sweeps it
        t.run(dead, trigger="stop", client=Via(d))
        write(dead, [rec("user", "죽기 직전 요청", session="dead")], mode="a")
        old = time.time() - 2 * t.SWEEP_IDLE_S
        os.utime(dead, (old, old))
        results = t.hook(dict(hook_event_name="SessionStart", transcript_path=live, cwd=str(sandbox), session_id="live", source="startup"))
        swept = [r for r in results if r.get("trigger") == "session_start:sweep"]
        assert swept and swept[0]["rows"] == 1 and swept[0]["path"] == "dead.jsonl"
        page = d.handle(dict(command="origins", kinds=["transcript"], limit=100))
        assert {r["source"] for r in page["rows"]} >= {"transcript:claude-code/live#1-2", "transcript:claude-code/agent-1#1-2",
                                                        "transcript:claude-code/dead#1-2", "transcript:claude-code/dead#3-3"}
    finally:
        d.close()
