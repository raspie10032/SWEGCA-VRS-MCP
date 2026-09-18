#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""recall_context.log 를 세어 훅이 실제로 무엇을 했는지 보여 준다.

    <venv python> recall_report.py            # 전체
    <venv python> recall_report.py --since 2026-09-12
    <venv python> recall_report.py --live      # 실제 프롬프트만 (되돌리기 제외)
    <venv python> recall_report.py --replay    # 되돌리기만

「도움이 되는지」의 절반은 여기서 나온다 — 얼마나 자주, 무엇을, 얼마에 냈는가.
나머지 절반 가운데 **「안 냈는데 필요했나」는 2026-09-11부터 자동으로 센다**: 같은
로그에 `memory_use_log.py`(PostToolUse) 가 recall·get_episode·메모리 문서 Read 를 `use`
행으로 남기므로, 프롬프트 행마다 그 뒤 같은 세션의 다음 프롬프트 전까지 온 `use` 를
붙인다. 훅이 침묵했는데 `use` 가 붙었으면 「안 냈는데 찾음」, 냈는데 붙었으면 「냈는데
더 찾음」. 「냈는데 무시됐나」는 여전히 사람이 본다 — 안 쓴 것은 흔적이 없다.
훅 로그는 프롬프트 원문을 남기지 않는다 — 낱말만 남긴다.
"""
import json
import os
import sys
from collections import Counter

LOG = os.path.join(os.path.expanduser("~"), ".claude", "hooks", "recall_context.log")


def load(since, want):
    rows = []
    if os.path.isfile(LOG):
        with open(LOG, encoding="utf-8") as source:
            for line in source:
                try:
                    row = json.loads(line)
                except ValueError:
                    continue
                if row.get("ts", "") < since:
                    continue
                if want == "replay" and not row.get("replay"):
                    continue
                if want == "live" and row.get("replay"):
                    continue
                rows.append(row)
    return rows


def attach_uses(rows):
    """프롬프트 행마다 `uses` 를 단다 — 같은 세션에서 다음 프롬프트가 오기 전의 use 행.

    session 이 없는 옛 행(2026-09-11 오후 이전)은 세션 구분 없이 시간순으로 잇는다.
    """
    prompts, open_by_session = [], {}
    for row in rows:
        session = row.get("session", "")
        if "use" in row:
            holder = open_by_session.get(session) or open_by_session.get("")
            if holder is not None:
                holder.setdefault("uses", []).append(row)
            continue
        row["uses"] = []
        prompts.append(row)
        open_by_session[session] = row
    return prompts


def kind_of(r):
    return ("오류" if "error" in r else "냄" if "injected" in r
            else {"short": "짧음", "weak": "안냄"}.get(r.get("skip"), "?"))


def main():
    since = ""
    if "--since" in sys.argv:
        since = sys.argv[sys.argv.index("--since") + 1]
    want = "replay" if "--replay" in sys.argv else "live" if "--live" in sys.argv else "all"
    rows = load(since, want)
    prompts = attach_uses(rows)
    if not prompts:
        print("로그 없음")
        return
    kinds = Counter(kind_of(r) for r in prompts)
    total = len(prompts)
    print(f"프롬프트 {total}건  ({prompts[0]['ts']} ~ {prompts[-1]['ts']})")
    for kind in ("냄", "안냄", "짧음", "오류"):
        if kinds.get(kind):
            print(f"  {kind:<4} {kinds[kind]:4d}  {kinds[kind] / total * 100:5.1f}%")
    sent = [r for r in prompts if "injected" in r]
    if sent:
        ms = sorted(r.get("ms", 0) for r in sent)
        print(f"  낸 것의 비용: 중앙 {ms[len(ms) // 2]} ms · 최악 {ms[-1]} ms · "
              f"글자 중앙 {sorted(r.get('chars', 0) for r in sent)[len(sent) // 2]}")
        what = Counter()
        for r in sent:
            for eid in r["injected"]:
                what["판정" if eid.startswith("obs:") else
                     "세션로그" if "session-log" in eid else "메모리문서"] += 1
        print(f"  낸 종류: {dict(what)}")

    # 자동 집계 — 프롬프트 뒤에 모델이 직접 기억을 찾았나
    used = [r for r in prompts if r["uses"]]
    missed = [r for r in used if "injected" not in r]
    extra = [r for r in used if "injected" in r]
    tracked = sum(1 for r in rows if "use" in r)
    print(f"\n=== 기억을 직접 찾은 프롬프트 (use 행 {tracked}건) ===")
    print(f"  안 냈는데 찾음 {len(missed)}건 · 냈는데 더 찾음 {len(extra)}건 · "
          f"냄 후 안 찾음 {len(sent) - len(extra)}건")
    for label, group in (("안 냈는데 찾음", missed), ("냈는데 더 찾음", extra)):
        for r in reversed(group[-20:]):
            words = " ".join(r.get("words", [])[:8])
            print(f"  [{label}] {r['ts'][5:16]}  [{words[:44]}]")
            for u in r["uses"][:6]:
                print(f"      {u['use']:<7} {u.get('what', '')[:70]}")

    if sent:
        print("\n=== 낸 것 (최근 것부터, 최대 40) ===")
        for r in reversed(sent[-40:]):
            words = " ".join(r.get("words", [])[:8])
            print(f"  {r['ts'][5:16]}  [{words[:44]}]")
            for eid in r["injected"]:
                print(f"      -> {eid[:84]}")
    errors = [r for r in prompts if "error" in r]
    if errors:
        print("\n=== 오류 ===")
        for r in errors[-5:]:
            print(f"  {r['ts']}  {r['error'][:120]}")
    print("\n판단할 것: 「안 냈는데 찾음」은 찾은 것이 기억 문서면 놓침, 로그를 적으려 세션 로그를 "
          "연 것이면 아님. 「냈는데 무시한 것」은 여전히 눈으로. 문턱은 recall_context.py 머리의 상수.")


if __name__ == "__main__":
    main()
