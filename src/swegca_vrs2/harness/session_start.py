#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SessionStart 훅: 직전 회기의 마지막 항목 몇 개를 컨텍스트에 얹는다.

새 세션은 MEMORY.md 색인만 들고 시작한다. 세션 로그(무엇을 했고 무엇이 남았나)는
읽어 달라고 부탁해야 읽히는데, 그 부탁이 늘 있지는 않다. **압축 뒤에도 이 훅이 뜬다**
(`source == "compact"`) — 그때는 더 많이 얹는다. 요약이 놓친 것을 메우는 보험이다.

압축 전용 훅(`compact_log.py`, PreCompact/PostCompact)은 2026-09-11 에 물렸다: 하니스가
`hookSpecificOutput.hookEventName` 에 그 두 이벤트를 받지 않아 출력이 통째로 거부됐다
(2.1.260 바이너리의 허용 목록 22개에 둘 다 없음 — 구조적이다). 손으로 돌려 JSON 이
나오는 것만 보고 「된다」고 적었던 것이 틀렸다 — 채널은 압축을 실제로 해 봐야 안다.
그래서 검증된 채널(이 훅) 하나로 합쳤다. auto·manual 압축 모두 같은 경로로 이 훅을
부른다(바이너리의 `Sj("compact")` 세 곳).

    stdin : 훅 입력 JSON (`cwd`, `source`: startup|resume|clear|compact|fork)
    stdout: hookSpecificOutput.additionalContext JSON, 로그가 없으면 아무것도 안 낸다
    영수증: `session_start.log` 에 한 줄(source·항목 수·글자 수). 압축 뒤 실제로 무엇이
            갔는지는 여기서 본다 — 「손으로 돌려 JSON 이 나온다」는 증거가 아니었다.
"""
import json
import os
import re
import sys
import time

ENTRIES = 3            # 보통 시작: 마지막 몇 항목
MAX_CHARS = 3500       # 그래도 이보다 길면 앞 항목부터 뺀다 — 마지막이 가장 값지다
COMPACT_ENTRIES = 20   # 압축 뒤: 옛 compact_log.py 의 60줄(약 9,300자)과 비슷한 양을 — 실제 제한은 글자 예산
COMPACT_CHARS = 9000   # 줄이 아니라 항목 단위로 — 항목 중간에서 시작하지 않는다
RECEIPT = os.path.join(__import__("swegca_vrs2.harness.paths", fromlist=["RECEIPTS"]).RECEIPTS, "session_start.log")


def receipt(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with open(RECEIPT, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def tail_entries(lines, entries, max_chars):
    """뒤에서부터 항목을 모은다. 마지막 항목은 무조건, 나머지는 개수·글자 예산 안에서."""
    starts = [i for i, line in enumerate(lines) if re.match(r"- 20\d\d-", line)]
    if not starts:
        return "", 0
    blocks, spent = [], 0
    for k in range(len(starts) - 1, -1, -1):
        end = starts[k + 1] if k + 1 < len(starts) else len(lines)
        block = "\n".join(lines[starts[k]:end])
        if blocks and (len(blocks) >= entries or spent + len(block) > max_chars):
            break
        blocks.append(block)
        spent += len(block) + 1
    blocks.reverse()
    tail = "\n".join(blocks)
    if len(tail) > max_chars:      # 마지막 항목 하나가 예산보다 큰 경우
        tail = "…" + tail[-max_chars:]
    return tail, len(blocks)


def context_for(cwd, source, session_id=None):
    """Adapter entry (2026-09-18): the session-log tail as text for startup/resume/compact, or None."""
    cwd = str(cwd or os.getcwd())
    source = str(source or "")
    compact = source == "compact"
    from .project_dir import resolve          # 하위 폴더 cwd 면 로그가 있는 조상 프로젝트로(2026-09-17)
    slug, memory_dir = resolve(cwd)
    log = os.path.join(memory_dir, "session-log.md")
    if not os.path.isfile(log):
        receipt(source=source, log="missing", slug=slug)
        return None
    with open(log, encoding="utf-8") as handle:
        lines = handle.read().splitlines()
    if compact:
        tail, count = tail_entries(lines, COMPACT_ENTRIES, COMPACT_CHARS)
        head = ("[기억] 컨텍스트가 압축됐다. 아래는 이 프로젝트 세션 로그의 마지막 항목들이다. "
                "요약이 놓친 것은 여기서 메우고, 「다음」이 적혀 있으면 그것이 이어서 할 일이다. "
                "사용자가 다른 것을 시키면 그쪽이 먼저다.\n")
    else:
        tail, count = tail_entries(lines, ENTRIES, MAX_CHARS)
        head = ("[기억] 이 프로젝트 세션 로그의 마지막 항목이다. 「다음」이 적혀 있으면 "
                "그것이 이어서 할 일이다. 사용자가 다른 것을 시키면 그쪽이 먼저다.\n")
    if not tail and not compact:
        receipt(source=source, log="no_entries")
        return None
    context = head + (f"=== {os.path.basename(log)} 마지막 {count}항목 ===\n" + tail if tail else "")
    turns_block, turns_count = ("", 0)
    if compact:
        # 2026-09-21 (대전제): the turns the compaction cut, as the STORE holds them — the experience with its
        # place in the log — not the log re-read, not only what the main chose to write about them
        turns_block, turns_count = cut_turns(session_id)
        if turns_block:
            context = context.rstrip("\n") + "\n" + turns_block
    receipt(source=source, entries=count, turns=turns_count, chars=len(context), session=str(session_id or "")[:8])
    return context


TURNS = 3            # cut turns injected after a compaction
TURN_CHARS = 1400    # per turn; the row keeps up to 6,000, the 「원문 위치」 line opens the rest


def cut_turns(session_id):
    """(text block, count): the last turns of this session from the store, each with its exact place in the log.
    Never raises; a daemon that is not up is a named miss in the receipt, not a blocked start."""
    if not session_id:
        return "", 0
    try:
        from .paths import STATE, PYTHON, BUNDLE_LIMIT, BUNDLES, HOT_BUNDLES
        from swegca_vrs2.loopback import ensure_daemon
        client = ensure_daemon(STATE, allow_ingest=False, python=PYTHON, wait_seconds=6, bundle_limit=BUNDLE_LIMIT, bundles=BUNDLES, hot_bundles=HOT_BUNDLES)
        try:
            page = client.request("turns", session=str(session_id), limit=TURNS, snippet=TURN_CHARS)
        finally:
            client.close()
    except Exception as error:
        receipt(source="compact", turns_miss=repr(error)[:120])
        return "", 0
    rows = page.get("rows") or []
    if not rows:
        return "", 0
    lines = [f"=== 압축 직전 대화 — 스토어의 마지막 {len(rows)}턴 (세션 {str(session_id)[:8]}; 「원문 위치」로 그 자리를 연다) ==="]
    for r in rows:
        part = {"partial": " [압축 전 미완 — 여기서 잘렸다]", "tail": " [이어짐]"}.get(r.get("part"), "")
        snap = r.get("snapshot") or {}
        if snap.get("line"):
            part += f" · 압축 스냅샷 session-log.md {snap['line']}행"       # item 16: the two records of one compaction
        lines.append(f"- 턴 {r.get('turn')}{part}")
        lines.append("  " + str(r.get("text") or "")[:TURN_CHARS].replace("\n", "\n  "))
        span = r.get("lines") or []
        if r.get("path") and len(span) == 2:
            lines.append(f'  원문 위치: Read file_path="{r["path"]}" offset={span[0]} limit={max(1, min(60, int(span[1]) - int(span[0]) + 1))}'
                         + (f"  (전문 {r.get('text_chars')}자: swegca-vrs2 memory_read {r.get('episode_id')})" if int(r.get("text_chars") or 0) > TURN_CHARS else ""))
    return "\n".join(lines) + "\n", len(rows)


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        data = {}
    context = context_for(data.get("cwd") or os.getcwd(), data.get("source"), data.get("session_id"))
    if not context:
        return
    out = {"hookSpecificOutput": {"hookEventName": "SessionStart",
                                  "additionalContext": context}}
    sys.stdout.buffer.write(json.dumps(out, ensure_ascii=False).encode("utf-8"))


if __name__ == "__main__":
    try:
        main()
    except Exception as failure:  # 세션 시작을 막지 않는다
        receipt(error=repr(failure)[:200])
