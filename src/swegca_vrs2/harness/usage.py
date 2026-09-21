#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""usage ledger — 회수 영수증(주입)과 Read(열기)를 맞춰 기록마다 「주입 n·열림 m」을 세고 스토어에 넘긴다 (2026-09-18).

Replay → Re-evidence 의 빠진 고리: 꺼낸 기억을 실제로 열었는지가 어디에도 되먹임되지 않았다. 여기서
`recall_context.log` 의 `injected`(+`opens`: 열기 줄의 path/offset/limit)와 `use: read`(path/offset) 행을
세션·시각·줄 범위로 맞춘다. 열림 = 같은 세션에서 주입 뒤에, 같은 파일을 그 줄 범위 안(offset 겹침)으로 Read.
opens 가 없는 옛 영수증은 문서 단위(같은 파일 Read)로만 맞추되 session-log.md 는 제외(로그를 적으려 여는
Read 와 못 가른다). 결과는 `usage_ledger.json`({source: [injected, opened]}) 에 누적하고, 바뀐 것만 데몬
`usage` 명령으로 넘긴다(저널 행 → 다음 consolidation 의 연관 입력; 승격은 절대 아님 — vrs_refine.USAGE_CAP).

    Stop 훅 stdin: {"session_id": …}  → 그 세션분만 재계산해 넘김
    손으로: python usage_ledger.py --all [--dry-run]   (전체 이력 백필)
"""
import io
import json
import os
import sys
import time

from .paths import RECEIPTS as HOOKS
RECALL_LOG = os.path.join(HOOKS, "recall_context.log")
LEDGER = os.path.join(HOOKS, "usage_ledger.json")
RECEIPT = os.path.join(HOOKS, "usage_ledger.log")
from .paths import SRC, STATE, PYTHON  # noqa: E402  (OS-neutral, 2026-09-18)


def receipt(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with open(RECEIPT, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def rows_of(session=None):
    out = []
    for raw in io.open(RECALL_LOG, encoding="utf-8", errors="replace"):
        try:
            r = json.loads(raw)
        except ValueError:
            continue
        if session and r.get("session") != session:
            continue
        if r.get("injected") or r.get("use") == "read":
            out.append(r)
    return out


def last_turn(session):
    """(injected, opened, sources_not_opened) of the session's most recent injected receipt — what the next
    prompt's packet says out loud (2026-09-21): replay that was skipped is named before the next use."""
    rows = rows_of(session)
    injected = [r for r in rows if r.get("injected")]
    if not injected:
        return 0, 0, []
    last = injected[-1]
    counts = count([last] + [r for r in rows if r.get("use") == "read" and r.get("ts", "") >= last.get("ts", "")])
    opens = last.get("opens") or {}
    # a record whose snippet was its whole text (the hint said 토막이 전문이다) needs no opening: not a miss
    due = {src: c for src, c in counts.items() if not (isinstance(opens.get(src), dict) and opens[src].get("whole"))}
    opened = sum(1 for c in due.values() if c[1])
    missed = [src for src, c in due.items() if not c[1]]
    return len(due), opened, missed


def norm(path):
    return (path or "").replace("\\", "/").casefold()


def count(rows):
    """{source: [injected, opened]} — 각 주입은 그 뒤 같은 세션의 Read 로 한 번만 「열림」이 된다."""
    counts = {}
    reads = [r for r in rows if r.get("use") == "read"]
    for r in rows:
        if not r.get("injected"):
            continue
        opens = r.get("opens") or {}
        later = [x for x in reads if x.get("session") == r.get("session") and x.get("ts", "") >= r.get("ts", "")]
        for source in r["injected"]:
            c = counts.setdefault(source, [0, 0])
            c[0] += 1
            where = opens.get(source)
            hit = False
            for x in later:
                if where:
                    if norm(x.get("path")) != norm(where["path"]):
                        continue
                    lo, hi = int(where["offset"]), int(where["offset"]) + int(where["limit"])
                    ro = x.get("offset")
                    if ro is None:                      # whole-file Read
                        hit = True
                    else:
                        rl = int(x.get("limit") or 2000)
                        hit = int(ro) < hi and int(ro) + rl > lo
                else:                                   # old receipt: doc-level, never the session log
                    name = source.rsplit("/", 1)[-1].split("#", 1)[0]
                    hit = name != "session-log.md" and x.get("what") == name
                if hit:
                    c[1] += 1
                    break
    return counts


def push(delta, dry):
    if not delta:
        return dict(status="nothing")
    if dry:
        return dict(status="dry", sources=len(delta))
    sys.path.insert(0, SRC)
    from swegca_vrs2.loopback import ensure_daemon
    client = ensure_daemon(STATE, allow_ingest=True, python=PYTHON)
    try:
        return client.request("usage", counts=delta)
    finally:
        client.close()


def flush_session(session_id):
    """Adapter entry (2026-09-18): reconcile one session's injected/opened counts and push the delta."""
    return main(["--session", str(session_id)[:8]])


def main(argv):
    dry = "--dry-run" in argv
    session = None
    if "--session" in argv:
        session = argv[argv.index("--session") + 1][:8] or None
    elif "--all" not in argv:
        try:
            data = json.loads(sys.stdin.read() or "{}")
        except ValueError:
            data = {}
        session = str(data.get("session_id") or "")[:8] or None
    if "--all" not in argv and not session:
        receipt(skip="no_session")
        return
    try:
        ledger = json.load(io.open(LEDGER, encoding="utf-8")) if os.path.exists(LEDGER) else {}
    except ValueError:
        ledger = {}
    parts = ledger.setdefault("_sessions", {})
    if session is not None:
        # a session's counts replace that session's earlier contribution: parts are kept per session
        parts[session] = count(rows_of(session))
    else:
        # full backfill: one part per session (so a later per-session Stop run replaces, never double-counts)
        by_session = {}
        for r in rows_of(None):
            by_session.setdefault(r.get("session") or "?", []).append(r)
        ledger["_sessions"] = parts = {sid: count(rows) for sid, rows in by_session.items()}
    total = {}
    for part in ledger["_sessions"].values():
        for source, (inj, opened) in part.items():
            t = total.setdefault(source, [0, 0])
            t[0] += inj; t[1] += opened
    previous = ledger.get("_total", {})
    delta = {s: v for s, v in total.items() if previous.get(s) != v}
    ledger["_total"] = total
    if not dry:
        io.open(LEDGER, "w", encoding="utf-8").write(json.dumps(ledger, ensure_ascii=False, indent=0))
    try:
        result = push(delta, dry)
    except Exception as failure:
        result = dict(status="error", error=repr(failure)[:200])
    opened_sources = sum(1 for v in total.values() if v[1] > 0)
    receipt(session=session or "all", sources=len(total), opened_sources=opened_sources, delta=len(delta), push=result)
    if session is not None and delta and not dry:
        # Stop-hook receipt the user sees (2026-09-18, 개선 ③): this session's injected / opened so far,
        # and how many injected records were never opened — the habit measured by receipt-use.py, live
        part = parts.get(session) or {}
        inj = sum(v[0] for v in part.values()); opened = sum(v[1] for v in part.values())
        unopened = sum(1 for v in part.values() if v[1] == 0)
        print(json.dumps({"systemMessage": f"[기억 사용] 이 세션 영수증 주입 {inj}·열림 {opened} · 한 번도 안 연 기록 {unopened}종"},
                         ensure_ascii=False))
    if dry or "--all" in argv:
        sys.stdout.reconfigure(encoding="utf-8")
        print(f"sources {len(total)} | opened>0 {opened_sources} | delta {len(delta)} | push {result}")
        for s, v in sorted(total.items(), key=lambda kv: -kv[1][1])[:8]:
            print(f"  {v[0]:3d}/{v[1]:2d}  {s[:90]}")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Exception as failure:
        receipt(error=repr(failure)[:300])
