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
import io
import json
import os
import re
import sys
import time

LOG = os.path.join(__import__("swegca_vrs2.harness.paths", fromlist=["RECEIPTS"]).RECEIPTS, "recall_context.log")
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


_READ_VERBS = re.compile(r"(?:^|[;&|(\s])(sed|cat|head|tail|grep|less|more|awk|Get-Content|gc|type|Select-String|python[0-9.]*\s+-c)\b", re.I)
_SED_RANGE = re.compile(r"sed\s+-n\s+['\"]?(\d+),(\d+)p")
_HEAD_N = re.compile(r"head\s+-n?\s*(\d+)|head\s+-c\s+\d+")
_PATHS = re.compile(r"(?:[A-Za-z]:)?[/\\][^\s'\"`;|&<>()]+?\.(?:md|jsonl|json|txt|py|csv|log)\b")


def offered_paths(session, limit=400):
    """Paths the recall hook offered to open in this session's recent receipts (``opens``): a memory doc, a
    transcript's 「원문 위치」, anything a 「열기」 line named. Read from the tail of the same log."""
    out = set()
    try:
        lines = io.open(LOG, encoding="utf-8", errors="replace").read().splitlines()[-limit:]
    except OSError:
        return out
    for raw in lines:
        try:
            r = json.loads(raw)
        except ValueError:
            continue
        if r.get("session") != session or not r.get("opens"):
            continue
        for where in r["opens"].values():
            if isinstance(where, dict) and where.get("path"):
                out.add(where["path"].replace(chr(92), "/").casefold())
    return out


def reads_in_command(command, known):
    """(path, offset, limit) for each known or memory-doc path a shell command reads. A ``sed -n A,Bp`` gives the
    lines; ``head -N`` gives 1..N; anything else is a whole-file read (offset None). Writes (>, tee, Set-Content)
    to the same path are not reads. Measured 2026-09-21: in bypass mode the main reads with sed/cat, and the
    Read-only matcher counted none of it as replay."""
    command = str(command or "")
    if not _READ_VERBS.search(command):
        return []
    # M=/path; sed -n 1,60p "$M/doc.md" — the shape the main actually uses: substitute simple assignments first
    for var, value in re.findall(r"(?:^|[;\s])([A-Za-z_][A-Za-z0-9_]*)=[\"']?([^\s;\"']+)", command):
        command = re.sub(r"\$\{?" + re.escape(var) + r"\}?", value.replace(chr(92), chr(92) * 2), command)
    out, seen = [], set()
    for m in _PATHS.finditer(command):
        raw = m.group(0)
        flat = raw.replace(chr(92), "/")
        key = flat.casefold()
        if key in seen:
            continue
        if not (memory_doc(flat) or key in known):
            continue
        after = command[m.end():m.end() + 40]
        before = command[max(0, m.start() - 40):m.start()]
        if re.search(r"^\s*(>|>>|\|\s*tee)", after) or re.search(r"(>|>>|tee|Set-Content|Out-File)\s*$", before):
            continue                                 # the path is written, not read
        seen.add(key)
        rng = _SED_RANGE.search(before + raw + after) or _SED_RANGE.search(command)
        head = _HEAD_N.search(command)
        if rng:
            offset, limit = int(rng.group(1)), max(1, int(rng.group(2)) - int(rng.group(1)) + 1)
        elif head and head.group(1):
            offset, limit = 1, int(head.group(1))
        else:
            offset, limit = None, None
        out.append((flat, offset, limit))
    return out


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
    tool = str(data.get("tool_name") or "")
    kind = TOOLS.get(tool)
    args = data.get("tool_input") or {}
    session = str(data.get("session_id") or "")[:8]
    if tool in ("Bash", "PowerShell"):
        # a shell read of an offered path is a replay too (2026-09-21) — via the same `use: read` line
        known = offered_paths(session)
        for path, offset, limit in reads_in_command(args.get("command"), known):
            note(use="read", what=os.path.basename(path), path=path, offset=offset, limit=limit, session=session, via=tool.lower())
        return
    if not kind:
        return
    if kind == "read":
        path = str(args.get("file_path") or "")
        if not memory_doc(path) and path.replace(chr(92), "/").casefold() not in offered_paths(session):
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
