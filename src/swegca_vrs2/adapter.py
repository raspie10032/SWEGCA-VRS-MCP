# -*- coding: utf-8 -*-
"""The harness loop around the store as one contract (2026-09-18).

Six interception points, each with a payload, a return, and a receipt. Claude Code implements them with
hooks (``local/hooks``); any other harness — an API driver for another model, another CLI — calls these
functions instead and gets the same behaviour, so the harness stops being a variable when models are
compared. The bodies live in the hook modules (they are the reference implementation and keep their
receipts); this module locates and calls them. See ``docs/ADAPTER_SPEC.md``.

    before_prompt(prompt, cwd, session_id)        -> context text | None      receipt: recall_context.log
    on_read(path, offset, limit, session_id)      -> bool                     receipt: recall_context.log (use: read)
    before_action(tool, tool_input, session_id)   -> None | deny reason       receipt: repeat_ledger.log when denied
    before_context_loss(transcript, cwd, session_id, trigger) -> entry | None receipt: precompact_snapshot.log
    after_context_loss(cwd, source)               -> context text | None      receipt: session_start.log
    on_stop(cwd, session_id)                      -> dict of step results     receipts: stop_reindex_v2 / usage_ledger / repeat_ledger / hook check
    emit(...) / confirm(...) / alias(...)         -> store results            receipt: vrs2_produce.log, journal rows

Degradation rules (a harness that lacks an interception point):
    * no pre-prompt hook      -> the harness prepends before_prompt() itself; if it cannot, it must call
                                 on_stop() so at least the ledgers and index stay current (recall then only
                                 reaches the next harness that can inject)
    * no pre-action hook      -> call before_action() *after* the action and record the denial as a repeat
                                 (a post-hoc gate: it cannot prevent, it still counts)
    * no context-loss event   -> call before_context_loss() from on_stop() every N stops (periodic snapshot)
    * no read event           -> usage stays at "injected", never "opened": say so in the report
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
import sys
import time

from .harness import (recall as _recall, read_log as _read_log, guard_backslash as _gb, guard_label as _gl,
                      guard_unopened as _gu, precompact as _precompact, session_start as _session_start,
                      reindex as _reindex, usage as _usage, repeats as _repeats, hook_check as _hook_check,
                      project_dir as _project_dir)
from .harness.paths import RECEIPTS as RECEIPTS_DIR, TOOLS, STATE, SRC

_cache = {}


def tool(name):
    """A script from the tools directory (vrs2-produce.py etc.), loaded once."""
    if name in _cache:
        return _cache[name]
    path = os.path.join(TOOLS, name)
    spec = importlib.util.spec_from_file_location(name.replace("-", "_").replace(".py", ""), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _cache[name] = module
    return module


# ── the six points ────────────────────────────────────────────────────────

def before_prompt(prompt, cwd, session_id):
    """Recall receipts for this prompt as text (verdicts with asks, one record, 「열기」 lines, 「※ 재검증 필요」
    with both sides, [열림 m/n], ⚠ 반복). None when the store has nothing worth injecting."""
    return _recall.context_for(prompt, cwd, session_id)


def on_read(path, offset, limit, session_id):
    """The model opened a memory file (usage re-evidence). True when it was a memory doc."""
    return _read_log.note_read(path, offset, limit, session_id)


def before_action(tool_name, tool_input, session_id):
    """Gates. Returns None (allow) or the reason to deny. A denial is also a repeat observation."""
    inp = tool_input or {}
    command = inp.get("command")
    for reason in (_gb.decide(tool_name, command, session_id), _gl.decide(tool_name, command, session_id),
                   _gu.decide(tool_name, inp, session_id)):
        if reason:
            return reason
    return None


def before_context_loss(transcript_path, cwd, session_id, trigger="harness"):
    """Write the 「압축 직전 자동」 entry from the transcript tail into the project's session log."""
    m = _precompact
    records = m.tail_records(transcript_path)
    entry = m.build_entry(records, trigger, session_id)
    if not entry:
        return None
    slug, memory = _project_dir.resolve(cwd)
    log_path = os.path.join(memory, "session-log.md")
    if not os.path.isfile(log_path) or m.already_there(log_path, entry):
        return None
    with open(log_path, "a", encoding="utf-8") as out:
        out.write(entry + "\n")
    m.receipt(written=len(entry), slug=slug, trigger=trigger, session=str(session_id)[:8])
    return entry


def after_context_loss(cwd, source="compact", session_id=None):
    """The session-log tail to re-inject after compaction/resume/startup."""
    return _session_start.context_for(cwd, source, session_id)


def on_stop(cwd, session_id):
    """Everything that keeps the store current between turns: index changed docs, usage ledger, repeat flush,
    hook verification. Returns what each step reported."""
    out = {}
    for key, call in (("reindex", lambda: _reindex.run(cwd, session_id)),
                      ("usage", lambda: _usage.flush_session(session_id)),
                      ("repeats", lambda: _repeats.flush(str(session_id)[:8])),
                      ("hooks", lambda: _hook_check.check())):
        try:
            out[key] = call()
        except Exception as failure:
            out[key] = f"error: {failure!r}"[:200]
    return out


# ── evidence ─────────────────────────────────────────────────────────────

def emit(producer, hypothesis, outcome, axes, context, source, text="", evidence=(), confidence=1.0):
    """An observation on a declared proposition (SWEGCA producer)."""
    return tool("vrs2-produce.py").produce(producer=producer, hypothesis=hypothesis, outcome=outcome, axes=list(axes),
                                           context=context, source=source, text=text, evidence=list(evidence),
                                           confidence=confidence)


def confirm(hypothesis, holds, evidence_path, session_id, text="", context=None):
    """The session's current evidence for/against a recalled proposition (revalidation path)."""
    return emit("session-main", hypothesis, "success" if holds else "failure", ["observational"],
                context or f"session-{str(session_id)[:8]}", f"{evidence_path.replace(chr(92), '/')}#file", text,
                [evidence_path], 0.5)


def alias(canonical, aliases):
    """Bind alias propositions to a canonical one (hypothesis registry)."""
    m = tool("vrs2-produce.py")
    from .loopback import ensure_daemon
    client = ensure_daemon(m.STATE, allow_ingest=True, python=m.PY)
    try:
        return client.request("alias", canonical=canonical, aliases=list(aliases))
    finally:
        client.close()


# ── conformance ──────────────────────────────────────────────────────────

RECEIPTS = {
    "before_prompt": ("recall_context.log", lambda r: "injected" in r or "skip" in r),
    "on_read": ("recall_context.log", lambda r: r.get("use") == "read"),
    "before_action": ("repeat_ledger.log", lambda r: "slug" in r),
    "before_context_loss": ("precompact_snapshot.log", lambda r: "written" in r or "skip" in r),
    "after_context_loss": ("session_start.log", lambda r: "source" in r),
    "on_stop": ("usage_ledger.log", lambda r: "session" in r),
}


def conformance(session_id=None, since=None):
    """How many receipts each interception point left (optionally for one session / since a timestamp).
    A harness conforms to a point when it leaves that receipt; zero means the point is not wired."""
    hooks_dir = RECEIPTS_DIR
    out = {}
    for point, (log_name, match) in RECEIPTS.items():
        path = os.path.join(hooks_dir, log_name)
        n = 0
        if os.path.isfile(path):
            for raw in io.open(path, encoding="utf-8", errors="replace"):
                try:
                    r = json.loads(raw)
                except ValueError:
                    continue
                if session_id and str(r.get("session") or "")[:8] != str(session_id)[:8]:
                    continue
                if since and str(r.get("ts") or "") < since:
                    continue
                if match(r):
                    n += 1
        out[point] = n
    return out


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sid = sys.argv[1] if len(sys.argv) > 1 else None
    print(json.dumps(conformance(sid), ensure_ascii=False))
