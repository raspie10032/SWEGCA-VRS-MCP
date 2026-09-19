# -*- coding: utf-8 -*-
"""G11 (2026-09-19): machine results close the loop — a command run for a hypothesis becomes an evidence row with
its exit code, tree and axis; the accumulator's standing decision names what the claim still lacks; and real runs
from enough producers, contexts and sources on every axis take a hypothesis out of abstain."""
import json
import os
import sys

from swegca_vrs2 import vrs_evidence as ve
from swegca_vrs2.harness import results
from swegca_vrs2.harness.recall import render
from swegca_vrs2.loopback import evidence_of, hook_recall
from swegca_vrs2.store import Main

PY = sys.executable
OK = [PY, "-c", "import sys; print('ran'); sys.exit(0)"]
FAIL = [PY, "-c", "import sys; print('boom', file=sys.stderr); sys.exit(1)"]


def ingest_via(m):
    """A ``produce`` stand-in: the observation goes straight into a test store (no daemon)."""
    def produce(*, producer, hypothesis, outcome, axes, context, source, text="", evidence=(), confidence=1.0, state=None,
                extra=None, client=None, **_):
        metadata = dict(kind="evidence", producer=producer, axes=list(axes), project=context, evidence=list(evidence),
                        confidence=confidence)
        for k, v in (extra or {}).items():
            metadata.setdefault(k, v)
        return m.ingest(dict(request_id="r:" + source, text=text, source=source, revision="1", outcome=outcome,
                             proposition=hypothesis, polarity="support" if outcome == "success" else "refute", metadata=metadata))
    return produce


def test_run_contract_binds_exit_tree_and_axis_and_feeds_the_intervention_axis(tmp_path):
    ledger = str(tmp_path / "run.log")
    m = Main(tmp_path / "s", allow_ingest=True)
    try:
        claim = "the fix makes the check pass"
        run = results.execute(OK)
        assert run["exit"] == 0 and "ran" in run["stdout_tail"] and not run["timed_out"]
        tree = results.tree_digest(str(tmp_path), watch=[str(tmp_path / "run.log")])
        obs = results.result_of(claim, "lab", OK, str(tmp_path), run, axis="intervention", tree=tree)
        assert obs["producer"] == "python-runner" or obs["producer"].endswith("-runner")
        assert obs["outcome"] == "success" and obs["axes"] == ["intervention"]
        assert obs["extra"]["run"]["exit"] == 0 and obs["extra"]["run"]["tree"]["key"] == tree["key"]
        line = results.record(obs, ledger=ledger, produce=ingest_via(m))
        assert line["recorded"] and line["axis"] == "intervention"
        # a counterfactual: the same check fails without the change, as predicted -> the experiment succeeded
        run2 = results.execute(FAIL)
        obs2 = results.result_of(claim, "lab", FAIL, str(tmp_path), run2, axis="counterfactual", expect="failure",
                                 tree={"key": "watch:before"})
        assert run2["exit"] == 1 and obs2["outcome"] == "success" and "boom" in obs2["text"]
        assert results.record(obs2, ledger=ledger, produce=ingest_via(m))["recorded"]
        rows = evidence_of(m, dict(proposition=claim))
        assert {(r["axes"][0], r["run"]["exit"]) for r in rows["rows"]} == {("intervention", 0), ("counterfactual", 1)}
        ev = ve.build(m.graph, m.memory)
        axes = ev.hypotheses["proposition:" + claim].summary()["axes"]
        assert axes["intervention"] == 1.0 and axes["counterfactual"] == 1.0
        assert json.loads(open(ledger, encoding="utf-8").read().splitlines()[0])["recorded"]
    finally:
        m.close()


def test_a_counterfactual_on_the_same_tree_is_refused_and_a_lost_result_is_flushed(tmp_path):
    ledger = str(tmp_path / "run.log")
    claim = "the same tree is not two experiments"
    run = results.execute(OK)
    tree = {"key": "git:abc/dirty:def"}
    first = results.result_of(claim, "lab", OK, str(tmp_path), run, axis="intervention", tree=tree)
    sink = []
    def produce(**kw):
        sink.append(kw); return dict(status="observation_recorded", episode_id=f"memory:{len(sink)}")
    assert results.record(first, ledger=ledger, produce=produce)["recorded"] == "memory:1"
    second = results.result_of(claim, "lab", OK, str(tmp_path), results.execute(FAIL), axis="counterfactual",
                               expect="failure", tree=tree)
    refused = results.record(second, ledger=ledger, produce=produce)
    assert refused["recorded"] is False and refused["error"].startswith("refused") and len(sink) == 1
    # with a different tree it is a counterfactual
    second["extra"]["run"]["tree"] = {"key": "git:abc"}
    assert results.record(second, ledger=ledger, produce=produce)["recorded"] == "memory:2"
    # the daemon is down: the result waits in the ledger, named; flush sends it
    def down(**kw):
        raise ConnectionError("no daemon")
    third = results.result_of(claim, "lab", OK, str(tmp_path), results.execute(OK), axis="observational")
    lost = results.record(third, ledger=ledger, produce=down)
    assert lost["recorded"] is None and lost["error"] and lost["pending"]["source"] == third["source"]
    sent = results.flush(ledger=ledger, produce=produce)
    assert len(sent) == 1 and sent[0]["closes"] == lost["id"] and sink[-1]["source"] == third["source"]
    assert results.flush(ledger=ledger, produce=produce) == []              # closed once


def test_hook_rows_carry_the_standing_decision_with_named_gaps(tmp_path):
    m = Main(tmp_path / "s", allow_ingest=True)
    try:
        claim = "the guard blocks a stale label"
        m.ingest(dict(request_id="v", text=f"판정: {claim}\n관문 라벨 정지 시험\n묻는 말: 라벨 관문 정지?", source="verdict:label", revision="1",
                      outcome="failure", proposition=claim, polarity="refute",
                      metadata=dict(kind="evidence", producer="asm-agent", axes=["observational"], project="p1")))
        before = hook_recall(m, dict(query="라벨 관문 정지 시험", limit=5, snippet=200))
        assert next(r for r in before["memories"] if r.get("proposition") == claim)["decision"] is None   # not consolidated yet
        m.consolidate(cycles=4)
        packet = hook_recall(m, dict(query="라벨 관문 정지 시험", limit=5, snippet=200))
        row = next(r for r in packet["memories"] if r.get("proposition") == claim)
        decision = row["decision"]
        assert decision["status"] == "abstain" and decision["reason"] == "minimum_effective_samples"
        assert decision["gaps"]["axes"]["intervention"] == 4 and decision["gaps"]["producers"] == 3
        text = render(packet, [row], [])
        assert "[증거 abstain: " in text and "개입 0/4" in text
        assert evidence_of(m, dict(proposition=claim))["decision"]["text"] == decision["text"]
    finally:
        m.close()


def test_real_runs_from_enough_producers_contexts_and_sources_leave_abstain(tmp_path):
    """The escape is reachable through the runner alone: 2 commands (source families) x 4 contexts x 4 producers,
    each run declaring the axis it stood on; the accumulator (unchanged) then accepts."""
    m = Main(tmp_path / "s", allow_ingest=True)
    try:
        claim = "the loop can leave abstain with machine results"
        produce = ingest_via(m)
        commands = {"a": OK, "b": [PY, "-c", "import sys; sys.exit(0)  # b"]}
        counter = 0
        for producer in ("pytest-runner", "bench-runner", "stop-hook", "batch-runner"):
            for context in ("p1", "p2", "p3", "p4"):
                for axis in ("observational", "intervention", "counterfactual"):
                    for name, cmd in commands.items():
                        run = results.execute(cmd if axis != "counterfactual" else FAIL)
                        counter += 1
                        obs = results.result_of(claim, context, cmd, str(tmp_path), run, axis=axis, producer=producer,
                                                expect="failure" if axis == "counterfactual" else "success",
                                                tree={"key": f"tree:{axis}"}, source=f"run:{name}#{producer}-{context}-{axis}-{counter}")
                        assert obs["outcome"] == "success"
                        line = results.record(obs, ledger=str(tmp_path / "run.log"), produce=produce)
                        assert line["recorded"]
        ev = ve.build(m.graph, m.memory)
        summary = ev.hypotheses["proposition:" + claim].summary()
        assert summary["status"] == "accept", summary
        assert ve.gaps(summary) == {}
        assert summary["producers"] == 4 and summary["contexts"] == 4 and min(summary["axes"].values()) >= 4
    finally:
        m.close()


def test_a_hook_or_batch_result_is_recorded_on_failure_and_once_a_day_on_success(tmp_path):
    ledger = str(tmp_path / "run.log")
    sink = []
    def produce(**kw):
        sink.append(kw); return dict(status="observation_recorded", episode_id=f"memory:{len(sink)}")
    claim = "the stop hook indexes the changed memory files of the current project"
    first = results.note_result(claim, "p1", "stop_reindex:p1", "success", producer="stop-hook", detail="3 rows", ledger=ledger, produce=produce)
    assert first["recorded"] == "memory:1" and sink[0]["axes"] == ["observational"] and sink[0]["producer"] == "stop-hook"
    assert results.note_result(claim, "p1", "stop_reindex:p1", "success", producer="stop-hook", detail="2 rows", ledger=ledger, produce=produce) is None
    failed = results.note_result(claim, "p1", "stop_reindex:p1", "failure", producer="stop-hook", detail="daemon down", ledger=ledger, produce=produce)
    assert failed["recorded"] == "memory:2" and sink[1]["outcome"] == "failure" and "daemon down" in sink[1]["text"]
    again = results.note_result(claim, "p1", "stop_reindex:p1", "failure", producer="stop-hook", detail="daemon down", ledger=ledger, produce=produce)
    assert again["recorded"] == "memory:3"                                        # a failure is never folded away
    assert results.note_result(claim, "p2", "stop_reindex:p2", "success", producer="stop-hook", detail="1 rows", ledger=ledger, produce=produce)["recorded"]
