import hashlib
import sqlite3

import pytest

from swegca_vrs_mcp.observations import Observation, Authenticator, Producer, TrustConfig, sign_observation
from swegca_vrs_mcp.stateful import StatefulCore, CoreError

NOW = 1_800_000_000_000_000_000
AXES = ["observational", "counterfactual", "intervention", "cross_context"]


def row(eid="e0", hyp="compiler", outcome="success", producer="p0", axis="observational", **kw):
    return Observation(event_id=eid, hypothesis_id=hyp, producer_id=producer,
        context_id=eid, axis=axis, outcome=outcome, observation={"text": "compiler build result"},
        cues=[hyp], evidence_refs=["fixture:" + eid], observed_at_ns=NOW - 100, **kw)


def signed_rows(hyp="compiler"):
    producers, rows = {}, []
    for i in range(32):
        pid = f"p{i}"
        # Explicitly synthetic test keys, never operator credentials.
        key = hashlib.sha256(f"synthetic-producer-{i}".encode()).digest()
        producers[pid] = Producer(key_hex=key.hex(), source_family=f"family{i % 4}", allowed_axes=AXES)
        event = row(f"{hyp}-{i}", hyp, producer=pid, axis=AXES[i % 4])
        rows.append(event.model_copy(update={"signature": sign_observation(event, key)}))
    return Authenticator(TrustConfig(producers=producers)), rows


def open_core(path, **kw):
    return StatefulCore(path, clock=lambda: NOW, **kw)


def ingest(core, rows, rid="ingest"):
    return core.ingest(rows, request_id=rid, expected_revision=core.revision)


def converge(core, rid="vrs", **kw):
    return core.converge(request_id=rid, expected_revision=core.revision, **kw)


def test_unsigned_all_outcomes_restart_and_lexical_recall(tmp_path):
    core = open_core(tmp_path, writable=True)
    outcomes = ["success", "failure", "negative", "uncertain", "conflict", "pending"]
    assert ingest(core, [row(str(i), outcome=o) for i, o in enumerate(outcomes)])["added"] == 6
    before = core.status()
    assert core.recall("build")["candidate_count"] == 6
    result = converge(core)
    assert result["graph"]["converged"], result
    assert result["graph"]["active_outcomes"] == dict.fromkeys(outcomes, 1)
    assert result["graph"]["evaluations"] >= 6
    assert not core.judge("compiler", "0")["eligible_for_bounded_world_write"]
    after = core.status()
    core.close()
    core = open_core(tmp_path)
    assert core.status()["memory_snapshot_id"] == before["memory_snapshot_id"]
    assert core.status()["vrs_snapshot_id"] == after["vrs_snapshot_id"]
    assert core.status()["owner_id"] == after["owner_id"]
    assert core.recall("build")["candidate_count"] == 6
    with pytest.raises(CoreError, match="writes_disabled"):
        ingest(core, [row("new")])
    core.close()


def test_authenticated_trajectory_commit_restart_rollback(tmp_path):
    auth, rows = signed_rows()
    core = open_core(tmp_path, writable=True, authenticator=auth)
    baseline = core.world()["state_hash"]
    ingest(core, rows)
    assert not core.judge("compiler", rows[-1].event_id)["eligible_for_bounded_world_write"]
    vrs = converge(core)
    assert vrs["graph"]["converged"], vrs
    judged = core.judge("compiler", rows[-1].event_id)
    assert judged["eligible_for_bounded_world_write"], judged["accumulator"]
    receipt = core.commit_judgment("compiler", rows[-1].event_id, request_id="commit", expected_revision=2)
    assert receipt["world_committed"], receipt
    assert receipt["after_hash"] != baseline
    memory_id = core.status()["memory_snapshot_id"]
    core.close()
    core = open_core(tmp_path, writable=True, authenticator=auth)
    assert core.world()["state_hash"] == receipt["after_hash"]
    assert core.status()["memory_snapshot_id"] == memory_id
    rolled = core.rollback_world(request_id="rollback", expected_revision=3)
    assert rolled["restored_hash"] == baseline
    assert core.status()["episode_count"] == 32
    core.close()


def test_idempotency_cas_duplicate_and_conflict(tmp_path):
    core = open_core(tmp_path, writable=True)
    first = ingest(core, [row()])
    assert core.ingest([row()], request_id="ingest", expected_revision=0) == first
    with pytest.raises(CoreError, match="different_payload"):
        core.ingest([row("other")], request_id="ingest", expected_revision=0)
    with pytest.raises(CoreError, match="stale_revision"):
        core.ingest([row("other")], request_id="other", expected_revision=0)
    assert ingest(core, [row()], "repeat")["added"] == 0
    with pytest.raises(CoreError, match="content_conflict"):
        ingest(core, [row(outcome="failure")], "conflict")
    assert core.status()["episode_count"] == 1
    core.close()


@pytest.mark.parametrize("stage", ["before_commit", "after_commit"])
def test_transaction_fault_and_retry(tmp_path, stage):
    core = open_core(tmp_path, writable=True)
    def fault(at):
        if at == stage:
            raise RuntimeError("injected")
    core._fault = fault
    with pytest.raises(RuntimeError, match="injected"):
        ingest(core, [row()])
    if stage == "after_commit":
        with pytest.raises(CoreError, match="reopen_store"):
            core.status()
    else:
        assert core.status()["episode_count"] == 0
    core.close()
    core = open_core(tmp_path, writable=True)
    assert core.status()["episode_count"] == (stage == "after_commit")
    core.ingest([row()], request_id="ingest", expected_revision=0)
    assert core.status()["episode_count"] == 1
    core.close()


def test_exclusive_owner_and_corruption(tmp_path):
    core = open_core(tmp_path, writable=True)
    with pytest.raises(CoreError, match="already_owned"):
        open_core(tmp_path, writable=True)
    core.close()
    with sqlite3.connect(tmp_path / "core.sqlite3") as db:
        db.execute("UPDATE head SET sha='broken'")
    with pytest.raises(CoreError, match="corrupt"):
        open_core(tmp_path)


def test_hot_judgment_has_no_disk_json_hash_or_full_scan(tmp_path, monkeypatch):
    auth, rows = signed_rows()
    core = open_core(tmp_path, writable=True, authenticator=auth)
    ingest(core, rows)
    converge(core)
    def blocked(*args, **kwargs):
        raise AssertionError("hot path performed forbidden I/O/serialization/hash/full scan")
    class NoScan(dict):
        values = blocked
        items = blocked
        __iter__ = blocked
    core._records = NoScan(core._records)
    with monkeypatch.context() as m:
        m.setattr("builtins.open", blocked)
        m.setattr("json.dumps", blocked)
        m.setattr("hashlib.sha256", blocked)
        assert core.judge("compiler", rows[-1].event_id)["eligible_for_bounded_world_write"]
        assert core.recall("compiler")["candidate_count"] == 32
    core.close()


def test_failed_convergence_cannot_commit(tmp_path):
    auth, rows = signed_rows()
    core = open_core(tmp_path, writable=True, authenticator=auth)
    ingest(core, rows)
    assert converge(core, max_rounds=1)["status"] == "vrs_pending"
    assert not core.judge("compiler", rows[-1].event_id)["eligible_for_bounded_world_write"]
    core.close()


def test_new_conflict_invalidates_world_without_deleting_experience(tmp_path):
    auth, rows = signed_rows()
    core = open_core(tmp_path, writable=True, authenticator=auth)
    ingest(core, rows)
    converge(core)
    assert core.commit_judgment("compiler", rows[-1].event_id, request_id="commit", expected_revision=2)["world_committed"]
    ingest(core, [row("failure-new", outcome="failure")], "new")
    assert not core.world()["claim_authority_current"]
    assert core.recall("compiler")["candidate_count"] == 33
    assert not core.judge("compiler", rows[-1].event_id)["eligible_for_bounded_world_write"]
    core.close()


def test_auth_and_supersession_rejection(tmp_path):
    with pytest.raises(ValueError):
        Producer(key_hex="0" * 65, source_family="f", allowed_axes=AXES)
    core = open_core(tmp_path, writable=True)
    ingest(core, [row()])
    with pytest.raises(CoreError, match="supersession"):
        ingest(core, [row("new", supersedes=["e0"])], "new")
    assert core.status()["episode_count"] == 1
    core.close()


def test_expiry_revokes_authority_without_io_until_explicit_refresh(tmp_path):
    auth, rows = signed_rows()
    adjusted = []
    for i, r in enumerate(rows):
        r = r.model_copy(update={"expires_at_ns": NOW + 100})
        key = hashlib.sha256(f"synthetic-producer-{i}".encode()).digest()
        adjusted.append(r.model_copy(update={"signature": sign_observation(r, key)}))
    clock = [NOW]
    core = StatefulCore(tmp_path, writable=True, authenticator=auth, clock=lambda: clock[0])
    ingest(core, adjusted)
    converge(core)
    assert core.judge("compiler", rows[-1].event_id)["eligible_for_bounded_world_write"]
    clock[0] += 100
    assert not core.judge("compiler", rows[-1].event_id)["eligible_for_bounded_world_write"]
    converge(core, "expired-refresh")
    assert all(v < 1 for v in core._graph["strengths"].values())
    assert core.recall("compiler")["candidate_count"] == 32
    core.close()


def test_unrelated_claim_retained_and_signed_refutation_blocks(tmp_path):
    auth, first = signed_rows("compiler")
    _, second = signed_rows("renderer")
    core = open_core(tmp_path, writable=True, authenticator=auth)
    ingest(core, first + second)
    converge(core)
    for h, rows in [("compiler", first), ("renderer", second)]:
        assert core.commit_judgment(h, rows[-1].event_id, request_id=h,
            expected_revision=core.revision)["world_committed"]
    negative = row("refutation", producer="p0", outcome="negative")
    key = hashlib.sha256(b"synthetic-producer-0").digest()
    negative = negative.model_copy(update={"signature": sign_observation(negative, key)})
    ingest(core, [negative], "refute")
    assert core.world()["claim_authority_current"] == {"renderer": True}
    converge(core, "refute-vrs")
    assert not core.judge("compiler", first[-1].event_id)["eligible_for_bounded_world_write"]
    assert core.judge("renderer", second[-1].event_id)["eligible_for_bounded_world_write"]
    assert core._graph["active_outcomes"]["negative"] == 1
    assert "refutation" in core._graph["strengths"]
    core.close()


def test_successful_supersession_keeps_prior_experience(tmp_path):
    auth, rows = signed_rows()
    core = open_core(tmp_path, writable=True, authenticator=auth)
    ingest(core, rows)
    changed = rows[0].model_copy(update={"event_id": "correction", "supersedes": [rows[0].event_id]})
    key = hashlib.sha256(b"synthetic-producer-0").digest()
    changed = changed.model_copy(update={"signature": sign_observation(changed, key)})
    ingest(core, [changed], "correction")
    converge(core)
    assert core._graph["strengths"][rows[0].event_id] < 1
    assert core._graph["strengths"]["correction"] >= 1
    assert core.recall("compiler")["candidate_count"] == 33
    core.close()


def test_repeated_context_does_not_inflate_independent_support(tmp_path):
    auth, rows = signed_rows()
    repeated = []
    for i in range(32):
        event = row(f"repeat-{i}", producer="p0", axis=AXES[i % 4]).model_copy(update={"context_id": "same-context"})
        key = hashlib.sha256(b"synthetic-producer-0").digest()
        repeated.append(event.model_copy(update={"signature": sign_observation(event, key)}))
    core = open_core(tmp_path, writable=True, authenticator=auth)
    ingest(core, repeated)
    converge(core)
    judged = core.judge("compiler", repeated[-1].event_id)
    assert not judged["eligible_for_bounded_world_write"]
    assert judged["accumulator"]["effective_sample_size"] <= 4
    core.close()
