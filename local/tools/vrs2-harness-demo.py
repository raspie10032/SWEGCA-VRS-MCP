#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""어댑터 규격 실증 — Claude Code 없이 한 턴을 돈다 (2026-09-18).

before_prompt → (모델 자리: 여기선 답을 흉내 낸다) → on_read → before_action(가드) → before_context_loss → after_context_loss
→ on_stop → emit, 그리고 conformance 로 이 세션이 여섯 자리에 영수증을 남겼는지 센다. 모델은 아무거나 끼우면 된다 —
Luna/Sol 실험 하니스는 이 뼈대에 API 호출을 넣는 것.

    vrs2-venv python vrs2-harness-demo.py [--session demo-xxxx]
"""
import argparse
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, r"C:/Users/asm/mcp/SWEGCA-VRS-MCP-v2/src")
from swegca_vrs2 import adapter  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", default="demo" + time.strftime("%H%M"))
    ap.add_argument("--cwd", default=r"C:/Users/asm/Desktop/새 폴더")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    sid = a.session
    print("session", sid)

    # 1. before_prompt
    prompt = "압축 뒤 세션 로그가 왜 안 실렸나"
    ctx = adapter.before_prompt(prompt, a.cwd, sid)
    print("1 before_prompt:", (ctx or "(none)")[:110].replace("\n", " "))

    # 2. on_read — the model opens what the receipt pointed at
    opened = adapter.on_read(r"C:\Users\asm\.claude\projects\C--Users-asm-Desktop-----\memory\session-log.md", 809, 1, sid)
    print("2 on_read:", opened)

    # 3. before_action — a gate
    deny = adapter.before_action("Bash", {"command": "cat >> memory/session-log.md <<'EOF'\n- 2026-01-01 00:0x: x\nEOF"}, sid)
    print("3 before_action (bad label):", "deny" if deny else "allow", "|", (deny or "")[:60])
    print("3 before_action (plain):", adapter.before_action("Bash", {"command": "echo hi"}, sid) or "allow")

    # 4. before_context_loss — snapshot from a tiny fake transcript into a throwaway project log
    lab = tempfile.mkdtemp(prefix="vrs2-harness-")
    proj = os.path.join(os.path.expanduser("~"), ".claude", "projects", adapter.hook("project_dir.py").slug_of(lab), "memory")
    os.makedirs(proj, exist_ok=True)
    open(os.path.join(proj, "session-log.md"), "w", encoding="utf-8").write("- 2026-09-18 00:0x: 시험 로그\n")
    transcript = os.path.join(lab, "t.jsonl")
    with open(transcript, "w", encoding="utf-8") as out:
        out.write(json.dumps({"type": "user", "message": {"content": "데모 요청 하나"}}, ensure_ascii=False) + "\n")
        out.write(json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "데모 답. 다음: 두 번째 단계"}]}}, ensure_ascii=False) + "\n")
    entry = adapter.before_context_loss(transcript, lab, sid, trigger="demo")
    print("4 before_context_loss:", (entry or "(none)")[:100])

    # 5. after_context_loss — what gets re-injected
    back = adapter.after_context_loss(lab, "compact", sid)
    print("5 after_context_loss:", (back or "(none)")[:100].replace("\n", " "))

    # 6. on_stop
    result = adapter.on_stop(a.cwd, sid)
    print("6 on_stop:", {k: (v if isinstance(v, (str, int, float, list)) else type(v).__name__) for k, v in result.items()})

    # emit — one observation, then the conformance count for this session
    r = adapter.emit("harness-demo", "the adapter loop runs one full turn without claude code", "success", ["intervention"],
                     f"demo-{time.strftime('%Y-%m-%d')}", f"demo:harness#{sid}", "vrs2-harness-demo.py 한 턴", [__file__])
    print("emit:", r.get("status"))
    print("conformance for this session:", adapter.conformance(sid))
    import shutil; shutil.rmtree(lab, ignore_errors=True); shutil.rmtree(os.path.dirname(proj), ignore_errors=True)


if __name__ == "__main__":
    main()
