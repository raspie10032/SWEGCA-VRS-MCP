#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PreToolUse(Write|Edit|Bash|PowerShell) 가드: 이 세션에 영수증으로 주입됐는데 한 번도 안 연 memory 문서를 고치려 하면 막는다
(2026-09-18, 관문 ④).

판정 `receipt-path-line-is-rarely-opened-before-use`: path:line 만 있으면 긴 세션은 주입된 문서를 1/9·1/21 만 열었다. 열기 줄이
붙은 뒤에도 습관은 측정 중이다. 가장 좁고 확실한 관문은 「그 문서를 **고치기** 전엔 반드시 연다」 — 안 읽은 문서를 고치면
중복 절·모순이 생긴다. 조건: 이 세션의 회수 영수증(`recall_context.log`)에 그 문서를 여는 `opens` 가 있고, 그 뒤 같은 세션의
Read(`use: read`, 같은 파일)가 없다. session-log.md 는 예외(로그는 append 만 한다). Bash/PowerShell 은 `>`·`>>` 로 memory 문서에
쓰는 명령만 본다. 통과 표식 `# unopened-ok`. 가드는 어떤 예외도 통과로 처리한다.
"""
import io
import json
import os
import re
import sys

from .paths import RECEIPTS as HOOKS
RECALL_LOG = os.environ.get("UNOPENED_GUARD_LOG") or os.path.join(HOOKS, "recall_context.log")   # env: 시험용 로그
MARK = "# unopened-ok"
MEMORY_DOC = re.compile(r"(?:[A-Za-z]:|~)?[\\/](?:[^\s\"'<>|]+[\\/])*\.claude[\\/]projects[\\/][^\s\"'<>|]+[\\/]memory[\\/][^\s\"'<>|/\\]+\.md")   # Windows or POSIX absolute path


def norm(path):
    return (path or "").replace("\\", "/").casefold()


def targets(data):
    tool = data.get("tool_name")
    inp = data.get("tool_input") or {}
    if tool in ("Write", "Edit", "NotebookEdit"):
        p = str(inp.get("file_path") or "")
        return [p] if MEMORY_DOC.fullmatch(p.replace("/", "\\")) or MEMORY_DOC.fullmatch(p) else []
    if tool in ("Bash", "PowerShell"):
        cmd = str(inp.get("command") or "")
        if MARK in cmd or (">" not in cmd):
            return []
        return [m.group(0) for m in MEMORY_DOC.finditer(cmd)]
    return []


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return
    reason = decide(data.get("tool_name"), data.get("tool_input") or {}, data.get("session_id"))
    if reason:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                                 "permissionDecisionReason": reason}}, ensure_ascii=False))


def decide(tool_name, tool_input, session_id=None):
    """Adapter entry (2026-09-18): the deny reason for this action, or None."""
    data = {"tool_name": tool_name, "tool_input": tool_input or {}, "session_id": session_id}
    docs = [d for d in targets(data) if not norm(d).endswith("/session-log.md")]
    if not docs:
        return None
    session = str(data.get("session_id") or "")[:8]
    if not session or not os.path.isfile(RECALL_LOG):
        return None
    injected = {}          # norm path -> (ts, open dict) of the latest injection in this session
    reads = []             # (ts, norm path)
    for raw in io.open(RECALL_LOG, encoding="utf-8", errors="replace"):
        try:
            r = json.loads(raw)
        except ValueError:
            continue
        if r.get("session") != session:
            continue
        if r.get("injected"):
            for src, where in (r.get("opens") or {}).items():
                injected[norm(where.get("path"))] = (r.get("ts", ""), where)
        elif r.get("use") == "read" and r.get("path"):
            reads.append((r.get("ts", ""), norm(r["path"])))
    blocked = []
    for doc in docs:
        key = norm(doc)
        hit = injected.get(key)
        if not hit:
            continue
        ts, where = hit
        if any(p == key and rts >= ts for rts, p in reads):
            continue
        blocked.append((doc, where))
    if not blocked:
        return None
    calls = "; ".join(f'Read file_path="{w["path"]}" offset={w["offset"]} limit={w["limit"]}' for _, w in blocked)
    reason = ("이 세션에 영수증으로 주입됐는데 한 번도 안 연 memory 문서를 고치려 한다: "
              + ", ".join(os.path.basename(d) for d, _ in blocked)
              + f". 먼저 열고 고친다 — {calls}. (판정 receipt-path-line-is-rarely-opened-before-use; 셸이면 `{MARK}` 로 통과)")
    try:
        from .repeats import note_repeat
        note_repeat("receipt-path-line-is-rarely-opened-before-use", session, ", ".join(os.path.basename(d) for d, _ in blocked))
    except Exception:
        pass
    return reason


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
