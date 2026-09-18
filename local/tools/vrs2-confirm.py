#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""현재 요청의 증거를 과거 명제에 부딪힌다 (2026-09-17, 재검증 경로 ①).

회수된 판정/명제를 **지금** 확인하거나 반박한 순간, 그것을 관측으로 낸다. 프로듀서 `session-main`,
출처 = 지금 본 증거(파일#줄 → source_family 는 파일), 맥락 = 프로젝트-날짜. 다른 출처의 반대면
accumulator 가 그 명제를 unresolved 로 놓고, 훅이 「재검증 필요」로 보인다. 종결은 판정(supersedes).

    vrs2-venv python vrs2-confirm.py (--slug <판정 slug> | --hypothesis "<명제 그대로>") (--holds | --fails)
                                     --evidence <path[:line]> [--text ...] [--axes observational,intervention] [--confidence .5]

--holds = 그 명제가 지금 성립한다(support) · --fails = 지금 성립하지 않는다(refute). 판정의 outcome 과 무관하게
**명제 기준**이다. 같은 명제에 실제 반대 증거일 때만 쓴다 — 라벨이나 키워드가 다르다고 부딪히지 않는다.
"""
import argparse
import importlib.util
import json
import os
import re
import sqlite3
import sys
import time

V02_DB = r"C:/Users/asm/mcp/swegca-memory/core.sqlite3"
PRODUCE = r"C:/Users/asm/mcp/vrs2-produce.py"
HOOKS = os.path.join(os.path.expanduser("~"), ".claude", "hooks")


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hypothesis_of_slug(slug):
    """v0.2 스토어의 살아 있는 판정에서 명제 문장을 찾는다(slug 접두 일치, 최신 순)."""
    db = sqlite3.connect(V02_DB)
    rows = db.execute(
        "SELECT o.event_id, o.body FROM observations o JOIN record_meta m ON m.event_id = o.event_id "
        "WHERE o.event_id LIKE ? AND m.superseded = 0 ORDER BY o.seq DESC LIMIT 5", (f"obs:{slug}%",)).fetchall()
    db.close()
    if not rows:
        raise SystemExit(f"판정 없음: obs:{slug}* — --hypothesis 로 명제를 그대로 적거나 슬러그를 확인")
    ev = json.loads(rows[0][1])["event"]
    return ev["hypothesis_id"], rows[0][0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    who = ap.add_mutually_exclusive_group(required=True)
    who.add_argument("--slug", help="판정 slug(접두)")
    who.add_argument("--hypothesis", help="명제 문장 그대로(영어 소문자 한 문장)")
    how = ap.add_mutually_exclusive_group(required=True)
    how.add_argument("--holds", action="store_true", help="명제가 지금 성립한다(support)")
    how.add_argument("--fails", action="store_true", help="명제가 지금 성립하지 않는다(refute)")
    ap.add_argument("--evidence", required=True, help="지금 본 증거 path[:line] — 출처가 된다")
    ap.add_argument("--text", default="", help="무엇을 봤나 한두 줄")
    ap.add_argument("--axes", default="observational")
    ap.add_argument("--confidence", type=float, default=0.5, help="세션 자기 보고라 기본 .5")
    ap.add_argument("--context", default=None, help="기본: <프로젝트 슬러그>-<날짜>")
    ap.add_argument("--cwd", default=os.getcwd())
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    if a.slug:
        hypothesis, event_id = hypothesis_of_slug(a.slug)
    else:
        hypothesis, event_id = a.hypothesis.strip(), None
    m = re.match(r"^(.*?)(?::(\d+))?$", a.evidence.strip())
    path, line = m.group(1), m.group(2)
    path = path.replace("\\", "/")
    if not os.path.exists(path):
        raise SystemExit(f"증거 파일이 없다: {path} — 지금 본 것만 출처가 된다")
    source = f"{path}#{line or 'file'}"
    if a.context is None:
        sys.path.insert(0, HOOKS)
        from project_dir import resolve
        slug, _memory = resolve(a.cwd)
        a.context = f"{slug}-{time.strftime('%Y-%m-%d')}"
    outcome = "success" if a.holds else "failure"
    body = (f"세션이 지금 {'확인' if a.holds else '반박'}: {hypothesis}\n증거: {a.evidence}\n"
            + (f"판정: {event_id}\n" if event_id else "") + a.text.strip())
    print(f"{'holds' if a.holds else 'fails'} | {hypothesis[:80]}\n  출처 {source}\n  맥락 {a.context} | 축 {a.axes} | conf {a.confidence}")
    if a.dry_run:
        return
    produce = load(PRODUCE, "vrs2_produce").produce
    r = produce(producer="session-main", hypothesis=hypothesis, outcome=outcome, axes=a.axes.split(","),
                context=a.context, source=source, text=body, evidence=[path], confidence=a.confidence)
    print("  ", r.get("status"), r.get("episode_id", "")[:20])


if __name__ == "__main__":
    main()
