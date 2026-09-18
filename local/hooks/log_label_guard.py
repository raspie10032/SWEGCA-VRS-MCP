#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PreToolUse(Bash|PowerShell) 가드: 세션 로그에 append 하는 명령의 시각 라벨이 지금과 어긋나면 막는다 (2026-09-18).

반복 실수 관문 ①. 세션 로그 항목의 `- YYYY-MM-DD HH:Mx:` 라벨을 기억으로 적어 앞지르거나 뒤처지는 버릇 —
판정·규칙으로는 안 고쳐졌고(하루 30회+ 정정) 백슬래시 가드처럼 행동 순간에 막아야 끊긴다.

규칙: 명령이 `session-log.md` 에 append(`>>`) 하고 라벨을 담고 있으면, 라벨마다 날짜가 오늘이고 시각이 지금과
TOLERANCE 분 안이어야 한다. `HH:Mx` 는 그 10분 구간의 중앙으로 본다. 어긋나면 deny + 지금 맞는 라벨을 알려준다.
sed 로 라벨을 고치는 명령(`>>` 없음)은 검사하지 않는다. 통과 표식 `# label-ok`(옛 일을 뒤늦게 적을 때).
가드는 어떤 예외도 통과로 처리한다.
"""
import json
import re
import sys
import time

MARK = "# label-ok"
TOLERANCE_MIN = 15
LABEL = re.compile(r"-\s+(\d{4})-(\d{2})-(\d{2})\s+(\d{1,2}):(\d)([\dx])\s*:")


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return
    reason = decide(data.get("tool_name"), (data.get("tool_input") or {}).get("command"), data.get("session_id"))
    if reason:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                                 "permissionDecisionReason": reason}}, ensure_ascii=False))


def decide(tool_name, command, session_id=None):
    """Adapter entry (2026-09-18): the deny reason for this command, or None."""
    data = {"session_id": session_id}
    if tool_name not in ("Bash", "PowerShell"):
        return None
    command = str(command or "")
    if "session-log.md" not in command or ">>" not in command or MARK in command:
        return None
    now = time.localtime()
    now_min = now.tm_hour * 60 + now.tm_min
    today = (now.tm_year, now.tm_mon, now.tm_mday)
    bad = []
    for y, mo, d, hh, m1, m2 in LABEL.findall(command):
        label = f"{y}-{mo}-{d} {hh}:{m1}{m2}"
        if (int(y), int(mo), int(d)) != today:
            bad.append((label, "오늘 날짜가 아니다")); continue
        minute = int(m1) * 10 + (5 if m2 == "x" else int(m2))
        delta = int(hh) * 60 + minute - now_min
        if abs(delta) > TOLERANCE_MIN:
            bad.append((label, f"지금과 {delta:+d}분 차이"))
    if not bad:
        return None
    try:                                   # 반복 계수기: 막은 시도 = 판정 session-log-time-labels-drift 의 반복
        import os
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from repeat_ledger import note_repeat
        note_repeat("session-log-time-labels-drift-from-the-clock", data.get("session_id"), "; ".join(l for l, _ in bad))
    except Exception:
        pass
    right = time.strftime("%Y-%m-%d %H:%M", now)[:-1] + "x"
    reason = ("세션 로그 시각 라벨이 실제와 어긋난다: " + "; ".join(f"{l} ({why})" for l, why in bad)
              + f". 지금 맞는 라벨은 `{right}` — 기억으로 적지 말고 `date` 출력을 그대로 쓴다. "
              + f"옛 일을 뒤늦게 적는 것이면 명령에 `{MARK}` 를 붙인다.")
    return reason


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
