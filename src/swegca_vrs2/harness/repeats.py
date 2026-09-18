#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""반복 계수기 — 판정으로 결판난 실수를 또 하면 그것이 관측이 된다 (2026-09-18, 관문 ②).

두 입구:
  * 관문(가드)이 막을 때 `note_repeat(slug, session, detail)` → `repeat_ledger.log` 한 줄 (가드는 스토어를 안 부른다).
  * 사람이 눈으로 본 반복: `python repeat_ledger.py --slug <판정> --evidence <path:line> [--text …]` → 즉시 관측.
Stop 훅(`--flush`): 이 세션의 가드 행을 판정(slug)별로 합쳐 **세션당 관측 하나**(프로듀서 `gate`, 출처 `gate:<slug>#<세션>`,
극성 = 그 판정의 극성 그대로, metadata repeat=횟수, confidence 1 — 기계가 셌다)로 넣는다. 같은 (slug, 세션)은 한 번만
넣는다(`repeat_flushed.json`). 훅은 판정 줄에 「⚠ 반복 n회·세션 m개」를 붙이고 세션 3개부터 「관문 후보」라 적는다.
"""
import importlib.util
import io
import json
import os
import sqlite3
import sys
import time

from .paths import RECEIPTS as HOOKS
LEDGER = os.path.join(HOOKS, "repeat_ledger.log")
FLUSHED = os.path.join(HOOKS, "repeat_flushed.json")
RECEIPT = os.path.join(HOOKS, "repeat_ledger.receipts.log")
V02_DB = r"C:/Users/asm/mcp/swegca-memory/core.sqlite3"
PRODUCE = r"C:/Users/asm/mcp/vrs2-produce.py"


def note_repeat(slug, session, detail=""):
    """가드가 부른다: 스토어 접속 없이 한 줄만 남긴다."""
    try:
        with open(LEDGER, "a", encoding="utf-8") as out:
            out.write(json.dumps(dict(slug=slug, session=str(session)[:8], detail=str(detail)[:160],
                                      ts=time.strftime("%Y-%m-%d %H:%M:%S")), ensure_ascii=False) + "\n")
    except OSError:
        pass


def receipt(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with open(RECEIPT, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def verdict_of(slug):
    """살아 있는 판정의 (명제, outcome)."""
    db = sqlite3.connect(V02_DB)
    rows = db.execute(
        "SELECT o.body FROM observations o JOIN record_meta m ON m.event_id = o.event_id "
        "WHERE o.event_id LIKE ? AND m.superseded = 0 ORDER BY o.seq DESC LIMIT 1", (f"obs:{slug}%",)).fetchall()
    db.close()
    if not rows:
        return None
    ev = json.loads(rows[0][0])["event"]
    return ev["hypothesis_id"], ev["outcome"]


def produce_repeat(slug, session, count, evidence, text, dry):
    found = verdict_of(slug)
    if found is None:
        return dict(status="no_verdict", slug=slug)
    hypothesis, outcome = found
    if dry:
        return dict(status="dry", slug=slug, hypothesis=hypothesis[:60], outcome=outcome, count=count)
    spec = importlib.util.spec_from_file_location("vrs2_produce", PRODUCE)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    body = f"반복 {count}회 — 판정 {slug} 의 실수를 세션 {session} 이 다시 했다(관문이 막았거나 사람이 봤다).\n{text}".strip()
    # produce() 는 metadata 를 고정 키로 짜므로 repeat 횟수는 본문·출처에 싣고 confidence 1 로 낸다
    return mod.produce(producer="gate", hypothesis=hypothesis, outcome=outcome, axes=["observational"],
                       context=f"session-{session}", source=f"gate:{slug}#{session}", text=body,
                       evidence=list(evidence), confidence=1.0)


def flush(session, dry=False):
    try:
        flushed = json.load(io.open(FLUSHED, encoding="utf-8")) if os.path.exists(FLUSHED) else {}
    except ValueError:
        flushed = {}
    counts = {}
    if os.path.exists(LEDGER):
        for raw in io.open(LEDGER, encoding="utf-8", errors="replace"):
            try:
                r = json.loads(raw)
            except ValueError:
                continue
            if r.get("session") == session:
                counts[r["slug"]] = counts.get(r["slug"], 0) + 1
    done = []
    for slug, n in counts.items():
        key = f"{slug}#{session}"
        if key in flushed:
            continue
        result = produce_repeat(slug, session, n, [LEDGER], f"가드 로그 {LEDGER}", dry)
        if not dry and result.get("status") == "observation_recorded":
            flushed[key] = dict(count=n, ts=time.strftime("%Y-%m-%d %H:%M:%S"))
        done.append((slug, n, result.get("status")))
    if not dry:
        io.open(FLUSHED, "w", encoding="utf-8").write(json.dumps(flushed, ensure_ascii=False, indent=0))
    receipt(session=session, flushed=done)
    return done


def main(argv):
    if "--flush" in argv:
        try:
            data = json.loads(sys.stdin.read() or "{}")
        except ValueError:
            data = {}
        session = str(data.get("session_id") or "")[:8]
        if session:
            flush(session, dry="--dry-run" in argv)
        return
    if "--slug" in argv:
        slug = argv[argv.index("--slug") + 1]
        evidence = [argv[argv.index("--evidence") + 1]] if "--evidence" in argv else []
        text = argv[argv.index("--text") + 1] if "--text" in argv else ""
        session = (argv[argv.index("--session") + 1] if "--session" in argv else "manual")[:8]
        sys.stdout.reconfigure(encoding="utf-8")
        print(produce_repeat(slug, session, 1, evidence, text, "--dry-run" in argv))
        return
    print(__doc__)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Exception as failure:
        receipt(error=repr(failure)[:300])
