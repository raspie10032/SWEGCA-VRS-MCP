# -*- coding: utf-8 -*-
"""Where the vrs2 tools find the package, the store and the interpreter (2026-09-18, OS-neutral).

Same resolution as swegca_vrs2.harness.paths: env VRS2_* → ~/.claude/vrs2.json → defaults. Kept as a standalone file so
a tool can be run before the package is importable (it adds SRC to sys.path).
"""
import io
import json
import os
import sys

HOME = os.path.expanduser("~")
CONFIG = os.path.join(HOME, ".claude", "vrs2.json")
HERE = os.path.dirname(os.path.abspath(__file__))


def _cfg():
    try:
        return json.load(io.open(CONFIG, encoding="utf-8")) if os.path.isfile(CONFIG) else {}
    except ValueError:
        return {}


def _get(key, default):
    return os.environ.get("VRS2_" + key.upper()) or _cfg().get(key) or default


SRC = _get("src", os.path.join(os.path.dirname(os.path.dirname(HERE)), "src") if os.path.basename(os.path.dirname(HERE)) == "local" else HERE)
STATE = _get("state", os.path.join(HOME, ".claude", "vrs2-memory"))
TOOLS = _get("tools", HERE)
PY = _get("python", sys.executable)
RECEIPTS = _get("receipts", os.path.join(HOME, ".claude", "hooks"))
V02_DB = _get("v02_db", None)
V02_SRC = _get("v02_src", None)
V02_KEYS = _get("v02_keys", None)
PRODUCE = os.path.join(TOOLS, "vrs2-produce.py")
if SRC not in sys.path:
    sys.path.insert(0, SRC)
