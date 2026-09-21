# -*- coding: utf-8 -*-
"""Antigravity as a third-party agent (2026-09-21): its conversation is a SQLite database of protobuf steps. The
tail reads it in place (read-only), one row per turn with the user's words, the answer, the reasoning and the tool
calls, bound to the step range; the span is verified from the database; the hint opens it with `--show`."""
import json
import os
import sqlite3
import time

from swegca_vrs2.harness import origin as origin_mod
from swegca_vrs2.harness import read_log, recall
from swegca_vrs2.harness import transcripts as t
from swegca_vrs2.loopback import Daemon
from tests.standalone.test_transcripts import Via


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def pb(fields):
    """Encode {field: value} — int → varint, str/bytes → length-delimited, dict → nested message, list → repeated."""
    out = bytearray()
    for field, value in fields.items():
        for v in (value if isinstance(value, list) else [value]):
            if isinstance(v, int):
                out += varint(field << 3) + varint(v)
            else:
                body = pb(v) if isinstance(v, dict) else (v.encode("utf-8") if isinstance(v, str) else v)
                out += varint((field << 3) | 2) + varint(len(body)) + body
    return bytes(out)


def meta(secs, tool=None):
    m = {1: {1: secs, 2: 500}}
    if tool:
        name, args = tool
        m[4] = {1: "abc123", 2: name, 3: json.dumps(args), 9: name}
    return m


def user_step(secs, text):
    return 14, pb({1: 14, 4: 3, 5: meta(secs), 19: {2: text, 3: {1: text}}})


def answer_step(secs, text, thinking=""):
    body = {1: text, 6: "bot-1", 8: text}
    if thinking:
        body[3] = thinking
    return 15, pb({1: 15, 4: 3, 5: meta(secs), 20: body})


def tool_step(secs, kind, name, args):
    return kind, pb({1: kind, 4: 3, 5: meta(secs, (name, args))})


def checkpoint_step(secs):
    return 98, pb({1: 98, 4: 3, 5: meta(secs)})


def make_db(path, steps, workspace="file:///C:/Users/asm/Documents/antigravity/proud-davinci"):
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE steps (idx integer, step_type integer NOT NULL DEFAULT 0, status integer NOT NULL DEFAULT 0, "
               "has_subtrajectory numeric NOT NULL DEFAULT false, metadata blob, error_details blob, permissions blob, task_details blob, "
               "render_info blob, step_payload blob, step_format integer NOT NULL DEFAULT 0, PRIMARY KEY (idx))")
    db.execute("CREATE TABLE trajectory_metadata_blob (id text DEFAULT 'main', data blob, PRIMARY KEY (id))")
    db.execute("INSERT INTO trajectory_metadata_blob VALUES ('main', ?)", (pb({1: {1: workspace, 2: workspace}}),))
    db.executemany("INSERT INTO steps (idx, step_type, status, step_payload) VALUES (?, ?, 3, ?)",
                   [(i, kind, payload) for i, (kind, payload) in enumerate(steps)])
    db.commit()
    db.close()


def add_steps(path, first, steps):
    db = sqlite3.connect(path)
    db.executemany("INSERT INTO steps (idx, step_type, status, step_payload) VALUES (?, ?, 3, ?)",
                   [(first + i, kind, payload) for i, (kind, payload) in enumerate(steps)])
    db.commit()
    db.close()


T0 = 1_780_000_000


def test_a_conversation_database_becomes_turns_with_words_reasoning_tools_and_place(tmp_path):
    path = str(tmp_path / "47144a22-692e-4a30-b913-38d00ccef1b2.db")
    make_db(path, [
        user_step(T0, "sqlite 서버 구축 시작. 엑셀과 tsv 를 db 로"),
        checkpoint_step(T0 + 1),
        answer_step(T0 + 2, "I will look at the workspace first.", "**Planning** an empty workspace, SQLite it is."),
        tool_step(T0 + 3, 9, "list_dir", {"DirectoryPath": "C:\\Users\\asm\\Documents\\antigravity\\proud-davinci"}),
        tool_step(T0 + 4, 21, "run_command", {"CommandLine": "python --version", "Cwd": "C:\\x"}),
        answer_step(T0 + 9, "Python 3.11 is installed; the plan is written."),
        user_step(T0 + 60, "적재용 DB 라는 것을 이해하고 시작해줘"),
        answer_step(T0 + 62, "I will create task.md for the staging database.", "**Staging DB** — load history, chunks."),
    ])
    assert t.detect_format(path) == "antigravity" and t.is_conversation_log(path) and not t.is_conversation_log(path + "-wal")
    assert t._session_of(path, "antigravity") == "47144a22-692e-4a30-b913-38d00ccef1b2"
    assert t.antigravity_cwd(path).replace("\\", "/") == "C:/Users/asm/Documents/antigravity/proud-davinci"
    os.utime(path, (time.time() - 30, time.time() - 30))                     # quiet: the open turn may be taken
    out = t.run(path, trigger="cli", project="proud-davinci", dry_run=True)
    assert out["rows"] == 2 and out["agent"] == "antigravity" and out["session"] == "47144a22"
    first, second = out["texts"]
    assert "사용자: sqlite 서버 구축 시작" in first and "어시스턴트: I will look at the workspace first.\nPython 3.11 is installed" in first
    assert "사고: **Planning** an empty workspace" in first
    assert "도구: list_dir antigravity/proud-davinci · run_command python --version" in first
    assert "[대화 antigravity 세션 47144a22 턴 1 · 2026-05-29 " in first and "턴 2" in second and "적재용 DB" in second


def test_turns_enter_the_store_from_the_database_and_are_verified_from_it(tmp_path, monkeypatch):
    path = str(tmp_path / "cascade1.db")
    make_db(path, [user_step(T0, "퀘이사 정렬 결과를 어디에 적었나"),
                   answer_step(T0 + 2, "퀘이사 정렬 결과는 chunk_7 에 적었다 — 열두 줄이다"),
                   tool_step(T0 + 3, 5, "write_to_file", {"TargetFile": "C:\\work\\chunk_7.csv"})])
    os.utime(path, (time.time() - 30, time.time() - 30))
    d = Daemon(tmp_path / "store", allow_ingest=True, idle_seconds=3600)
    try:
        out = t.run(path, trigger="cli", agent="antigravity", project="proj", client=Via(d))
        assert out["rows"] == 1 and out["sent"] == 1 and not out["errors"]
        packet = d.handle(dict(command="hook_recall", query="퀘이사 정렬 결과 어디 적었지", limit=5, snippet=700))
        row = next(m for m in packet["memories"] if (m.get("metadata") or {}).get("kind") == "transcript")
        meta_ = row["metadata"]
        assert meta_["agent"] == "antigravity" and meta_["session"] == "cascade1" and meta_["lines"] == [1, 3]
        assert meta_["origin"]["format"] == "antigravity" and meta_["origin"]["lines"] == [1, 3]
        assert origin_mod.verify_span(path, meta_["origin"], meta_["origin"]["sha256"])["state"] == "intact"
        # the hint opens the database through the tail, not through Read
        hint = recall.open_hint(row)
        assert 'vrs2-tail.py" --show "' in hint and hint.rstrip().endswith("(대화 턴 1 의 로그 1-3행)") or "--show" in hint
        assert row["_open"] == dict(path=path.replace("\\", "/"), offset=1, limit=3, whole=True)
        # the agent goes on: the next turn is taken from the next step, the earlier span stays intact
        add_steps(path, 3, [user_step(T0 + 60, "다음은 정렬 뒤 검증"), answer_step(T0 + 61, "검증 스크립트를 쓴다")])
        os.utime(path, (time.time() - 30, time.time() - 30))
        again = t.run(path, trigger="cli", agent="antigravity", project="proj", client=Via(d))
        assert again["rows"] == 1 and again["turns"] == [2] and again["lines"] == [4, 5]
        assert origin_mod.verify_span(path, meta_["origin"], meta_["origin"]["sha256"])["state"] == "intact"
        # a rewritten step breaks the span: changed, not intact
        db = sqlite3.connect(path)
        db.execute("UPDATE steps SET step_payload=? WHERE idx=1", (answer_step(T0 + 2, "다른 답")[1],))
        db.commit(); db.close()
        assert origin_mod.verify_span(path, meta_["origin"], meta_["origin"]["sha256"])["state"] == "changed"
    finally:
        d.close()
    # the shell read of the offered database through --show counts as an open, with the step range
    known = {path.replace("\\", "/").casefold()}
    cmd = f'"C:/py/python.exe" "C:/Users/asm/mcp/vrs2-tail.py" --show "{path}" 1 3'
    assert read_log.reads_in_command(cmd, known) == [(path.replace("\\", "/"), 1, 3)]


def test_registered_with_a_short_idle_the_sweep_takes_a_live_agent_within_its_idle(tmp_path, monkeypatch):
    monkeypatch.setattr(t, "STATE_DIR", str(tmp_path / "tail"))
    monkeypatch.setattr(t, "WATCH_FILE", str(tmp_path / "tail" / "watch.json"))
    path = str(tmp_path / "live.db")
    make_db(path, [user_step(T0, "질문 하나"), answer_step(T0 + 1, "답 하나")])
    t.register(str(tmp_path / "*.db"), agent="antigravity", fmt="antigravity", idle=15)
    assert t.load_watch()[0]["idle"] == 15.0
    sent = []

    class Client:
        def request(self, command, **a):
            sent.append(len(a.get("rows", [])))
            return dict(status="observations_recorded", results=[{}] * len(a.get("rows", [])))

        def close(self):
            pass
    os.utime(path, (time.time() - 5, time.time() - 5))                       # 5 s quiet: not yet
    assert t.sweep_all(trigger="daemon", client_factory=Client) == []
    os.utime(path, (time.time() - 20, time.time() - 20))                     # 20 s quiet: taken (the default would wait 600 s)
    out = t.sweep_all(trigger="daemon", client_factory=Client)
    assert len(out) == 1 and out[0]["rows"] == 1 and out[0]["agent"] == "antigravity" and sent == [1]
