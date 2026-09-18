#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vrs2 하니스 설치기 — 어느 OS 든 같은 절차 (2026-09-18).

1. `~/.claude/vrs2.json` 을 쓴다(src·state·tools·python·receipts·v02_*): 하니스 모듈과 도구가 전부 여기서 경로를 읽는다.
2. `~/.claude/hooks/` 에 껍데기 훅을 쓴다(json 에서 src 를 읽어 패키지를 import 하는 여섯 줄).
3. `~/.claude/settings.json` 의 hooks 에 이 OS 의 파이썬 경로로 항목을 넣는다(있으면 갱신, 다른 훅은 보존).
4. 스토어 디렉터리·영수증 디렉터리를 만든다.

    python vrs2-install.py [--src <repo>/src] [--state DIR] [--tools DIR] [--python PATH] [--receipts DIR]
                           [--v02-db PATH --v02-src DIR --v02-keys PATH] [--dry-run]

윈도(venv\\Scripts\\python.exe)·리눅스·맥(venv/bin/python) 차이는 --python 하나로 흡수한다. 리눅스/맥에서의 실제 실행은
2026-09-18 현재 이 PC 에 그 환경이 없어 **검증하지 않았다** — 경로·프로세스 생성·정규식만 OS 중립으로 고쳤다.
"""
import argparse
import io
import json
import os
import shutil
import sys
import time

HOME = os.path.expanduser("~")
CLAUDE = os.path.join(HOME, ".claude")
HERE = os.path.dirname(os.path.abspath(__file__))

SHIM = '''#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""껍데기: 몸통은 swegca_vrs2.harness.{mod} (docs/ADAPTER_SPEC.md). 경로는 ~/.claude/vrs2.json 또는 VRS2_SRC."""
import io
import json
import os
import sys

_cfg = os.path.join(os.path.expanduser("~"), ".claude", "vrs2.json")
try:
    _src = os.environ.get("VRS2_SRC") or json.load(io.open(_cfg, encoding="utf-8")).get("src")
except Exception:
    _src = None
if _src:
    sys.path.insert(0, _src)
from swegca_vrs2.harness.{mod} import main  # noqa: E402

if __name__ == "__main__":
    try:
        {call}
    except Exception:
        pass
'''
SHIMS = {  # hook file -> (module, call, event, matcher, extra args)
    "recall_context_v2.py": ("recall", "main()", "UserPromptSubmit", None, ""),
    "session_start.py": ("session_start", "main()", "SessionStart", None, ""),
    "precompact_snapshot.py": ("precompact", "main(sys.argv[1:])", "PreCompact", None, ""),
    "memory_use_log.py": ("read_log", "main()", "PostToolUse",
                          "Read|mcp__swegca-vrs__recall|mcp__swegca-vrs__get_episode|mcp__swegca-vrs2__memory_context|mcp__swegca-vrs2__memory_recall|mcp__swegca-vrs2__memory_read_path", ""),
    "bash_backslash_guard.py": ("guard_backslash", "main()", "PreToolUse", "Bash", ""),
    "log_label_guard.py": ("guard_label", "main()", "PreToolUse", "Bash|PowerShell", ""),
    "unopened_edit_guard.py": ("guard_unopened", "main()", "PreToolUse", "Write|Edit|Bash|PowerShell", ""),
    "stop_reindex_v2.py": ("reindex", "main()", "Stop", None, ""),
    "usage_ledger.py": ("usage", "main(sys.argv[1:])", "Stop", None, ""),
    "repeat_ledger.py": ("repeats", "main(sys.argv[1:])", "Stop", None, " --flush"),
    "hook_change_check.py": ("hook_check", "main()", "Stop", None, ""),
}
PROJECT_DIR_SHIM = '''# -*- coding: utf-8 -*-
"""껍데기: 몸통은 swegca_vrs2.harness.project_dir."""
import io
import json
import os
import sys

_cfg = os.path.join(os.path.expanduser("~"), ".claude", "vrs2.json")
try:
    _src = os.environ.get("VRS2_SRC") or json.load(io.open(_cfg, encoding="utf-8")).get("src")
except Exception:
    _src = None
if _src:
    sys.path.insert(0, _src)
from swegca_vrs2.harness.project_dir import PROJECTS, slug_of, resolve  # noqa: E402,F401
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--src", default=os.path.join(os.path.dirname(HERE), "src") if os.path.basename(os.path.dirname(HERE)) == "local" else None)
    ap.add_argument("--state", default=os.path.join(CLAUDE, "vrs2-memory"))
    ap.add_argument("--tools", default=HERE)
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--receipts", default=os.path.join(CLAUDE, "hooks"))
    ap.add_argument("--v02-db"); ap.add_argument("--v02-src"); ap.add_argument("--v02-keys")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    if not a.src:
        ap.error("--src <repo>/src 가 필요하다(도구 폴더가 <repo>/local/tools 가 아니면 자동으로 못 찾는다)")
    cfg = dict(src=os.path.abspath(a.src), state=os.path.abspath(a.state), tools=os.path.abspath(a.tools),
               python=os.path.abspath(a.python), receipts=os.path.abspath(a.receipts))
    for k in ("v02_db", "v02_src", "v02_keys"):
        v = getattr(a, k)
        if v:
            cfg[k] = os.path.abspath(v)
    print("config:", json.dumps(cfg, ensure_ascii=False, indent=1))
    if a.dry_run:
        return
    os.makedirs(CLAUDE, exist_ok=True); os.makedirs(cfg["receipts"], exist_ok=True); os.makedirs(cfg["state"], exist_ok=True)
    io.open(os.path.join(CLAUDE, "vrs2.json"), "w", encoding="utf-8", newline="\n").write(json.dumps(cfg, ensure_ascii=False, indent=1) + "\n")
    # shims
    hooks_dir = os.path.join(CLAUDE, "hooks")
    os.makedirs(hooks_dir, exist_ok=True)
    stamp = time.strftime("%H%M")
    for fname, (mod, call, *_rest) in SHIMS.items():
        path = os.path.join(hooks_dir, fname)
        if os.path.isfile(path):
            shutil.copy(path, path + f".pre-install-{stamp}")
        io.open(path, "w", encoding="utf-8", newline="\n").write(SHIM.format(mod=mod, call=call))
    io.open(os.path.join(hooks_dir, "project_dir.py"), "w", encoding="utf-8", newline="\n").write(PROJECT_DIR_SHIM)
    # settings.json hooks (merge: replace entries that point at these shims, keep everything else)
    settings_path = os.path.join(CLAUDE, "settings.json")
    try:
        settings = json.load(io.open(settings_path, encoding="utf-8")) if os.path.isfile(settings_path) else {}
    except ValueError:
        settings = {}
    if os.path.isfile(settings_path):
        shutil.copy(settings_path, settings_path + f".pre-install-{stamp}")
    hooks = settings.setdefault("hooks", {})
    py = cfg["python"].replace("\\", "/")
    for fname, (mod, call, event, matcher, extra) in SHIMS.items():
        cmd = f'"{py}" "{os.path.join(hooks_dir, fname).replace(chr(92), "/")}"{extra}'
        groups = hooks.setdefault(event, [])
        # drop any group entry that already runs this shim, then add ours
        for g in groups:
            g["hooks"] = [h for h in g.get("hooks", []) if fname not in h.get("command", "")]
        groups[:] = [g for g in groups if g.get("hooks")]
        entry = {"type": "command", "command": cmd, "timeout": 30}
        if matcher:
            groups.append({"matcher": matcher, "hooks": [entry]})
        else:
            groups.append({"hooks": [entry]})
    io.open(settings_path, "w", encoding="utf-8", newline="\n").write(json.dumps(settings, ensure_ascii=False, indent=2) + "\n")
    print(f"installed: {len(SHIMS)} shims in {hooks_dir}, settings.json hooks merged, config {os.path.join(CLAUDE, 'vrs2.json')}")


if __name__ == "__main__":
    main()
