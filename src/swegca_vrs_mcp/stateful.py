"""Exclusive, transactional main for standalone SWEGCA+VRS cognition."""
from __future__ import annotations

from dataclasses import asdict
import hashlib
import json
import re
from itertools import groupby
from pathlib import Path
import sqlite3
import threading
import time
import uuid

from filelock import FileLock, Timeout
import torch

from .architecture.mosaic_cognitive_kernel import CognitiveState
from .architecture.mosaic_evidence_accumulator import (
    EvidenceAccumulatorConfig, EvidenceAccumulatorState, EvidenceObservation,
    assess_accumulator, update_accumulator,
)
from .architecture.mosaic_bounded_world_write import (
    BoundedWorldWriteConfig, bounded_verification_write, cognitive_state_hash,
    world_write_gates_from_decision,
)
from .architecture.mosaic_synapse_arbiter import SingleWorldArbiter, SynapseProposal
from .architecture.mosaic_omni import SLOT_ROLES
from .core.mosaic_memory_activation import (
    MemoryEpisode, MemoryStep, CurrentEvidenceVerdict, activate_memory,
    append_memory_activation_index, build_memory_activation_index,
    detect_deja_vu, recall_memory,
)
from .observations import Observation, Authenticator, canonical
from .plasticity import converge_graph
from .runtime import plain


class CoreError(RuntimeError):
    """Sanitized failure code suitable for a tool response."""


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def episode(record):
    row = record["event"]
    tokens = re.findall(r"\w+", canonical(row["observation"]).decode().casefold())
    return MemoryEpisode(row["event_id"], tuple(dict.fromkeys([row["hypothesis_id"], *row["cues"], *tokens])),
        (MemoryStep("observation_outcome", row["observation"], (row["hypothesis_id"],),
                    row["hypothesis_id"], row["outcome"], tuple(row["evidence_refs"])),),
        tuple(row["evidence_refs"]), record["content_hash"],
        "authenticated_observation_not_belief" if record["authenticated"] else "pending_untrusted")


class StatefulCore:
    def __init__(self, directory: Path, *, writable=False, authenticator=None, clock=time.time_ns):
        self._mutex = threading.RLock()
        self._clock = clock
        self._writable = writable
        self._auth = authenticator or Authenticator()
        self._poisoned = False
        self._closed = False
        self._fault = lambda stage: None  # Test-only fault injection; never exposed over MCP.
        if not directory.exists() and not writable:
            raise CoreError("store_missing_read_only")
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self._lease = FileLock(str(directory / "owner.lock"))
        try:
            self._lease.acquire(timeout=0)
        except Timeout:
            raise CoreError("store_already_owned") from None
        try:
            database = directory / "core.sqlite3"
            self._db = sqlite3.connect(str(database) if writable else database.resolve().as_uri() + "?mode=ro", uri=not writable, isolation_level=None,
                                       check_same_thread=False, timeout=5)
            if writable:
                self._db.execute("PRAGMA journal_mode=WAL")
                self._db.execute("PRAGMA synchronous=FULL")
                self._db.executescript("""
                CREATE TABLE IF NOT EXISTS head (id INTEGER PRIMARY KEY CHECK(id=1), body TEXT NOT NULL, sha TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS observations (seq INTEGER PRIMARY KEY, event_id TEXT UNIQUE NOT NULL, body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS operations (request_id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, receipt TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS graphs (snapshot_id TEXT PRIMARY KEY, body TEXT NOT NULL, sha TEXT NOT NULL);
            """)
            found = self._db.execute("SELECT body,sha FROM head WHERE id=1").fetchone()
            self._records = {}
            for body, in self._db.execute("SELECT body FROM observations ORDER BY seq"):
                record = json.loads(body)
                event = Observation.model_validate(record["event"])
                if digest(event.model_dump()) != record["content_hash"]:
                    raise CoreError("observation_corrupt")
                authentic, family = self._auth.authenticate(event, record["admitted_at_ns"])
                if authentic != record["authenticated"] or family != record["source_family"]:
                    raise CoreError("trust_binding_changed")
                self._records[event.event_id] = record
            self._operations = {rid: (fp, json.loads(receipt)) for rid, fp, receipt in
                                self._db.execute("SELECT request_id,fingerprint,receipt FROM operations")}
            if found is None:
                if self._records or not writable:
                    raise CoreError("missing_store_head")
                self.owner_id = "swegca:" + str(uuid.uuid4())
                self.revision = 0
                self._world = self._empty_world()
                self._graph = None
                self._dirty = set()
                self._claims = {}
                self._last_commit = None
                self._world_hash = cognitive_state_hash(self._world)
                self._write_initial()
            else:
                head = json.loads(found[0])
                if digest(head) != found[1] or head.get("schema") != 1:
                    raise CoreError("store_head_corrupt")
                if head["trust_fingerprint"] != self._auth.fingerprint:
                    raise CoreError("trust_policy_changed_requires_explicit_migration")
                if head["observation_count"] != len(self._records):
                    raise CoreError("store_generation_mismatch")
                if head["graph"] is not None:
                    stored = self._db.execute("SELECT body,sha FROM graphs WHERE snapshot_id=?",
                        (head["graph"]["snapshot_id"],)).fetchone()
                    if stored is None:
                        raise CoreError("graph_generation_missing")
                    graph = json.loads(stored[0])
                    if digest(graph) != stored[1] or graph["snapshot_id"] != head["graph"]["snapshot_id"]:
                        raise CoreError("graph_generation_corrupt")
                    head["graph"] = graph
                self.owner_id, self.revision = head["owner_id"], head["revision"]
                self._world = CognitiveState.from_dict(head["world"])
                self._world_hash = cognitive_state_hash(self._world)
                if self._world_hash != head["world_hash"] or self._world.owner_id != self.owner_id:
                    raise CoreError("world_integrity_failure")
                self._graph, self._dirty = head["graph"], set(head["dirty"])
                self._claims, self._last_commit = head["claims"], head["last_commit"]
            self._memory = build_memory_activation_index(())
            # Replay admission waves, preserving the same structural-sharing root
            # after restart instead of silently changing snapshot identity.
            for _, wave in groupby(self._records.values(), key=lambda r: r["admission_revision"]):
                self._memory = append_memory_activation_index(self._memory, tuple(episode(r) for r in wave))
            self._bundles = {}
            self._refresh_bundles({r["event"]["hypothesis_id"] for r in self._records.values()})
        except BaseException:
            if hasattr(self, "_db"):
                self._db.close()
            self._lease.release()
            raise

    def _empty_world(self):
        return CognitiveState(torch.zeros(1, 20, 8), torch.zeros(1, 6, 8),
                              torch.zeros(1, 6, 8), owner_id=self.owner_id)

    def _head(self, *, revision=None, records=None, graph=None, dirty=None,
              world=None, claims=None, last_commit=None):
        return {"schema": 1, "owner_id": self.owner_id,
                "revision": self.revision if revision is None else revision,
                "trust_fingerprint": self._auth.fingerprint,
                "observation_count": len(self._records if records is None else records),
                "graph": self._graph if graph is None else graph,
                "dirty": sorted(self._dirty if dirty is None else dirty),
                "world": (self._world if world is None else world).to_dict(),
                "world_hash": cognitive_state_hash(self._world if world is None else world),
                "claims": self._claims if claims is None else claims,
                "last_commit": last_commit}

    def _write_initial(self):
        head = self._head()
        self._db.execute("INSERT INTO head VALUES(1,?,?)", (canonical(head).decode(), digest(head)))

    @staticmethod
    def _disk_head(head):
        # Refer to immutable graph generations: ingestion must not serialize and
        # rehash every VRS edge merely to publish a new experience batch.
        return {**head, "graph": {"snapshot_id": head["graph"]["snapshot_id"]} if head["graph"] else None}

    def close(self):
        with self._mutex:
            if not self._closed:
                self._db.close()
                self._lease.release()
                self._closed = True

    def _check(self):
        if self._closed or self._poisoned:
            raise CoreError("core_unavailable_reopen_store")

    def _begin(self, request_id, expected_revision, fingerprint):
        self._check()
        if not self._writable:
            raise CoreError("writes_disabled")
        if request_id in self._operations:
            old_fp, receipt = self._operations[request_id]
            if old_fp != fingerprint:
                raise CoreError("request_id_reused_with_different_payload")
            return json.loads(canonical(receipt))
        if expected_revision != self.revision:
            raise CoreError("stale_revision")
        return None

    def _publish(self, request_id, fingerprint, head, receipt, additions, memory):
        if len(canonical(receipt)) > 1_048_576:
            raise CoreError("receipt_too_large_before_commit")
        committed = False
        graph_changed = head["graph"] is not self._graph
        try:
            self._db.execute("BEGIN IMMEDIATE")
            for r in additions:
                self._db.execute("INSERT INTO observations(event_id,body) VALUES(?,?)",
                                 (r["event"]["event_id"], canonical(r).decode()))
            if graph_changed and head["graph"]:
                graph = head["graph"]
                self._db.execute("INSERT OR IGNORE INTO graphs VALUES(?,?,?)",
                    (graph["snapshot_id"], canonical(graph).decode(), digest(graph)))
            disk_head = self._disk_head(head)
            self._db.execute("UPDATE head SET body=?,sha=? WHERE id=1",
                             (canonical(disk_head).decode(), digest(disk_head)))
            self._db.execute("INSERT INTO operations VALUES(?,?,?)",
                             (request_id, fingerprint, canonical(receipt).decode()))
            self._fault("before_commit")
            self._db.execute("COMMIT")
            committed = True
            self._fault("after_commit")
            for r in additions:
                self._records[r["event"]["event_id"]] = r
            self.revision = head["revision"]
            self._world = CognitiveState.from_dict(head["world"])
            self._world_hash = head["world_hash"]
            self._graph, self._dirty = head["graph"], set(head["dirty"])
            self._claims, self._last_commit = head["claims"], head["last_commit"]
            self._memory = memory
            touched = ({r["event"]["hypothesis_id"] for r in self._records.values()} if graph_changed
                       else {r["event"]["hypothesis_id"] for r in additions})
            self._refresh_bundles(touched)
            self._operations[request_id] = fingerprint, receipt
        except BaseException:
            if committed:
                self._poisoned = True
            elif self._db.in_transaction:
                self._db.execute("ROLLBACK")
            raise
        return json.loads(canonical(receipt))

    def status(self):
        with self._mutex:
            self._check()
            return {"mode": "stateful_core", "owner_id": self.owner_id,
                    "revision": self.revision, "persistent_state_count": 1,
                    "episode_count": len(self._records), "memory_snapshot_id": self._memory.snapshot_id,
                    "outcome_counts": dict(self._memory.outcome_counts), "world_hash": self._world_hash,
                    "dirty_hypotheses": sorted(self._dirty), "world_claims": sorted(self._claims),
                    "graph_converged": bool(self._graph and self._graph["converged"] and not self._dirty),
                    "vrs_snapshot_id": self._graph["snapshot_id"] if self._graph else None,
                    "writable": self._writable, "hot_lookup_requires_io": False,
                    "growth_claimed": False, "external_action_authorized": False}

    def get_episode(self, event_id):
        with self._mutex:
            self._check()
            return plain(self._memory.episode(event_id))

    def recall(self, query, cues=(), offset=0, limit=50):
        with self._mutex:
            self._check()
            if offset < 0 or not 1 <= limit <= 200:
                raise ValueError("invalid pagination")
            signal = detect_deja_vu(self._memory, query=query,
                current_cues=tuple(dict.fromkeys([query, *re.findall(r"\w+", query.casefold()), *cues])))
            recalled = recall_memory(self._memory, signal)
            selected = recalled.candidates[offset:offset + limit]
            return {"revision": self.revision, "memory_snapshot_id": self._memory.snapshot_id,
                    "candidate_count": len(recalled.candidates), "candidates": plain(selected),
                    "episodes": [plain(self._memory.episode(c.episode_id)) for c in selected],
                    "next_offset": offset + limit if offset + limit < len(recalled.candidates) else None,
                    "retrieval": "lexical_cues_not_embedding_semantics", "authority_granted": False}

    def ingest(self, rows, *, request_id, expected_revision):
        with self._mutex:
            fp = digest({"kind": "ingest", "rows": [r.model_dump() for r in rows]})
            prior = self._begin(request_id, expected_revision, fp)
            if prior is not None:
                return prior
            records = dict(self._records)
            additions, touched = [], set()
            for supplied in rows:
                row = Observation.model_validate_json(canonical(supplied.model_dump()))
                content = digest(row.model_dump())
                if row.event_id in records:
                    if records[row.event_id]["content_hash"] != content:
                        raise CoreError("event_id_content_conflict")
                    continue
                admitted = self._clock()
                authentic, family = self._auth.authenticate(row, admitted)
                for old in row.supersedes:
                    prior_row = records.get(old)
                    if (not authentic or prior_row is None or
                        prior_row["event"]["hypothesis_id"] != row.hypothesis_id or
                        prior_row["event"]["producer_id"] != row.producer_id):
                        raise CoreError("invalid_supersession")
                record = {"event": row.model_dump(), "content_hash": content,
                          "authenticated": authentic, "source_family": family, "admitted_at_ns": admitted,
                          "admission_revision": self.revision + 1}
                records[row.event_id] = record
                additions.append(record)
                touched.add(row.hypothesis_id)
            memory = (append_memory_activation_index(self._memory, tuple(episode(r) for r in additions))
                      if additions else self._memory)
            dirty = self._dirty | touched
            claims = {h: e for h, e in self._claims.items() if h not in touched}
            world, claims = self._build_world(claims, records, memory, self._graph, dirty)
            head = self._head(revision=self.revision + 1, records=records, dirty=dirty,
                              world=world, claims=claims)
            receipt = {"status": "observations_committed", "revision": head["revision"],
                       "added": len(additions), "duplicates": len(rows) - len(additions),
                       "authenticated": sum(r["authenticated"] for r in additions),
                       "raw_is_belief": False, "invalidated_world_claims": sorted(set(self._claims) - set(claims))}
            return self._publish(request_id, fp, head, receipt, additions, memory)

    def converge(self, *, request_id, expected_revision, max_rounds=48):
        with self._mutex:
            fp = digest({"kind": "converge", "max_rounds": max_rounds})
            prior = self._begin(request_id, expected_revision, fp)
            if prior is not None:
                return prior
            graph = converge_graph(self._records, previous=self._graph, now_ns=self._clock(),
                                   max_rounds=max_rounds).payload()
            dirty = set() if graph["converged"] else set(self._dirty)
            world, claims = self._build_world(self._claims, self._records, self._memory, graph, dirty)
            head = self._head(revision=self.revision + 1, graph=graph, dirty=dirty, world=world, claims=claims)
            receipt = {"status": "vrs_converged" if graph["converged"] else "vrs_pending",
                       "revision": head["revision"], "graph": {k: v for k, v in graph.items() if k not in {"strengths", "scores"}}, "pre_convergence_pruning": 0,
                       "raw_observations_preserved": len(self._records), "global_graph_arithmetic": True}
            return self._publish(request_id, fp, head, receipt, [], self._memory)

    def _prepare_bundle(self, hypothesis, records, memory, graph):
        related = [eid for eid in memory.episode_ids_for_cue(hypothesis)
                   if records[eid]["event"]["hypothesis_id"] == hypothesis]
        superseded = {old for eid in related if records[eid]["authenticated"]
                      for old in records[eid]["event"]["supersedes"]}
        config = EvidenceAccumulatorConfig()
        accumulator = EvidenceAccumulatorState.empty(hypothesis, config)
        now, expiries = self._clock(), []
        for eid in related:
            r, e = records[eid], records[eid]["event"]
            expiry = e["expires_at_ns"]
            if expiry is not None and expiry > now:
                expiries.append(expiry)
            if (not graph or not graph["converged"] or not r["authenticated"] or eid in superseded
                    or (expiry is not None and expiry <= now) or graph["strengths"].get(eid, 0) < 1):
                continue
            verdict = "support" if e["outcome"] == "success" else "refute" if e["outcome"] in {"failure", "negative"} else None
            if verdict:
                accumulator = update_accumulator(accumulator, EvidenceObservation(
                    hypothesis, eid, r["source_family"], e["context_id"], e["axis"], verdict,
                    e["observed_at_ns"], expiry, e["producer_id"]), config, current_step=now).state
        return assess_accumulator(accumulator, config), superseded, min(expiries, default=None)

    def _refresh_bundles(self, hypotheses):
        for h in hypotheses:
            self._bundles[h] = self._prepare_bundle(h, self._records, self._memory, self._graph)
        self._empty_decision = assess_accumulator(EvidenceAccumulatorState.empty("unknown", EvidenceAccumulatorConfig()), EvidenceAccumulatorConfig())

    def _judge(self, hypothesis, current_id, records, memory, graph, dirty, *, prepare=False):
        now = self._clock()
        current = records.get(current_id)
        decision, superseded, expiry = (self._prepare_bundle(hypothesis, records, memory, graph) if prepare
            else self._bundles.get(hypothesis, (self._empty_decision, set(), None)))
        def valid(eid):
            r = records[eid]
            expiry = r["event"]["expires_at_ns"]
            return r["authenticated"] and eid not in superseded and (expiry is None or expiry > now)
        current_valid = (current is not None and current["event"]["hypothesis_id"] == hypothesis
                         and valid(current_id) and current["event"]["outcome"] in {"success", "failure", "negative"})
        ready = bool(graph and graph["converged"] and hypothesis not in dirty and current_valid
                     and (expiry is None or now < expiry))
        def assess(replay):
            row = records[replay.episode_id]["event"]
            promoted = ready and row["hypothesis_id"] == hypothesis and valid(replay.episode_id) and graph["strengths"].get(replay.episode_id, 0) >= 1.0
            verdict = ("support" if row["outcome"] == "success" else "refute"
                       if row["outcome"] in {"failure", "negative"} else "conflict"
                       if row["outcome"] == "conflict" else "insufficient") if promoted else "insufficient"
            return CurrentEvidenceVerdict(replay.episode_id, hypothesis, verdict,
                "Authenticated, current, non-superseded VRS evidence" if promoted else "Unpromoted, stale, pending or untrusted",
                tuple(current["event"]["evidence_refs"]) if current_valid else (), ())
        activation = activate_memory(memory, query=hypothesis, current_cues=[hypothesis], judge=assess)
        accepted = ready and not activation.re_evidence.should_abstain and decision.status == "accept"
        # Current refutation cannot be overruled by a residual of old supports.
        if current_valid and current["event"]["outcome"] != "success":
            accepted = False
        return activation, decision, accepted

    def judge(self, hypothesis, current_event_id):
        with self._mutex:
            self._check()
            activation, decision, accepted = self._judge(hypothesis, current_event_id,
                self._records, self._memory, self._graph, self._dirty)
            return {"revision": self.revision, "memory_snapshot_id": self._memory.snapshot_id,
                    "vrs_snapshot_id": self._graph["snapshot_id"] if self._graph else None,
                    "activation": plain(activation), "accumulator": asdict(decision),
                    "eligible_for_bounded_world_write": accepted,
                    "world_committed": False, "external_action_authorized": False}

    def _write_claim(self, state, hypothesis, current, records, memory, graph, dirty):
        activation, decision, accepted = self._judge(hypothesis, current, records, memory, graph, dirty, prepare=True)
        if not accepted:
            return None
        ids = activation.re_evidence.selected_support
        producers = sorted({records[eid]["event"]["producer_id"] for eid in ids})
        verification = SLOT_ROLES.index("verification")
        proposals = []
        for producer in producers:
            delta = torch.zeros(1, 32, 8)
            delta[0, verification, 0] = 0.01 * graph["scores"][hypothesis]
            mask = torch.zeros(1, 32, dtype=torch.bool)
            mask[0, verification] = True
            proposals.append(SynapseProposal(producer, delta, torch.tensor([decision.posterior_mean]),
                torch.zeros(1), torch.tensor([1 - decision.causal_lower_bound]), mask,
                (tuple(eid for eid in ids if records[eid]["event"]["producer_id"] == producer),)))
        arbitration = SingleWorldArbiter(minimum_weight=0.5)(state, tuple(proposals), commit=False)
        if not bool(arbitration.accepted.any()) or bool(arbitration.unresolved_contradiction.any()):
            return None
        proposal = SynapseProposal("main-arbitrated-evidence", arbitration.proposed_delta,
            torch.ones(1), torch.zeros(1), torch.zeros(1), proposals[0].target_slot_mask, (tuple(ids),))
        gates = world_write_gates_from_decision(decision,
            definitions_complete=bool(hypothesis),
            counterfactual_support=any(records[eid]["event"]["axis"] == "counterfactual" for eid in ids),
            intervention_support=any(records[eid]["event"]["axis"] == "intervention" for eid in ids),
            regime_change_suspected=decision.regime_change_score >= EvidenceAccumulatorConfig().regime_change_threshold,
            slot_gate_passed=state.persistent_state_count == 1,
            device_gate_passed=state.semantic_slots.device.type == "cpu", capacity_strategy_safe=True,
            evidence_current=True, accumulator_revision_current=True, runtime_context_safe=True)
        result = bounded_verification_write(state, proposal, gates, BoundedWorldWriteConfig(), commit=True)
        return result.state if result.committed else None

    def _build_world(self, claims, records, memory, graph, dirty):
        # Recompute only the small active claim ledger after invalidation; never
        # mutate live tensors or average conflicting proposals into the World.
        state, kept = self._empty_world(), {}
        for hypothesis, current in sorted(claims.items()):
            updated = self._write_claim(state, hypothesis, current, records, memory, graph, dirty)
            if updated is not None:
                state, kept[hypothesis] = updated, current
        return state, kept

    def commit_judgment(self, hypothesis, current_event_id, *, request_id, expected_revision):
        with self._mutex:
            fp = digest({"kind": "world_commit", "hypothesis": hypothesis, "current": current_event_id})
            prior = self._begin(request_id, expected_revision, fp)
            if prior is not None:
                return prior
            claims = {**self._claims, hypothesis: current_event_id}
            world, kept = self._build_world(claims, self._records, self._memory, self._graph, self._dirty)
            if hypothesis not in kept:
                return {"status": "world_write_rejected", "revision": self.revision,
                        "reason": "evidence_vrs_arbitration_or_bounded_gate", "world_committed": False}
            head = self._head(revision=self.revision + 1, world=world, claims=kept,
                last_commit={"revision": self.revision + 1, "request_id": request_id,
                             "before_world": self._world.to_dict(), "before_claims": self._claims,
                             "before_hash": self._world_hash, "after_hash": cognitive_state_hash(world)})
            receipt = {"status": "world_committed", "revision": head["revision"],
                       "before_hash": self._world_hash, "after_hash": head["world_hash"],
                       "target_role": "verification", "world_committed": True,
                       "external_action_authorized": False}
            return self._publish(request_id, fp, head, receipt, [], self._memory)

    def rollback_world(self, *, request_id, expected_revision):
        with self._mutex:
            fp = digest({"kind": "rollback", "expected_revision": expected_revision})
            prior = self._begin(request_id, expected_revision, fp)
            if prior is not None:
                return prior
            last = self._last_commit
            if not last or last["revision"] != self.revision or last["after_hash"] != self._world_hash:
                raise CoreError("rollback_not_current_world_head")
            world = CognitiveState.from_dict(last["before_world"])
            if cognitive_state_hash(world) != last["before_hash"]:
                raise CoreError("rollback_preimage_corrupt")
            head = self._head(revision=self.revision + 1, world=world, claims=last["before_claims"])
            receipt = {"status": "world_rolled_back", "revision": head["revision"],
                       "restored_hash": head["world_hash"], "experience_deleted": False}
            return self._publish(request_id, fp, head, receipt, [], self._memory)

    def world(self):
        with self._mutex:
            self._check()
            validity = {h: self._judge(h, e, self._records, self._memory, self._graph, self._dirty)[2]
                        for h, e in self._claims.items()}
            return {"revision": self.revision, "state": self._world.to_dict(),
                    "state_hash": self._world_hash, "claim_authority_current": validity,
                    "external_action_authorized": False}
