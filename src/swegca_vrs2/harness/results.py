# -*- coding: utf-8 -*-
"""Machine results as evidence — the minimal G11 (2026-09-19).

The loop the goal asks for is intent → action → actual outcome → experience → VRS → later cognition. Before
this module the store's evidence came from claims (verdicts, confirmations, bench notes) and from the gates;
no hypothesis had a single observation on the ``intervention`` or ``counterfactual`` axis, so every one of
the 42 live hypotheses abstained at the accumulator's first check (``minimum_effective_samples``) whatever
was claimed about it. Those two axes can only be fed by something that *ran*: a test on the changed tree
(intervention) and the same test on the tree without the change (counterfactual).

One contract for every machine result::

    observation = result_of(hypothesis, context, command, cwd, exit_code, expect, axis, ...)
    -> dict(producer, hypothesis, outcome, axes, context, source, text, extra=dict(run=...))

``outcome`` is the *experiment's*: success when the exit code met the expectation (``--expect failure`` on a
counterfactual run: the test fails without the fix, as predicted). The raw exit code, command, duration and
the tree digest (git HEAD + dirty state, or watched files) are bound in ``metadata.run`` so the row says
what actually happened. The producer is the runner (``pytest-runner``, ``<script>-runner``, ``stop-hook``),
a distinct source from the agent's own claims; the source family is the command digest, so re-running the
same command is one group for the accumulator — repetition is not new evidence (a settled verdict here).

A counterfactual run on the same tree digest as its intervention run is refused: it would be the same
experiment twice under two names. Every run is one line in ``vrs2_run.log`` (jsonl) before anything is
sent; a line the daemon could not take is re-sent by ``flush()`` (the Stop hook calls it), so a result is
never lost to a daemon that was down at the time — the miss is in the ledger, named.
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import time

from .paths import RECEIPTS, PRODUCE, STATE

LEDGER = os.path.join(RECEIPTS, "vrs2_run.log")
AXES = ("observational", "counterfactual", "intervention", "cross_context")
TAIL = 1500
AXIS_NOTE = {
    "observational": "바꾼 것 없이 관측",
    "intervention": "바꾼 트리에서 실행 (개입)",
    "counterfactual": "바꾸지 않은 트리에서 실행 (반사실)",
    "cross_context": "다른 맥락에서 실행",
}


def command_digest(command):
    return hashlib.sha256("\x1f".join(str(c) for c in command).encode("utf-8")).hexdigest()[:12]


def producer_of(command):
    """The runner's producer id: ``pytest-runner`` for pytest, ``<script>-runner`` for a script, else the program."""
    parts = [str(c) for c in command]
    if any("pytest" in p for p in parts[:3]):
        return "pytest-runner"
    for p in parts:
        base = os.path.basename(p)
        if base.endswith(".py"):
            return base[:-3] + "-runner"
    return (os.path.basename(parts[0]) if parts else "command") + "-runner"


def _git(cwd, *args):
    try:
        out = subprocess.run(["git", *args], cwd=cwd, capture_output=True, timeout=20)
        return out.stdout.decode("utf-8", "replace") if out.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def tree_digest(cwd, watch=()):
    """What the command ran against: git HEAD and a digest of the dirty state (status + diff), and/or a digest
    of watched files (for trees outside git, or a copy under test). ``key`` joins them for the pairing check."""
    tree = {}
    head = _git(cwd, "rev-parse", "HEAD")
    if head:
        tree["git"] = head.strip()[:12]
        status = _git(cwd, "status", "--porcelain") or ""
        diff = _git(cwd, "diff") or ""
        if status.strip():
            tree["dirty"] = hashlib.sha256((status + diff).encode("utf-8")).hexdigest()[:12]
    if watch:
        h = hashlib.sha256()
        for path in watch:
            path = os.path.join(cwd, path) if cwd and not os.path.isabs(path) else path
            h.update(path.encode("utf-8"))
            try:
                with open(path, "rb") as f:
                    h.update(f.read())
            except OSError:
                h.update(b"<missing>")
        tree["watch"] = h.hexdigest()[:12]
    tree["key"] = "/".join(f"{k}:{v}" for k, v in tree.items() if k != "key") or "unknown"
    return tree


def execute(command, cwd=None, timeout=None, tail=TAIL, env=None):
    """Run the command; exit code, duration, output tails. A timeout is exit ``-1`` with ``timed_out``."""
    started = time.perf_counter_ns()
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        done = subprocess.run([str(c) for c in command], cwd=cwd or None, capture_output=True, timeout=timeout, env=env)
        exit_code, out, err, timed_out = done.returncode, done.stdout, done.stderr, False
    except subprocess.TimeoutExpired as expired:
        exit_code, out, err, timed_out = -1, expired.stdout or b"", expired.stderr or b"", True
    except OSError as error:
        exit_code, out, err, timed_out = -2, b"", repr(error).encode("utf-8"), False
    duration_ms = (time.perf_counter_ns() - started) // 1_000_000
    return dict(exit=exit_code, duration_ms=duration_ms, timed_out=timed_out, stamp=stamp,
                stdout_tail=out.decode("utf-8", "replace")[-tail:], stderr_tail=err.decode("utf-8", "replace")[-tail:])


def result_of(hypothesis, context, command, cwd, run, *, expect="success", axis="observational", producer=None,
              source=None, tree=None, evidence=(), confidence=1.0, note=""):
    """The evidence observation for one run — the arguments of ``produce()`` plus ``extra.run``."""
    if axis not in AXES:
        raise ValueError("axis: " + ", ".join(AXES))
    if expect not in ("success", "failure"):
        raise ValueError("expect: success 또는 failure")
    ran_ok = run["exit"] == 0
    outcome = "success" if ran_ok == (expect == "success") else "failure"
    digest = command_digest(command)
    run_id = hashlib.sha256(f"{digest}|{run['stamp']}|{cwd}|{run['exit']}|{os.getpid()}".encode("utf-8")).hexdigest()[:10]
    tree = tree or {}
    body = "\n".join(x for x in [
        f"실행: {' '.join(str(c) for c in command)}",
        f"작업 디렉터리: {cwd or os.getcwd()} · 트리: {tree.get('key', 'unknown')}",
        f"종료 코드: {run['exit']}{' (시간 초과)' if run.get('timed_out') else ''} (기대: {expect}) → 실험 {outcome}",
        f"소요: {run['duration_ms']} ms · 축: {axis} — {AXIS_NOTE[axis]}",
        note.strip(),
        ("--- stdout 꼬리 ---\n" + run["stdout_tail"].strip()) if run.get("stdout_tail", "").strip() else "",
        ("--- stderr 꼬리 ---\n" + run["stderr_tail"].strip()) if run.get("stderr_tail", "").strip() else "",
    ] if x)
    return dict(producer=producer or producer_of(command), hypothesis=hypothesis, outcome=outcome, axes=[axis],
                context=context, source=source or f"run:{digest}#{run_id}", text=body, evidence=list(evidence),
                confidence=float(confidence),
                extra=dict(run=dict(id=run_id, command=[str(c) for c in command][:32], digest=digest, cwd=str(cwd or os.getcwd()),
                                    exit=run["exit"], expect=expect, duration_ms=run["duration_ms"],
                                    timed_out=bool(run.get("timed_out")), tree=dict(tree), stamp=run["stamp"])))


def ledger_lines(path=LEDGER):
    if not os.path.exists(path):
        return []
    rows = []
    for raw in io.open(path, encoding="utf-8", errors="replace"):
        try:
            rows.append(json.loads(raw))
        except ValueError:
            continue
    return rows


def pairing_conflict(observation, lines):
    """A counterfactual run must differ in tree from the intervention run of the same hypothesis and command
    (and the other way round): the same tree under two axis names is one experiment counted twice."""
    axis = observation["axes"][0]
    if axis not in ("intervention", "counterfactual"):
        return None
    other = "counterfactual" if axis == "intervention" else "intervention"
    run = observation["extra"]["run"]
    for line in lines:
        if line.get("hypothesis") != observation["hypothesis"] or line.get("digest") != run["digest"]:
            continue
        if line.get("axis") == other and line.get("tree") == run["tree"].get("key") and run["tree"].get("key") != "unknown":
            return f"{axis} run on the same tree ({run['tree']['key']}) as the {other} run {line.get('id')} — not a {axis}"
    return None


def _append(line, path=LEDGER):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "a", encoding="utf-8") as out:
            out.write(json.dumps(line, ensure_ascii=False) + "\n")
    except OSError:
        pass


def _ledger_line(observation, **fields):
    run = observation["extra"]["run"]
    line = dict(id=run["id"], ts=run["stamp"], hypothesis=observation["hypothesis"], axis=observation["axes"][0],
                expect=run["expect"], exit=run["exit"], outcome=observation["outcome"], tree=run["tree"].get("key"),
                digest=run["digest"], command=" ".join(run["command"])[:300], producer=observation["producer"],
                source=observation["source"], context=observation["context"], recorded=None)
    line.update(fields)
    return line


def _produce_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("vrs2_produce", PRODUCE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def record(observation, *, state=STATE, client=None, ledger=LEDGER, force=False, dry=False, produce=None):
    """Ledger first, then the store. Returns the ledger line (``recorded`` = episode id, or None with ``error``)."""
    conflict = pairing_conflict(observation, ledger_lines(ledger))
    if conflict and not force:
        line = _ledger_line(observation, error="refused: " + conflict, recorded=False)
        _append(line, ledger)
        return line
    if dry:
        return _ledger_line(observation, recorded=False, dry=True)
    line = _ledger_line(observation, forced=bool(conflict and force))
    try:
        fn = produce or _produce_module().produce
        result = fn(producer=observation["producer"], hypothesis=observation["hypothesis"], outcome=observation["outcome"],
                    axes=observation["axes"], context=observation["context"], source=observation["source"],
                    text=observation["text"], evidence=observation.get("evidence") or (),
                    confidence=observation.get("confidence", 1.0), state=state, extra=observation.get("extra"), client=client)
        line["recorded"] = result.get("episode_id") or result.get("status")
        line["status"] = result.get("status")
    except Exception as error:
        line["error"] = repr(error)[:200]
        line["pending"] = observation             # everything needed to send it later (flush)
    _append(line, ledger)
    return line


def flush(*, state=STATE, client=None, ledger=LEDGER, produce=None, limit=50):
    """Re-send ledger lines the daemon could not take (``pending``); each success appends a closing line."""
    lines = ledger_lines(ledger)
    closed = {l.get("closes") for l in lines if l.get("closes")}
    sent = []
    for line in lines:
        if not line.get("pending") or line.get("id") in closed or line.get("recorded"):
            continue
        observation = line["pending"]
        try:
            fn = produce or _produce_module().produce
            result = fn(producer=observation["producer"], hypothesis=observation["hypothesis"], outcome=observation["outcome"],
                        axes=observation["axes"], context=observation["context"], source=observation["source"],
                        text=observation["text"], evidence=observation.get("evidence") or (),
                        confidence=observation.get("confidence", 1.0), state=state, extra=observation.get("extra"), client=client)
        except Exception:
            continue
        closing = _ledger_line(observation, closes=line["id"], recorded=result.get("episode_id") or result.get("status"),
                               status=result.get("status"), flushed=True)
        _append(closing, ledger)
        closed.add(line["id"])
        sent.append(closing)
        if len(sent) >= limit:
            break
    return sent


def note_result(hypothesis, context, source_family, outcome, *, producer, detail="", daily=True, ledger=LEDGER, **kw):
    """A machine result that did not come from ``execute`` (a hook, a batch): failures are recorded every time,
    successes once per day per source family when ``daily`` — a heartbeat, not a row per tick."""
    day = time.strftime("%Y-%m-%d")
    if outcome == "success" and daily:
        for line in ledger_lines(ledger):
            if line.get("source", "").startswith(source_family + "#") and line.get("outcome") == "success" \
                    and str(line.get("ts", "")).startswith(day) and line.get("recorded"):
                return None
    run = dict(exit=0 if outcome == "success" else 1, duration_ms=0, timed_out=False, stamp=time.strftime("%Y-%m-%d %H:%M:%S"),
               stdout_tail="", stderr_tail=detail[-TAIL:])
    observation = result_of(hypothesis, context, [producer], None, run, expect="success", axis="observational",
                            producer=producer, source=f"{source_family}#{day}-{hashlib.sha256(detail.encode('utf-8')).hexdigest()[:6]}",
                            tree={"key": "unknown"})
    return record(observation, ledger=ledger, **kw)


def main(argv):
    """CLI body shared by ``vrs2-run.py``: ``--hypothesis … --context … [options] -- command…`` or ``--flush``."""
    import argparse
    ap = argparse.ArgumentParser(prog="vrs2-run.py", description="run a command as a machine result for a hypothesis (G11)")
    ap.add_argument("--hypothesis"); ap.add_argument("--context")
    ap.add_argument("--axis", default="observational", choices=AXES[:3])
    ap.add_argument("--expect", default="success", choices=["success", "failure"])
    ap.add_argument("--producer"); ap.add_argument("--source"); ap.add_argument("--cwd", default=os.getcwd())
    ap.add_argument("--timeout", type=float, default=None); ap.add_argument("--watch", nargs="*", default=[])
    ap.add_argument("--note", default=""); ap.add_argument("--evidence", nargs="*", default=[])
    ap.add_argument("--confidence", type=float, default=1.0); ap.add_argument("--state", default=STATE)
    ap.add_argument("--force", action="store_true"); ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--flush", action="store_true"); ap.add_argument("--quiet", action="store_true")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    if a.flush:
        sent = flush(state=a.state)
        print(f"flushed {len(sent)}")
        return 0
    command = a.command[1:] if a.command and a.command[0] == "--" else a.command
    if not command or not a.hypothesis or not a.context:
        ap.error("--hypothesis, --context and a command after -- are required")
    run = execute(command, cwd=a.cwd, timeout=a.timeout)
    if not a.quiet:
        sys.stdout.write(run["stdout_tail"]); sys.stderr.write(run["stderr_tail"])
    tree = tree_digest(a.cwd, a.watch)
    observation = result_of(a.hypothesis, a.context, command, a.cwd, run, expect=a.expect, axis=a.axis,
                            producer=a.producer, source=a.source, tree=tree, evidence=a.evidence, confidence=a.confidence, note=a.note)
    line = record(observation, state=a.state, force=a.force, dry=a.dry_run)
    print(f"\n[vrs2-run] exit {run['exit']} in {run['duration_ms']} ms → {observation['outcome']} ({a.axis}, expected {a.expect}) "
          f"| {observation['producer']} | tree {tree['key']} | {'dry' if a.dry_run else line.get('recorded') or line.get('error')}")
    # the wrapper's exit is the experiment's: 0 when the run met the expectation, 1 when it did not, 2 when the
    # result could not be recorded (refused pairing, or no daemon — then it waits in the ledger for --flush)
    if line.get("error"):
        return 2
    return 0 if observation["outcome"] == "success" else 1
