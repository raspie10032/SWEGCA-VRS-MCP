# -*- coding: utf-8 -*-
"""Where the harness finds the store, the tools, the python, and keeps receipts (2026-09-18, OS-neutral).

Resolution order for every path: environment variable → ``~/.claude/vrs2.json`` → default.

    VRS2_SRC       package source dir            default: this repo's src (from __file__)
    VRS2_STATE     store directory               default: ~/.claude/vrs2-memory
    VRS2_TOOLS     vrs2-*.py tools dir           default: <repo>/local/tools
    VRS2_PYTHON    interpreter for daemon/tools  default: sys.executable
    VRS2_RECEIPTS  receipts/ledgers/state dir    default: ~/.claude/hooks
    VRS2_V02_DB    v0.2 signed store (optional)  default: none

``vrs2-install.py`` writes the json for a machine; nothing here is Windows-specific.
"""
import io
import json
import os
import sys

HOME = os.path.expanduser("~")
CONFIG = os.path.join(HOME, ".claude", "vrs2.json")
_REPO_SRC = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_REPO = os.path.dirname(_REPO_SRC)


def _config():
    try:
        return json.load(io.open(CONFIG, encoding="utf-8")) if os.path.isfile(CONFIG) else {}
    except ValueError:
        return {}


def _get(key, default):
    env = os.environ.get("VRS2_" + key.upper())
    if env:
        return env
    value = _config().get(key)
    return value if value else default


SRC = _get("src", _REPO_SRC)
STATE = _get("state", os.path.join(HOME, ".claude", "vrs2-memory"))
TOOLS = _get("tools", os.path.join(_REPO, "local", "tools"))
PYTHON = _get("python", sys.executable)
RECEIPTS = _get("receipts", os.path.join(HOME, ".claude", "hooks"))
V02_DB = _get("v02_db", None)
PRODUCE = os.path.join(TOOLS, "vrs2-produce.py")
IMPORTER = os.path.join(TOOLS, "vrs2-import.py")
CONFIRM_CMD = f'"{PYTHON}" "{os.path.join(TOOLS, "vrs2-confirm.py")}"'


def describe():
    return dict(config=CONFIG if os.path.isfile(CONFIG) else None, src=SRC, state=STATE, tools=TOOLS, python=PYTHON,
                receipts=RECEIPTS, v02_db=V02_DB, os=os.name, platform=sys.platform)
