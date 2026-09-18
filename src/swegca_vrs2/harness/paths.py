# -*- coding: utf-8 -*-
"""Where the harness keeps receipts, ledgers and state (2026-09-18)."""
import os

RECEIPTS = os.environ.get("VRS2_RECEIPTS") or os.path.join(os.path.expanduser("~"), ".claude", "hooks")
SRC = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATE = os.environ.get("VRS2_STATE") or r"C:\Users\asm\mcp\vrs2-memory"
TOOLS = os.environ.get("VRS2_TOOLS") or r"C:\Users\asm\mcp"
