#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""껍데기: 몸통은 swegca_vrs2.harness.precompact (docs/ADAPTER_SPEC.md). 경로는 ~/.claude/vrs2.json 또는 VRS2_SRC."""
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
from swegca_vrs2.harness.precompact import main  # noqa: E402

if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Exception:
        pass
