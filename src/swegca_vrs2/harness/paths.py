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
import getpass
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
BUNDLE_LIMIT = int(_get("bundle_limit", 60000) or 60000)   # recommended records per bundle (docs/SIZING.md, 2026-09-19)
# G7 resident layer (2026-09-19): other bundles this machine's daemon answers for (id -> state dir; "main" is STATE),
# which bundle each project's Stop hook writes to (slug -> id; absent = main), how many others may stay hot
BUNDLES = {str(k): v for k, v in (_config().get("bundles") or {}).items() if k != "main" and v}
BUNDLE_OF = {str(k): str(v) for k, v in (_config().get("bundle_of") or {}).items() if v}
HOT_BUNDLES = int(_get("hot_bundles", 1) or 1)
V02_DB = _get("v02_db", None)
# signed producers (2026-09-19, 3.0 step 6): the registry of producer public keys (shareable), the directory of
# this machine's private keys, and the user identity carried as provenance on produced rows
PRODUCERS = _get("producers", os.path.join(HOME, ".claude", "vrs2-producers.json"))
KEYS = _get("keys", os.path.join(HOME, ".claude", "vrs2-keys"))
try:
    _LOGIN = getpass.getuser()
except Exception:
    _LOGIN = "unknown"
USER = str(_get("user", _LOGIN))
PRODUCE = os.path.join(TOOLS, "vrs2-produce.py")
IMPORTER = os.path.join(TOOLS, "vrs2-import.py")
CONFIRM_CMD = f'"{PYTHON}" "{os.path.join(TOOLS, "vrs2-confirm.py")}"'


def describe():
    return dict(config=CONFIG if os.path.isfile(CONFIG) else None, src=SRC, state=STATE, tools=TOOLS, python=PYTHON,
                receipts=RECEIPTS, v02_db=V02_DB, producers=PRODUCERS, keys=KEYS, user=USER, os=os.name, platform=sys.platform)
