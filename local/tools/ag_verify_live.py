# -*- coding: utf-8 -*-
"""End-to-end under the run's conditions: a session journal (agent antigravity) that keeps receiving rows every 0.5 s
(the tail's role) while the bridge — the same ReconnectingClient + LoopbackMCP the MCP exe runs, with the antigravity
hint — does memory_status -> memory_context -> pages -> memory_release, repeated. usage: verify_live.py <state_dir>"""
import json, sys, threading, time
sys.path.insert(0, "C:/Users/asm/mcp/SWEGCA-VRS-MCP-v2/src")
from swegca_vrs2.loopback import LoopbackClient, port_of
from swegca_vrs2.server import ReconnectingClient, LoopbackMCP
sys.stdout.reconfigure(encoding="utf-8")
STATE = sys.argv[1]
SESSION = "verify-live-" + time.strftime("%H%M%S")
feeder = LoopbackClient(port_of(STATE), 60)
stop = threading.Event()
sent = [0]


def row(i):
    return dict(request_id=f"{SESSION}:{i}", text=f"안티그래비티 대화 턴 {i}: 청크 SQLITE {200 * i + 1} 을 썼다, 다음 시작줄 {200 * i + 201}",
                source=f"transcript:antigravity/{SESSION}#{i}-{i}", revision=f"r{i}", outcome="pending", cues=[],
                metadata=dict(kind="transcript", project="C--Users-asm-mcp-ab-compaction", agent="antigravity", session=SESSION, turn=i, lines=[i, i]))


def feed():
    i = 30                                               # distinct ids after the 30 seed rows
    while not stop.is_set():
        i += 1
        feeder.request("ingest_many", session=SESSION, rows=[row(i)])
        sent[0] = i
        time.sleep(0.5)


for i in range(1, 31):                                   # 30 rows before the first read: more than K
    feeder.request("ingest_many", session=SESSION, rows=[row(i)])
t = threading.Thread(target=feed, daemon=True); t.start()
client = ReconnectingClient(STATE, False, session_hint={"agent": "antigravity"})
bridge = LoopbackMCP(client, False)
ok = 0
for n in range(5):
    t0 = time.perf_counter()
    if len(sys.argv) > 2 and sys.argv[2] == 'fresh':
        client.close(); client = ReconnectingClient(STATE, False, session_hint={'agent': 'antigravity'}); bridge = LoopbackMCP(client, False)
    st = bridge.call_tool("memory_status", {})
    pair = st["pair_snapshot_id"]
    ctx = bridge.call_tool("memory_context", dict(request_id=f"v{n}", query="이 대화에서 마지막으로 쓴 chunk 파일과 다음 시작줄", expected_pair_snapshot_id=pair, page_size=3))
    status = ctx["status"]
    if status == "memory_context_ready":
        sel = ctx["main_cue_selection"]["memory_selection"]["data"]
        pages, seen = 1, [m["episode_id"] for m in ctx["memories"]]
        while ctx.get("next_call"):
            ctx = bridge.call_tool("memory_context", ctx["next_call"]["arguments"]); pages += 1
            seen += [m["episode_id"] for m in ctx["memories"]]
        heads = [str(m["replay"]["data"])[:60].replace("\n", " ") for m in ctx["memories"][:1]]
        print(f"#{n} ready layer={sel.get('layer')} session={sel.get('session')} judged={sel.get('judged')} candidates={sel.get('candidate_count')} "
              f"unjudged={len(sel.get('unjudged_ids') or [])} pages={pages} rows={len(seen)} refreshed={'yes' if getattr(bridge, '_refreshed', None) else 'no'} "
              f"{(time.perf_counter()-t0)*1000:.0f} ms sent={sent[0]} head={heads}")
        ok += 1
        bridge.call_tool("memory_release", dict(request_id=f"v{n}", view_id=ctx["view_id"]))
    else:
        print(f"#{n} {status} {json.dumps(ctx, ensure_ascii=False)[:300]}")
        if ctx.get("view_id"):
            bridge.call_tool("memory_release", dict(request_id=f"v{n}", view_id=ctx["view_id"]))
    time.sleep(1.0)
stop.set(); t.join(2)
print("ok", ok, "/ 5; rows sent", 30 + sent[0])
print("session_end ->", json.dumps(feeder.request("session_end", session=SESSION, wait=True), ensure_ascii=False)[:200])
feeder.close(); client.close()
