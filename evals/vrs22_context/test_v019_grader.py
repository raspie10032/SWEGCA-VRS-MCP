"""Self checks for the frozen v019 scoring rules."""
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "v019_grader", ROOT / "tools/summarize_vrs22_compaction_stress_v019.py")
GRADER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GRADER)


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
