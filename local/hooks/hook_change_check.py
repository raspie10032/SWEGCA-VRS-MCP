#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stop 훅: 훅 스크립트나 settings.json 을 바꿨는데 그 뒤 그 훅의 영수증이 0건이면 경고한다 (2026-09-18, 관문 ③).

09-11 판정 `compact-hook-output-rejected-by-harness-schema`: 손으로 돌려 JSON 이 나오는 걸 보고 「정상」이라 적었지만
하니스는 그 출력을 거부하고 있었다. 스크립트가 도는 것과 하니스가 그 출력을 받는 것은 다른 검사고, 후자는 그 이벤트를
실제로 일으켜야만 보인다. 그래서: 훅 파일의 mtime 이 스냅숏보다 새로운데 그 훅의 영수증 로그에 mtime 뒤 줄이 없으면
systemMessage 로 알린다. 영수증 로그가 없는 가드(막을 때만 출력)는 검사에서 뺀다. 상태 `hook_snapshot.json`.
"""
import io
import json
import os
import re
import sys
import time

HOOKS = os.path.dirname(os.path.abspath(__file__))
SETTINGS = os.path.join(os.path.expanduser("~"), ".claude", "settings.json")
SNAPSHOT = os.path.join(HOOKS, "hook_snapshot.json")
# 훅 스크립트 → 그 훅이 실제로 돌면 남는 영수증 로그. None = 영수증이 없는 훅(막을 때만 출력하는 가드 등)
RECEIPTS = {
    "recall_context_v2.py": "recall_context.log",
    "session_start.py": "session_start.log",
    "stop_reindex_v2.py": "stop_reindex_v2.log",
    "usage_ledger.py": "usage_ledger.log",
    "precompact_snapshot.py": "precompact_snapshot.log",
    "repeat_ledger.py": "repeat_ledger.receipts.log",
    "memory_use_log.py": "recall_context.log",
    "bash_backslash_guard.py": None,
    "log_label_guard.py": None,
    "hook_change_check.py": None,
    "unopened_edit_guard.py": None,
}
NOTE = {"precompact_snapshot.py": "PreCompact 는 다음 압축 때 돈다 — 그때 영수증 `written` 을 본다"}


def hook_scripts():
    try:
        s = json.load(io.open(SETTINGS, encoding="utf-8"))
    except Exception:
        return {}
    out = {}
    for event, groups in (s.get("hooks") or {}).items():
        for grp in groups:
            for h in grp.get("hooks", []):
                for m in re.finditer(r"hooks[\\/]+([A-Za-z0-9_]+\.py)", h.get("command", "")):
                    out.setdefault(m.group(1), set()).add(event)
    return out


def last_receipt_ts(log_name):
    path = os.path.join(HOOKS, log_name)
    if not os.path.isfile(path):
        return 0.0
    try:
        with open(path, "rb") as handle:
            handle.seek(max(0, os.path.getsize(path) - 4000))
            tail = handle.read().decode("utf-8", errors="replace").strip().split("\n")
        for line in reversed(tail):
            try:
                ts = json.loads(line).get("ts")
                if ts:
                    return time.mktime(time.strptime(ts, "%Y-%m-%d %H:%M:%S"))
            except Exception:
                continue
    except OSError:
        pass
    return os.path.getmtime(path)


def main():
    scripts = hook_scripts()
    try:
        snap = json.load(io.open(SNAPSHOT, encoding="utf-8")) if os.path.exists(SNAPSHOT) else {}
    except ValueError:
        snap = {}
    warnings = []
    for name, events in scripts.items():
        path = os.path.join(HOOKS, name)
        if not os.path.isfile(path):
            continue
        mtime = os.path.getmtime(path)
        log_name = RECEIPTS.get(name, "?")
        if log_name is None:
            snap[name] = dict(mtime=mtime, proven=True)
            continue
        if log_name == "?":
            warnings.append(f"{name}: 영수증 로그 매핑 없음(hook_change_check.RECEIPTS 에 적어라)")
            continue
        seen = last_receipt_ts(log_name)
        proven = seen >= mtime
        prev = snap.get(name) or {}
        snap[name] = dict(mtime=mtime, proven=proven)
        if not proven and prev.get("mtime") != mtime:      # once per change, not every stop
            age = int((time.time() - mtime) / 60)
            warnings.append(f"{name}({'/'.join(sorted(events))}) 를 {age}분 전에 바꿨는데 그 뒤 영수증 0건"
                            + (f" — {NOTE[name]}" if name in NOTE else " — 그 이벤트를 한 번 실제로 일으켜 확인하라(09-11 판정)"))
    try:
        io.open(SNAPSHOT, "w", encoding="utf-8").write(json.dumps(snap, ensure_ascii=False, indent=0))
    except OSError:
        pass
    if warnings:
        print(json.dumps({"systemMessage": "[훅 검증] " + " | ".join(warnings)}, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
