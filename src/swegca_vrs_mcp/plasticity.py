"""Outcome-bearing relation graph around the unchanged VRS numerical kernel."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib

import numpy as np
import torch

from .core.vrs_refinement import refine_vrs
from .observations import canonical

EDGE_DTYPE = np.dtype([("source", "<u4"), ("target", "<u4"),
                       ("sign", "i1"), ("vrs_strength", "<f4")])


@dataclass(frozen=True)
class GraphResult:
    strengths: dict[str, float]
    scores: dict[str, float]
    snapshot_id: str
    converged: bool
    rounds: int
    evaluations: int
    max_delta: float
    active_outcomes: dict[str, int]

    def payload(self):
        return dict(self.__dict__)


def converge_graph(records: dict, *, previous: dict | None, now_ns: int,
                   max_rounds: int = 48, tolerance: float = 1e-5) -> GraphResult:
    """All stored outcome types participate; no pre-convergence pruning.

    A star relation per observation binds source experience to its proposition.
    This generic graph adapter is new; the numerical refine_vrs kernel is reused.
    Full-graph recomputation is explicit, not mislabeled as incremental arithmetic.
    """
    if not 1 <= max_rounds <= 128 or not 0 < tolerance <= 1e-3:
        raise ValueError("invalid convergence limits")
    hypotheses = sorted({r["event"]["hypothesis_id"] for r in records.values()})
    event_ids = list(records)
    hindex = {h: i for i, h in enumerate(hypotheses)}
    n = len(hypotheses) + len(event_ids)
    direct = np.zeros(n, dtype=np.float32)
    unresolved = np.zeros(n, dtype=np.bool_)
    edges = np.empty(len(event_ids), dtype=EDGE_DTYPE)
    superseded = {old for r in records.values() if r["authenticated"]
                  for old in r["event"]["supersedes"]}
    active_outcomes = {k: 0 for k in ("success", "failure", "negative", "uncertain", "conflict", "pending")}
    counts = {h: [0, 0] for h in hypotheses}
    for i, eid in enumerate(event_ids):
        record = records[eid]
        event = record["event"]
        h = event["hypothesis_id"]
        source, target = len(hypotheses) + i, hindex[h]
        outcome = event["outcome"]
        active_outcomes[outcome] += 1
        expired = event["expires_at_ns"] is not None and event["expires_at_ns"] <= now_ns
        resolved = record["authenticated"] and eid not in superseded and not expired
        polarity = 1 if outcome == "success" else -1 if outcome in {"failure", "negative"} else 0
        # Superseded evidence remains connected/active, but no longer claims truth.
        if resolved and polarity:
            direct[source] = np.tanh(1.0)
            counts[h][0 if polarity > 0 else 1] += 1
        else:
            unresolved[source] = True
            if eid not in superseded:
                unresolved[target] = True
        edges[i] = (source, target, polarity or 1, 0.75)
    for h, (support, refute) in counts.items():
        direct[hindex[h]] = np.tanh((support - refute) / max(1, support + refute))
        if support and refute:
            unresolved[hindex[h]] = True
    prior = previous or {}
    weights = np.array([prior.get("strengths", {}).get(eid, 0.75) for eid in event_ids], dtype=np.float32)
    state = direct.copy()
    evaluations = 0
    delta = 0.0
    converged = not event_ids
    rounds = 0
    for rounds in range(1, max_rounds + 1):
        if not event_ids:
            rounds = 0
            break
        new_state, _, new_weights, evaluated, _ = refine_vrs(
            direct, edges, shuffle_cycles=16, reinforcement_passes=1,
            edge_batch_size=max(1, min(len(edges), 4096)), seed=1729,
            device=torch.device("cpu"), initial_state=state,
            initial_vrs_strengths=weights, unresolved_term_mask=unresolved)
        evaluations += evaluated
        delta = float(max(np.max(np.abs(new_state - state)), np.max(np.abs(new_weights - weights))))
        state, weights = new_state, new_weights
        if not np.isfinite(state).all() or not np.isfinite(weights).all():
            raise ValueError("nonfinite VRS result")
        if delta <= tolerance:
            converged = True
            break
    strengths = {eid: float(weights[i]) for i, eid in enumerate(event_ids)}
    scores = {h: float(state[i]) for i, h in enumerate(hypotheses)}
    snapshot = hashlib.sha256(canonical({"strengths": strengths, "scores": scores,
                                       "converged": converged,
                                       "rounds": rounds, "evaluations": evaluations, "max_delta": delta,
                                       "active_outcomes": active_outcomes,
                                       "records": [(eid, records[eid]["content_hash"], records[eid]["authenticated"])
                                                   for eid in event_ids]})).hexdigest()
    return GraphResult(strengths, scores, snapshot, converged, rounds, evaluations, delta, active_outcomes)
