#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PreToolUse(Bash) 가드: 명령에 백슬래시 두 개(\\\\)가 있으면 막는다.

이 하니스의 Bash 도구는 명령을 bash 에 넘기는 길에 백슬래시 쌍을 하나로 접는다
(2026-09-11 실측: 2->1, 3->2, 4->2, 6->3; 따옴표 친 'EOF' 헤리독·인라인 인용·
python -c 모두 같다. PowerShell 도구는 온전). 단일 백슬래시는 그대로 간다.
그래서 파이썬 문자열 이스케이프("\\\\n"), sed 치환 이스케이프, 윈도 경로가 든 JSON
같은 것을 Bash 로 쓰면 조용히 깨진다 — 하루에 일곱 번 깨진 뒤에 쟀다.
판정 `obs:bash-tool-collapses-doubled-backslashes`.

    stdin : {"tool_name": "Bash", "tool_input": {"command": "..."}}
    stdout: 막을 때만 permissionDecision=deny 와 사유. 통과면 아무것도 안 낸다.
    통과 표식: 명령 어딘가에 `# backslash-ok` — 접힘을 알고 일부러 겹쳐 쓴 경우.

가드는 절대 도구를 못 쓰게 만들지 않는다 — 어떤 예외도 통과로 처리한다.
"""
import json
import sys

MARK = "# backslash-ok"
REASON = ("Bash 도구는 백슬래시 쌍(\\\\)을 하나로 접는다 — 실측 2->1, 4->2, "
          "따옴표 헤리독도 같다. 이 명령의 \\\\ 는 셸에 \\ 로 도착해 조용히 깨진다. "
          "파이썬 이스케이프·sed 이스케이프·윈도 경로 JSON 은 Write 도구나 PowerShell "
          "도구로 쓴다. 접힘을 알고 일부러 겹친 것이면 명령에 `" + MARK + "` 를 붙인다.")


def decide(tool_name, command, session_id=None):
    """Adapter entry (2026-09-18): the deny reason for this command, or None.
    The collapse was measured on the Windows Bash tool; elsewhere the gate stays off unless VRS2_BACKSLASH_GUARD=1."""
    import os
    if os.name != "nt" and os.environ.get("VRS2_BACKSLASH_GUARD") != "1":
        return None
    command = str(command or "")
    if tool_name != "Bash" or "\\\\" not in command or MARK in command:
        return None
    try:                                   # 반복 계수기(2026-09-18): 막은 시도는 판정의 반복 관측이 된다
        import os
        from .repeats import note_repeat
        note_repeat("bash-tool-collapses-doubled-backslashes", session_id, command[:120])
    except Exception:
        pass
    return REASON


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return
    reason = decide(data.get("tool_name"), (data.get("tool_input") or {}).get("command"), data.get("session_id"))
    if not reason:
        return
    out = {"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                  "permissionDecision": "deny",
                                  "permissionDecisionReason": reason}}
    sys.stdout.buffer.write(json.dumps(out, ensure_ascii=False).encode("utf-8"))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
