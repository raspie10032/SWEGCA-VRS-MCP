#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""껍데기 (2026-09-18): 몸통은 swegca_vrs2.harness.recall — docs/ADAPTER_SPEC.md 의 한 자리."""
import sys
sys.path.insert(0, r"C:\Users\asm\mcp\SWEGCA-VRS-MCP-v2\src")
from swegca_vrs2.harness.recall import main  # noqa: E402

if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
