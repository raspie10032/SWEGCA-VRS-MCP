#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PostToolUse 훅: 모델이 기억을 직접 찾은 순간을 회수 훅 로그에 적는다.

회수 훅(`recall_context.py`)이 프롬프트마다 냈다/안 냈다를 적지만, 「안 냈는데 필요했다」
는 사람이 눈으로 세어야 했다. 훅이 침묵한 직후 모델이 `recall`·`get_episode` 를 부르거나
`memory/*.md` 를 Read 했으면 그것이 곧 그 신호다 — 여기서 같은 로그에 `use` 행으로 남기고
`recall_report.py` 가 프롬프트 행과 `session` 으로 이어 센다.

    matcher: Read|mcp__swegca-vrs__recall|mcp__swegca-vrs__get_episode
    stdin  : {"session_id", "tool_name", "tool_input", ...}
    stdout : 아무것도 안 낸다

Read 는 `~/.claude/projects/<프로젝트>/memory/*.md` 만 센다 — 그 밖의 Read 는 기억이
아니다. 한계: Bash 의 cat/grep 으로 읽는 것은 못 본다(도구 입력이 명령 문자열뿐이라
읽기인지 쓰기인지 못 가른다). 로그를 적으려고 세션 로그를 여는 것도 `use` 로 잡히니
집계는 경로를 같이 보여 사람이 가려낸다.
"""
import json
import os
import sys
import time

LOG = os.path.join(os.path.expanduser("~"), ".claude", "hooks", "recall_context.log")
TOOLS = {"mcp__swegca-vrs__recall": "recall", "mcp__swegca-vrs__get_episode": "episode",
         "mcp__swegca-vrs2__memory_context": "recall", "mcp__swegca-vrs2__memory_recall": "recall",
         "mcp__swegca-vrs2__memory_read_path": "episode", "Read": "read"}


def note(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with open(LOG, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def memory_doc(path):
    """auto-memory 의 문서인가. 경로 구분자는 둘 다 온다."""
    flat = str(path).replace(os.sep, "/").replace(chr(92), "/")
    return "/.claude/projects/" in flat and "/memory/" in flat and flat.endswith(".md")


def note_read(path, offset, limit, session_id):
    """Adapter entry (2026-09-18): record a memory-file read (what the PostToolUse hook does for Read)."""
    if not memory_doc(path):
        return False
    note(use="read", what=os.path.basename(str(path)), path=str(path).replace(chr(92), "/"), offset=offset,
         limit=limit, session=str(session_id or "")[:8])
    return True


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return
    kind = TOOLS.get(str(data.get("tool_name") or ""))
    if not kind:
        return
    args = data.get("tool_input") or {}
    session = str(data.get("session_id") or "")[:8]
    if kind == "read":
        path = str(args.get("file_path") or "")
        if not memory_doc(path):
            return
        # offset/limit make a Read matchable to the recalled record it opens (usage re-evidence, 2026-09-18)
        note(use=kind, what=os.path.basename(path), path=path.replace(chr(92), "/"), offset=args.get("offset"),
             limit=args.get("limit"), session=session)
    elif kind == "recall":
        note(use=kind, what=str(args.get("query") or "")[:120], session=session)
    else:
        note(use=kind, what=str(args.get("episode_id") or args.get("event_id") or "")[:120],
             session=session)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
