# -*- coding: utf-8 -*-
"""Tests never write this machine's live receipts (2026-09-21, found during a premise check: three
``turns_miss: RuntimeError('down')`` lines in ~/.claude/hooks/session_start.log and ``slug: proj`` lines in
precompact_snapshot.log were the test suite, not the hooks — and the premise gate reads those files). Every
harness module keeps its receipt / ledger / state paths as module constants; this fixture points them all at
the test's own directory."""
import importlib
import os

import pytest

RECEIPT_CONSTANTS = {
    "swegca_vrs2.harness.transcripts": ("STATE_DIR", "RECEIPT", "WATCH_FILE"),
    "swegca_vrs2.harness.session_start": ("RECEIPT",),
    "swegca_vrs2.harness.precompact": ("LOG",),
    "swegca_vrs2.harness.read_log": ("LOG",),
    "swegca_vrs2.harness.usage": ("RECALL_LOG", "LEDGER", "RECEIPT"),
    "swegca_vrs2.harness.reindex": ("STATE_FILE", "RECEIPT"),
    "swegca_vrs2.harness.repeats": ("LEDGER", "FLUSHED", "RECEIPT"),
    "swegca_vrs2.harness.results": ("LEDGER",),
    "swegca_vrs2.harness.code_ledger": ("LEDGER_DIR",),
    "swegca_vrs2.harness.hook_check": ("SNAPSHOT",),
    "swegca_vrs2.harness.recall": ("RECEIPT", "LOG"),
}


@pytest.fixture(autouse=True)
def no_live_receipts(tmp_path, monkeypatch):
    sink = tmp_path / "receipts"
    sink.mkdir(exist_ok=True)
    for module_name, names in RECEIPT_CONSTANTS.items():
        try:
            module = importlib.import_module(module_name)
        except Exception:
            continue
        for name in names:
            if not hasattr(module, name):
                continue
            current = str(getattr(module, name))
            target = sink / os.path.basename(current.rstrip("/\\"))
            monkeypatch.setattr(module, name, str(target))
    yield sink
