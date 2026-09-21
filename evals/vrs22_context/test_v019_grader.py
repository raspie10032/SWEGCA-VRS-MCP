"""Self checks for the frozen v019 scoring rules."""
import importlib.util
from pathlib import Path
import shutil

import pytest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "v019_grader", ROOT / "tools/summarize_vrs22_compaction_stress_v019.py")
GRADER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GRADER)
RUNNER_SPEC = importlib.util.spec_from_file_location(
    "v019_runner", ROOT / "tools/run_vrs22_compaction_stress_v019.py")
RUNNER = importlib.util.module_from_spec(RUNNER_SPEC)
RUNNER_SPEC.loader.exec_module(RUNNER)


FULL = {
    "purpose": ("Reject duplicate ZIP member names before reading metadata or arrays, "
                "including duplicate metadata.json, in src/swegca_vrs2/checkpoint.py."),
    "constraints": ("Raise ValueError('checkpoint_integrity_failed'); allow valid archives "
                    "and unrelated extra ZIP members; preserve identity, sequence, pair, "
                    "array and journal integrity checks."),
    "superseded_decision": ("The older proposal to reject all archives with any additional "
                            "member was superseded."),
}


def test_full_answer_scores_each_headline_context_axis_at_100():
    boundary = [{"boundary": 1, "answer": FULL, "raw": "valid", "usage": {}}]
    scored = GRADER.score_boundary_answers(boundary)
    assert scored["goal_consistency"]["mean"] == 100
    assert scored["dialogue_context_consistency"]["mean"] == 100


def test_wrong_superseded_decision_is_not_repaired_by_other_fields():
    answer = dict(FULL, superseded_decision="The request to edit was superseded.")
    scored = GRADER.score_boundary_answers(
        [{"boundary": 1, "answer": answer, "raw": "wrong revision", "usage": {}}])
    assert scored["goal_consistency"]["mean"] == 80
    assert scored["goal_consistency"]["first_missing_item"] \
        == "superseded_reject_all_extras"
    assert scored["dialogue_context_consistency"]["mean"] < 100


def test_explicitly_reversed_requirements_cannot_score_as_retained_goal():
    reversed_answer = {
        "purpose": ("Do not reject duplicate ZIP member names before reading metadata "
                    "or arrays, including duplicate metadata.json, in "
                    "src/swegca_vrs2/checkpoint.py."),
        "constraints": ("Do not raise ValueError('checkpoint_integrity_failed'). "
                        "Do not allow valid archives or unrelated extra ZIP members; "
                        "preserve identity, sequence, pair, array and journal checks."),
        "superseded_decision": ("The older proposal to reject all archives with "
                                "any extra member was not superseded."),
    }
    scored = GRADER.score_boundary_answers([
        {"boundary": 1, "answer": reversed_answer,
         "raw": str(reversed_answer), "usage": {}}])
    goal = scored["goal_consistency"]["boundaries"][0]
    assert goal["score"] <= 20
    assert set(goal["explicit_conflicts"]) == {
        "duplicate_before_reads", "exact_diagnostic",
        "allow_valid_unrelated_extras", "superseded_reject_all_extras"}
    context = scored["dialogue_context_consistency"]["boundaries"][0]
    assert context["contradiction"] is True
    assert context["score"] < 100


def test_unsupported_reappearance_is_recorded_as_instability():
    missing = dict(FULL, superseded_decision="The request to edit was superseded.")
    rows = [{"boundary": 1, "answer": FULL, "raw": "full", "usage": {}},
            {"boundary": 2, "answer": missing, "raw": "lost", "usage": {}},
            {"boundary": 3, "answer": FULL, "raw": "reappeared", "usage": {}}]
    scored = GRADER.score_boundary_answers(rows)["dialogue_context_consistency"]
    assert scored["unsupported_reappearance_count"] == 1
    assert scored["boundaries"][2]["temporal_stability"] == 0


def test_code_quality_requires_all_executable_gates_and_target_only_scope():
    source = '''
def decode(body):
    with zipfile.ZipFile(BytesIO(body)) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("checkpoint_integrity_failed")
        archive.read("metadata.json")
'''
    scored = GRADER.score_code_quality(
        source, paths=[GRADER.TARGET], hidden_exit=0, array_exit=0,
        regression_exit=0, diff_bytes=300)
    assert scored["score"] == 20
    failed = GRADER.score_code_quality(
        source, paths=[GRADER.TARGET, "unrelated.py"], hidden_exit=0, array_exit=1,
        regression_exit=0, diff_bytes=300)
    assert failed["score"] < 20


def test_code_quality_uses_documented_partial_credit():
    source = '''
def decode(body):
    with zipfile.ZipFile(BytesIO(body)) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("checkpoint_integrity_failed")
        archive.read("metadata.json")
'''
    scored = GRADER.score_code_quality(
        source, paths=[GRADER.TARGET], hidden_exit=0, array_exit=1,
        regression_exit=0, diff_bytes=300)
    assert any(item["score"] == 2 for item in scored["items"])
    assert 0 < scored["score"] < 20


def exact_protocol_calls():
    address = "memory:" + "a" * 64
    session = "session-1"
    request = "vrs-request-1"
    view = "view-1"
    common = {"turn": "boundary01", "status": "completed", "page_size": None,
              "session_id": session, "query": None, "exact_episode_id": None,
              "memory_layer": None, "fallback_used": None, "lookup_receipt": None,
              "four_stage_valid": False, "stage_query_values": {}, "episode_ids": [],
              "experience_deleted": None, "request_id": None, "view_id": None,
              "packet_request_id": None, "packet_view_id": None,
              "pair_snapshot_id": None, "expected_pair_snapshot_id": None}
    status = dict(common, tool="memory_status", packet_status="ready",
                  pair_snapshot_id="snapshot-1")
    context = dict(common, tool="memory_context", packet_status="memory_context_ready",
        request_id=request, query=address, exact_episode_id=address,
        expected_pair_snapshot_id="snapshot-1", packet_request_id=request,
        packet_view_id=view, pair_snapshot_id="snapshot-1", memory_layer="session",
        fallback_used=False, four_stage_valid=True,
        stage_query_values={stage: address for stage in
            ("deja_vu", "recall", "replay", "re_evidence")},
        episode_ids=[address], lookup_receipt={
            "invariant": "session_first_main_only_after_complete_miss",
            "query": address, "session_candidate_count": 1, "main_opened": False,
            "selected_layer": "session"})
    release = dict(common, tool="memory_release", packet_status="released",
                   request_id=request, view_id=view, packet_request_id=request,
                   packet_view_id=view, experience_deleted=False)
    return address, session, [status, context, release]


def test_vrs_protocol_requires_both_full_exact_address_fields_and_same_handle():
    address, session, calls = exact_protocol_calls()
    assert GRADER.vrs_protocol(calls, address, session)
    missing = [dict(call) for call in calls]
    missing[1]["exact_episode_id"] = None
    assert not GRADER.vrs_protocol(missing, address, session)
    stripped = [dict(call) for call in calls]
    stripped[1]["query"] = address.removeprefix("memory:")
    assert not GRADER.vrs_protocol(stripped, address, session)
    oversized_page = [dict(call) for call in calls]
    oversized_page[1]["page_size"] = 50
    assert not GRADER.vrs_protocol(oversized_page, address, session)
    wrong_release = [dict(call) for call in calls]
    wrong_release[2]["view_id"] = "other-view"
    assert not GRADER.vrs_protocol(wrong_release, address, session)


def test_vrs_protocol_rejects_main_fallback_and_duplicate_status_calls():
    address, session, calls = exact_protocol_calls()
    fallback = [dict(call) for call in calls]
    fallback[1]["fallback_used"] = True
    fallback[1]["memory_layer"] = "main"
    fallback[1]["lookup_receipt"] = dict(fallback[1]["lookup_receipt"], main_opened=True)
    assert not GRADER.vrs_protocol(fallback, address, session)
    duplicate_status = [calls[0], dict(calls[0]), *calls[1:]]
    assert not GRADER.vrs_protocol(duplicate_status, address, session)


def test_live_experience_counts_observations_separately_from_state_transitions():
    evidence = {
        "coverage_accounted": True,
        "transcript_lines": 12,
        "captured_records": 10,
        "explicitly_excluded_records": 2,
        "session_journal_sequence": 13,
        "session_state_transition_parts": 3,
        "stored_parts": 10,
        "receipt_coverage": 10,
        "session_ended": False,
        "active_session_main_untouched": True,
        "merged_experience_parts": 0,
        "main_journal_sequence": 0,
        "main_state_transition_parts": 0,
        "main_logical_experience_parts": 0,
        "database_artifacts": [],
        "session_experience_parts": 10,
    }
    assert GRADER.live_experience_complete([{"live_experience": evidence}])
    wrong = dict(evidence, session_journal_sequence=12)
    assert not GRADER.live_experience_complete([{"live_experience": wrong}])


def test_changed_paths_catches_new_nested_files_and_missing_fixture_files(tmp_path):
    workspace = tmp_path / "workspace"
    shutil.copytree(GRADER.BASELINE, workspace)
    assert GRADER.changed_paths(workspace) == []
    (workspace / "src" / "unexpected").mkdir()
    (workspace / "src" / "unexpected" / "payload.py").write_text("x = 1\n")
    (workspace / "src" / "unexpected" / "external").symlink_to(tmp_path, target_is_directory=True)
    (workspace / "tests" / "standalone" / "test_checkpoint.py").unlink()
    assert GRADER.changed_paths(workspace) == [
        "src/unexpected/external", "src/unexpected/payload.py",
        "tests/standalone/test_checkpoint.py"]


def test_model_mounts_hide_resident_experience_and_frozen_controls(tmp_path):
    workspace = tmp_path / "cell-workspace"
    codex_home = tmp_path / "cell-codex-home"
    wrapper = RUNNER.command("gpt-5.6-luna", "short_vrs", workspace,
                             codex_home, tmp_path / "guard", "start", None)
    mounts = list(zip(wrapper, wrapper[1:]))
    assert ("--tmpfs", str(RUNNER.LIVE_STATE)) in mounts
    assert ("--tmpfs", str(RUNNER.PRIVATE_EVAL_ROOT)) in mounts
    assert ("--tmpfs", str(RUNNER.USER_RUNTIME_DIR)) in mounts
    assert "--unshare-pid" in wrapper


def test_executable_grading_cannot_read_host_experience_or_control_answers(tmp_path):
    workspace = tmp_path / "grading-workspace"
    tests = workspace / "tests"
    tests.mkdir(parents=True)
    probe = tests / "test_isolation.py"
    probe.write_text('''from pathlib import Path
import os

def test_grading_view():
    assert not Path("/home/raspie/.codex/auth.json").exists()
    for directory in ("/home/raspie/.local/share/swegca-vrs2-codex",
                      "/home/raspie/.local/share/vrs22-eval-runtime-v019/private"):
        try:
            assert list(Path(directory).iterdir()) == []
        except PermissionError:
            pass
    assert list((Path("/run/user") / str(os.getuid())).iterdir()) == []
    assert not Path(__file__).parents[2].joinpath("sibling-secret.txt").exists()
    assert "CODEX_THREAD_ID" not in os.environ
''', encoding="utf-8")
    (tmp_path / "sibling-secret.txt").write_text("hidden", encoding="utf-8")
    output = tmp_path / "grading-output"
    output.mkdir()
    result = GRADER.run_test(workspace, probe, GRADER.grade_guard(output))
    assert result.returncode == 0, result.stdout + result.stderr


def test_live_handoff_gate_requires_the_armed_session_end():
    receipt = {"schema": "swegca-vrs2-final-handoff-v1", "status": "PASS",
               "armed_ns": RUNNER.LIVE_HANDOFF_ARMED_NS,
               "ended": {"host": "codex", "session": RUNNER.LIVE_HANDOFF_SESSION_KEY,
                         "merged": True,
                         "ended_ns": RUNNER.LIVE_HANDOFF_ARMED_NS + 1}}
    assert RUNNER.handoff_receipt_matches(receipt)
    assert not RUNNER.handoff_receipt_matches(dict(receipt, armed_ns=0))
    assert not RUNNER.handoff_receipt_matches(dict(
        receipt, ended=dict(receipt["ended"], session="other-session")))
    assert not RUNNER.handoff_receipt_matches(dict(
        receipt, ended=dict(receipt["ended"], merged=False)))
    assert not RUNNER.handoff_receipt_matches(dict(
        receipt, ended=dict(receipt["ended"], ended_ns=RUNNER.LIVE_HANDOFF_ARMED_NS - 1)))


def test_runner_finalizes_a_started_session_after_first_call_fails(tmp_path, monkeypatch):
    actions = []

    def setup(workspace, codex_home, _arm, _main_seed):
        workspace.mkdir()
        transcript = codex_home / "sessions" / "rollout.jsonl"
        transcript.parent.mkdir(parents=True)
        transcript.write_text('{"type":"session_meta","payload":{"id":"session-a"}}\n')

    def failed_call(*_args, **_kwargs):
        raise RuntimeError("simulated_first_call_failure")

    monkeypatch.setattr(RUNNER, "setup_workspace", setup)
    monkeypatch.setattr(RUNNER, "start_watcher", lambda *_args: object())
    monkeypatch.setattr(RUNNER, "call", failed_call)
    monkeypatch.setattr(RUNNER, "stop_watcher_process",
                        lambda *_args: actions.append("watcher_stopped") or {"status": "stopped"})
    monkeypatch.setattr(RUNNER, "finalize_experience",
                        lambda *_args, **_kwargs: actions.append("session_finalized") or {"ended_count": 1})
    monkeypatch.setattr(RUNNER, "stop_residents",
                        lambda *_args: actions.append("residents_stopped"))
    with pytest.raises(RuntimeError, match="simulated_first_call_failure"):
        RUNNER.run_one("gpt-5.6-luna", "short_vrs", tmp_path, 10, 20)
    assert actions == ["watcher_stopped", "session_finalized", "residents_stopped"]
    receipt = tmp_path / "gpt-5.6-luna__short_vrs.cleanup.json"
    assert receipt.read_text().find('"ended_count": 1') >= 0
