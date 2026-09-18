# -*- coding: utf-8 -*-
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
