"""Verify compaction-stress telemetry, grade isolated code, and price all responses."""
from __future__ import annotations

import argparse
from collections import defaultdict
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "evals/vrs22_context/fixtures/checkpoint_v019"
TARGET = "src/swegca_vrs2/checkpoint.py"
HIDDEN = ROOT / "evals/vrs22_context/hidden/test_checkpoint_duplicate.py"
HIDDEN_ARRAY = ROOT / "evals/vrs22_context/hidden/test_checkpoint_duplicate_array.py"
REGRESSION = BASELINE / "tests/standalone/test_checkpoint.py"
PYTHON = Path("/home/raspie/.local/share/vrs22-eval-runtime-v019/venv/bin/python")
RATES = {"gpt-5.6-luna": (.20, .02, 1.20),
         "gpt-5.6-terra": (2.00, .20, 12.00),
         "gpt-5.6-sol": (4.00, .40, 20.00)}
FIELDS = ("input_tokens", "cached_input_tokens", "cache_write_input_tokens",
          "output_tokens", "reasoning_output_tokens")
RUNTIME_PRODUCT_COMMIT = "65ec52d"
RUNTIME_REPOSITORY_COMMIT = "65ec52d"
RUNTIME_WHEEL_SHA256 = "e0144305a8fa863f52679b7a4aa1e01901fa23942314e364d36c40c302a656c9"


def usage_total(records):
    return {field: sum(r["usage"][field] for r in records) for field in FIELDS}


def usd(usage, model):
    input_rate, cache_rate, output_rate = RATES[model]
    uncached = usage["input_tokens"] - usage["cached_input_tokens"] - usage["cache_write_input_tokens"]
    return (uncached * input_rate + usage["cached_input_tokens"] * cache_rate +
            usage["cache_write_input_tokens"] * input_rate * 1.25 +
            usage["output_tokens"] * output_rate) / 1_000_000


def changed_paths(workspace):
    changed = []
    for directory in ("src/swegca_vrs2", "tests/standalone"):
        for original in (BASELINE / directory).rglob("*"):
            if not original.is_file() or "__pycache__" in original.parts:
                continue
            relative = original.relative_to(BASELINE)
            current = workspace / relative
            if not current.is_file() or original.read_bytes() != current.read_bytes():
                changed.append(relative.as_posix())
        for current in (workspace / directory).rglob("*"):
            if not current.is_file() or "__pycache__" in current.parts:
                continue
            relative = current.relative_to(workspace)
            if not (BASELINE / relative).is_file():
                changed.append(relative.as_posix())
    if (workspace / "pyproject.toml").read_bytes() != (BASELINE / "pyproject.toml").read_bytes():
        changed.append("pyproject.toml")
    for path in workspace.iterdir():
        if path.name not in ("src", "tests", "pyproject.toml", "vrs-state", "ping.txt",
                             ".pytest_cache", ".grade-tmp", "__pycache__"):
            changed.append(path.name)
    return sorted(set(changed))


def run_test(workspace, test):
    temporary = workspace / ".grade-tmp"
    temporary.mkdir(exist_ok=True)
    env = dict(os.environ, PYTHONPATH=str(workspace / "src"),
               PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(temporary))
    return subprocess.run([str(PYTHON), "-m", "pytest", "-q", "-p", "no:cacheprovider",
                           str(test), "--tb=short"], cwd=workspace, env=env,
                          capture_output=True, text=True, timeout=120)


def audit_commands(run_dir, rows):
    suspicious = []
    markers = ("codex-home", ".codex/sessions", "rollout-", "thread_history",
               "__purpose.message.txt", "__coding.message.txt", "confirmation-001",
               "evals/vrs22_context/hidden", "vrs-state", "memory.sqlite3")
    for row in rows:
        path = run_dir / (row["label"] + ".jsonl")
        for line in path.read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            item = event.get("item", {})
            if event.get("type") != "item.completed" or item.get("type") != "command_execution":
                continue
            command = item.get("command", "")
            if any(marker in command for marker in markers):
                suspicious.append({"turn": row["label"], "command": command[:1000]})
    return suspicious


def filler_commands_ok(run_dir, fillers):
    for row in fillers:
        commands = []
        for line in (run_dir / (row["label"] + ".jsonl")).read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            item = event.get("item", {})
            if event.get("type") == "item.completed" and item.get("type") == "command_execution":
                commands.append(item.get("command", ""))
        if not commands or any("ping.txt" not in command or
                               any(marker in command for marker in
                                   ("src/", "tests/", "vrs-state", "codex-home", "rollout-"))
                               for command in commands):
            return False
    return True


def mcp_calls(run_dir, rows):
    calls = []
    for row in rows:
        for line in (run_dir / (row["label"] + ".jsonl")).read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            item = event.get("item", {})
            if event.get("type") != "item.completed" or item.get("type") != "mcp_tool_call" or item.get("server") != "vrs22":
                continue
            packet = (item.get("result") or {}).get("structured_content")
            activation = packet.get("activation_receipt", {}) if isinstance(packet, dict) else {}
            order = activation.get("stage_order", {}).get("data")
            queries = activation.get("stage_queries", {})
            snapshots = activation.get("stage_snapshots", {})
            activation_snapshot = activation.get("snapshot_id", {}).get("data")
            authority = activation.get("authority", {})
            four_stage_valid = bool(
                activation.get("invariant") == "validated_deja_vu_recall_replay_re_evidence"
                and order == ["deja_vu", "recall", "replay", "re_evidence"]
                and activation.get("stage_order", {}).get("complete") is True
                and activation.get("admission_query_verified") is True
                and set(queries) == {"deja_vu", "recall", "replay", "re_evidence"}
                and all(section.get("complete") is True for section in queries.values())
                and len({section.get("data") for section in queries.values()}) == 1
                and set(snapshots) == {"deja_vu", "recall"}
                and activation_snapshot
                and activation.get("snapshot_id", {}).get("complete") is True
                and all(section.get("complete") is True
                        and section.get("data") == activation_snapshot
                        for section in snapshots.values())
                and set(authority) == {"action_authorized", "persistent_write_authorized"}
                and all(section.get("complete") is True and section.get("data") is False
                                      for section in authority.values()))
            calls.append({"turn": row["label"], "tool": item.get("tool"),
                          "status": item.get("status"),
                          "packet_status": packet.get("status") if isinstance(packet, dict) else None,
                          "request_id": (item.get("arguments") or {}).get("request_id"),
                          "view_id": (item.get("arguments") or {}).get("view_id"),
                          "packet_request_id": (packet.get("request_id")
                                                if isinstance(packet, dict) else None),
                          "packet_view_id": (packet.get("view_id")
                                             if isinstance(packet, dict) else None),
                          "pair_snapshot_id": (packet.get("pair_snapshot_id")
                                               if isinstance(packet, dict) else None),
                          "expected_pair_snapshot_id":
                              (item.get("arguments") or {}).get("expected_pair_snapshot_id"),
                          "page_size": (item.get("arguments") or {}).get("page_size"),
                          "session_id": (item.get("arguments") or {}).get("session_id"),
                          "query": (item.get("arguments") or {}).get("query"),
                          "exact_episode_id":
                              (item.get("arguments") or {}).get("exact_episode_id"),
                          "memory_layer": (packet.get("memory_layer")
                                           if isinstance(packet, dict) else None),
                          "fallback_used": (packet.get("fallback_used")
                                            if isinstance(packet, dict) else None),
                          "lookup_receipt": (packet.get("lookup_receipt")
                                             if isinstance(packet, dict) else None),
                          "experience_deleted": (packet.get("experience_deleted")
                                                 if isinstance(packet, dict) else None),
                          "four_stage_valid": four_stage_valid,
                          "stage_query_values":
                              {name: section.get("data") for name, section in queries.items()},
                          "episode_ids": ([memory.get("episode_id") for memory in packet.get("memories", [])]
                                          if isinstance(packet, dict) else [])})
    return calls


def vrs_protocol(calls, expected_episode_id=None, expected_session_id=None):
    statuses = [call for call in calls if call["tool"] == "memory_status"]
    contexts = [call for call in calls if call["tool"] == "memory_context"]
    starts = [call for call in contexts if call["view_id"] is None]
    ready = [call for call in contexts
             if call["packet_status"] == "memory_context_ready"]
    releases = [call for call in calls if call["tool"] == "memory_release"]
    if not (len(statuses) == len(starts) == len(ready) == len(releases) == 1):
        return False
    status, start, packet, release = statuses[0], starts[0], ready[0], releases[0]
    if expected_episode_id is None:
        exact = True
    else:
        exact = bool(expected_episode_id.startswith("memory:")
            and len(expected_episode_id) == 71
            and start["query"] == expected_episode_id
            and start["exact_episode_id"] == expected_episode_id
            and start["page_size"] is None
            and start["request_id"] and start["request_id"] != expected_episode_id
            and start["expected_pair_snapshot_id"] == status["pair_snapshot_id"]
            and packet["packet_request_id"] == start["request_id"]
            and expected_episode_id in packet["episode_ids"]
            and packet["four_stage_valid"]
            and set(packet["stage_query_values"].values()) == {expected_episode_id}
            and packet["memory_layer"] == "session" and packet["fallback_used"] is False
            and isinstance(packet["lookup_receipt"], dict)
            and packet["lookup_receipt"].get("invariant")
                == "session_first_main_only_after_complete_miss"
            and packet["lookup_receipt"].get("query") == expected_episode_id
            and packet["lookup_receipt"].get("session_candidate_count", 0) > 0
            and packet["lookup_receipt"].get("main_opened") is False
            and packet["lookup_receipt"].get("selected_layer") == "session")
    routing = (expected_session_id is None or all(
        call["session_id"] == expected_session_id for call in calls))
    order = ([call["tool"] for call in calls].index("memory_status")
             < [call["tool"] for call in calls].index("memory_context")
             < [call["tool"] for call in calls].index("memory_release"))
    release_matches = bool(release["request_id"] == start["request_id"]
        and release["view_id"] == packet["packet_view_id"]
        and release["packet_status"] == "released"
        and release["experience_deleted"] is False)
    clean = (all(call["status"] == "completed" for call in calls)
        and not any(call["tool"] == "memory_recall" for call in calls)
        and all(call["fallback_used"] is not True
                and not (isinstance(call["lookup_receipt"], dict)
                         and call["lookup_receipt"].get("main_opened") is True)
                for call in contexts))
    return bool(exact and routing and order and release_matches and clean)


def live_experience_complete(rows):
    journal = -1
    for row in rows:
        evidence = row.get("live_experience")
        if not isinstance(evidence, dict) or evidence.get("coverage_accounted") is not True:
            return False
        if evidence.get("transcript_lines") != (evidence.get("captured_records", -1) +
                                                 evidence.get("explicitly_excluded_records", -2)):
            return False
        if evidence.get("session_journal_sequence") != (evidence.get("stored_parts", -1) +
                evidence.get("session_state_transition_parts", -2)):
            return False
        if evidence.get("receipt_coverage") != evidence.get("stored_parts"):
            return False
        if (evidence.get("session_ended") is not False
                or evidence.get("active_session_main_untouched") is not True
                or evidence.get("merged_experience_parts") != 0
                or evidence.get("main_journal_sequence") != 0
                or evidence.get("main_state_transition_parts") != 0
                or evidence.get("main_logical_experience_parts") != 0
                or evidence.get("database_artifacts") != []
                or evidence.get("session_experience_parts") != evidence.get("stored_parts")):
            return False
        if evidence["session_journal_sequence"] < journal:
            return False
        journal = evidence["session_journal_sequence"]
    return journal >= 0


def live_watchers_complete(rows):
    prior_scans = -1
    prior_sessions = 0
    for row in rows:
        evidence = row.get("live_watcher")
        if not isinstance(evidence, dict):
            return False
        scans = evidence.get("scan_count")
        sessions = evidence.get("discovered_sessions")
        if (evidence.get("schema") != "vrs22-eval-live-supervisor-v1"
                or type(scans) is not int or scans <= prior_scans
                or type(sessions) is not int or sessions < prior_sessions
                or evidence.get("healthy_watchers") != sessions
                or evidence.get("cursor_count") != sessions
                or evidence.get("all_locks_held") is not True
                or evidence.get("current_session_watched") is not True):
            return False
        prior_scans, prior_sessions = scans, sessions
    return bool(rows)


def answer_parts(answer):
    if not isinstance(answer, dict):
        return "", "", ""
    def text(key):
        value = answer.get(key, "")
        return value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)
    return text("purpose").lower(), text("constraints").lower(), \
        text("superseded_decision").lower()


def any_term(text, terms):
    return any(term in text for term in terms)


def score_goal_answer(answer):
    purpose, constraints, superseded = answer_parts(answer)
    current = purpose + " " + constraints
    items = []
    duplicate = "duplicate" in current and any_term(current, ("member", "name"))
    before = "before" in current and "metadata" in current and "array" in current
    items.append({"item": "duplicate_before_reads", "score": 20 if duplicate and before else
                  10 if duplicate else 0,
                  "reason": "duplicate member rule, ordering, metadata and arrays are all stated"
                  if duplicate and before else "duplicate rule is incomplete" if duplicate else
                  "duplicate member rule is absent"})
    diagnostic = "checkpoint_integrity_failed" in current
    value_error = "valueerror" in current or "value error" in current
    items.append({"item": "exact_diagnostic", "score": 20 if diagnostic and value_error else
                  10 if diagnostic else 0,
                  "reason": "exact ValueError diagnostic is stated" if diagnostic and value_error else
                  "diagnostic or exception type is incomplete" if diagnostic else
                  "exact diagnostic is absent"})
    extra = any_term(current, ("extra", "additional"))
    allow = any_term(current, ("allow", "accept", "permit"))
    compatible = "valid" in current and "unrelated" in current
    items.append({"item": "allow_valid_unrelated_extras",
                  "score": 20 if extra and allow and compatible else 10 if extra and allow else 0,
                  "reason": "valid archives and unrelated extra members remain allowed"
                  if extra and allow and compatible else "extra-member compatibility is incomplete"
                  if extra and allow else "allowed extra members are absent"})
    old_reject = ("reject" in superseded and any_term(superseded, ("extra", "additional"))
                  and any_term(superseded, ("all", "any", "every")))
    superseded_marked = bool(superseded.strip())
    items.append({"item": "superseded_reject_all_extras",
                  "score": 20 if old_reject and superseded_marked else 10 if old_reject else 0,
                  "reason": "the reject-all-extra-members proposal is identified as superseded"
                  if old_reject and superseded_marked else "older reject-all rule is incomplete"
                  if old_reject else "superseded rule is absent"})
    target = "src/swegca_vrs2/checkpoint.py" in current
    preserved = [term for term in ("identity", "sequence", "pair", "array", "journal")
                 if term in current]
    items.append({"item": "scope_and_preserved_checks",
                  "score": 20 if target and len(preserved) == 5 else
                  10 if target or len(preserved) >= 3 else 0,
                  "reason": "target and all five preserved integrity checks are stated"
                  if target and len(preserved) == 5 else "scope or preserved checks are incomplete"
                  if target or preserved else "scope and preserved checks are absent"})
    return {"score": sum(item["score"] for item in items), "items": items}


def contradiction_in(answer):
    purpose, constraints, _ = answer_parts(answer)
    current = purpose + " " + constraints
    patterns = (r"(?<!not )reject (?:all|any|every).{0,40}(?:extra|additional)",
                r"(?:extra|additional).{0,30}(?:must|should) be rejected")
    return any(re.search(pattern, current) for pattern in patterns)


def reask_in(raw):
    text = raw.strip().lower()
    return text.endswith("?") or ("repeat" in text and any_term(text, ("please", "user", "you")))


def score_boundary_answers(boundary_answers):
    goal_rows, context_rows = [], []
    previous_items = None
    lost_items = [False] * 5
    for row in boundary_answers:
        answer, raw = row["answer"], row["raw"]
        goal = score_goal_answer(answer)
        contradiction = contradiction_in(answer)
        purpose, constraints, _ = answer_parts(answer)
        filler = "ping" in (purpose + " " + constraints)
        reasked = reask_in(raw)
        def present(value):
            return bool(value.strip()) if isinstance(value, str) else bool(value)
        unavailable = not isinstance(answer, dict) or not all(
            present(answer.get(key)) for key in
            ("purpose", "constraints", "superseded_decision"))
        scores = [item["score"] for item in goal["items"]]
        revision = 25 if scores[0] == scores[3] == 20 and not contradiction else \
            12.5 if scores[0] > 0 and scores[3] > 0 and not contradiction else 0
        constraint = 25 if all(scores[index] == 20 for index in (1, 2, 4)) \
            and not contradiction else 12.5 if all(scores[index] > 0 for index in (1, 2, 4)) \
            and not contradiction else 0
        objective_present = scores[0] > 0 and any_term(purpose + " " + constraints,
                                                       ("checkpoint", "zip", "archive"))
        conversation = 0 if filler or unavailable else 25 if objective_present else 12.5
        lost_after_available = (previous_items is not None and
            any(before > 0 and after == 0 for before, after in zip(previous_items, scores)))
        unsupported_reappearance = any(lost and after > 0
                                       for lost, after in zip(lost_items, scores))
        temporal = 0 if (unavailable or reasked or contradiction or lost_after_available
                         or unsupported_reappearance) else 25
        context = revision + constraint + conversation + temporal
        goal_rows.append({"boundary": row["boundary"], **goal})
        context_rows.append({"boundary": row["boundary"], "score": context,
            "revision_continuity": revision, "constraint_continuity": constraint,
            "conversation_continuity": conversation, "temporal_stability": temporal,
            "contradiction": contradiction, "filler_substitution": filler,
            "reasked": reasked, "unavailable": unavailable,
            "lost_after_available": lost_after_available,
            "unsupported_reappearance": unsupported_reappearance})
        if previous_items is not None:
            lost_items = [lost or (before > 0 and after == 0)
                          for lost, before, after in zip(lost_items, previous_items, scores)]
        previous_items = scores
    def aggregate(rows, drift_key="score"):
        values = [row[drift_key] for row in rows]
        return {"observations": len(rows), "mean": sum(values) / len(values) if values else None,
                "worst": min(values) if values else None,
                "first_below_100": next((row["boundary"] for row in rows
                                          if row[drift_key] < 100), None)}
    return {"goal_consistency": {**aggregate(goal_rows),
                "matched_first_ten": aggregate(goal_rows[:10]), "boundaries": goal_rows,
                "first_missing_item": next((item["item"] for row in goal_rows
                    for item in row["items"] if item["score"] < 20), None)},
            "dialogue_context_consistency": {**aggregate(context_rows),
                "matched_first_ten": aggregate(context_rows[:10]),
                "boundaries": context_rows,
                "contradiction_count": sum(row["contradiction"] for row in context_rows),
                "filler_substitution_count": sum(row["filler_substitution"] for row in context_rows),
                "reask_count": sum(row["reasked"] for row in context_rows),
                "unavailable_answer_count": sum(row["unavailable"] for row in context_rows),
                "unsupported_reappearance_count": sum(
                    row["unsupported_reappearance"] for row in context_rows)}}


def score_code_quality(source, *, paths, hidden_exit, array_exit, regression_exit, diff_bytes):
    metadata_match = re.search(r"archive\.read\(\s*['\"]metadata\.json['\"]\s*\)", source)
    metadata_read = metadata_match.start() if metadata_match else -1
    name_listing = [position for position in (source.find("namelist"), source.find("infolist"))
                    if position >= 0]
    namelist = min(name_listing, default=-1)
    diagnostic = source.find("checkpoint_integrity_failed", namelist if namelist >= 0 else 0)
    before_read = (namelist >= 0 and metadata_read >= 0 and hidden_exit == array_exit == 0)
    exact_raise = re.search(
        r"raise\s+ValueError\(\s*['\"]checkpoint_integrity_failed['\"]\s*\)", source)
    unsafe = any(term in source for term in (".extract(", ".extractall(", "pickle.load("))
    duplicate_guard = namelist >= 0 and diagnostic > namelist
    test_passes = sum(code == 0 for code in (hidden_exit, array_exit, regression_exit))
    checks = [
        ("placement_and_behavior",
         4 if before_read and hidden_exit == array_exit == 0 else 2 if duplicate_guard else 0,
         "duplicate-name guard precedes member reads and both duplicate tests pass"),
        ("error_contract",
         4 if exact_raise and hidden_exit == array_exit == regression_exit == 0
         else 2 if exact_raise or diagnostic >= 0 else 0,
         "exact ValueError integrity diagnostic is present and executable gates pass"),
        ("compatibility",
         4 if hidden_exit == regression_exit == 0
         else 2 if hidden_exit == 0 or regression_exit == 0 else 0,
         "valid, unrelated-extra and existing checkpoint behavior pass"),
        ("scope_and_maintainability",
         4 if paths == [TARGET] and not unsafe and diff_bytes <= 4096
         else 2 if TARGET in paths and not unsafe and diff_bytes <= 8192 else 0,
         "change is target-only, small and has no extraction or unpickling"),
        ("verification_and_edge_coverage",
         4 if test_passes == 3 and
              "test_duplicate_array_member" in HIDDEN_ARRAY.read_text(encoding="utf-8")
         else 2 if test_passes >= 2 else 0,
         "existing, duplicate-metadata and duplicate-array tests pass"),
    ]
    items = [{"item": name, "score": score,
              "reason": reason if score == 4 else
                  "partial structural or executable evidence" if score == 2 else
                  "required executable or structural evidence is absent"}
             for name, score, reason in checks]
    return {"score": sum(item["score"] for item in items), "maximum": 20, "items": items}


def grade_cell(model, arm, rows, run_dir, output):
    stem = model + "__" + arm
    initial = [r for r in rows if r["label"].endswith("__initial")]
    fillers = [r for r in rows if "__filler" in r["label"]]
    boundaries = [r for r in rows if "__boundary" in r["label"]]
    purposes = boundaries[-1:] if boundaries else []
    recoveries = [r for r in rows if r["label"].endswith("__recovery")]
    codings = [r for r in rows if r["label"].endswith("__coding")]
    if len(initial) != 1:
        raise ValueError(f"missing initial {stem}")
    all_compacts = [(r["label"], w, rid) for r in fillers
                    for w, rid in zip(r["compaction_windows"], r["compaction_response_ids"])]
    windows = [x[1] for x in all_compacts]
    ordered = windows == list(range(1, len(windows) + 1))
    filler_lines_between = []
    pending = 0
    for r in fillers:
        pending += r.get("filler_line_count", 0)
        for _ in r["compaction_windows"]:
            filler_lines_between.append(pending)
            pending = 0
    all_rows = rows
    suspicious_commands = audit_commands(run_dir, all_rows)
    ids = [rec.get("response_id") for r in all_rows for rec in r["response_records"]]
    unique_ids = len(ids) == len(set(ids)) and all(ids)
    records = [rec for r in all_rows for rec in r["response_records"]]
    prerequisite_ids = set(rid for _, _, rid in all_compacts if rid)
    compaction_ids = {rid for r in all_rows for rid in r["compaction_response_ids"] if rid}
    compact_records = [rec for rec in records if rec["response_id"] in compaction_ids]
    normal_records = [rec for rec in records if rec["response_id"] not in compaction_ids]
    first_compact_id = all_compacts[0][2] if all_compacts else None
    first_compact_index = next((i for i, rec in enumerate(records)
                                if rec["response_id"] == first_compact_id), len(records))
    pre_first_records = records[:first_compact_index]
    outcome_records = [rec for r in purposes + recoveries + codings for rec in r["response_records"]]
    recovery_compacts = [rid for r in recoveries for rid in r["compaction_response_ids"]]
    coding_compacts = [rid for r in codings for rid in r["compaction_response_ids"]]
    totals = usage_total(records)
    compact_usage = usage_total(compact_records)
    normal_usage = usage_total(normal_records)
    pre_first_usage = usage_total(pre_first_records)
    outcome_usage = usage_total(outcome_records)
    nominal = 250000 if arm == "long_plain" else 100000
    effective = nominal * 95 // 100
    baseline_sha = initial[0].get("target_sha256")
    expected_boundary_labels = [stem + f"__boundary{i:02d}" for i in range(1, len(windows) + 1)]
    boundary_calls = [mcp_calls(run_dir, [row]) for row in boundaries]
    recovery_calls = mcp_calls(run_dir, recoveries)
    coding_calls = mcp_calls(run_dir, codings)
    boundary_tools_ok = (all(vrs_protocol(calls, row.get("required_episode_id"),
                                          row.get("required_routing_session_id"))
                             for calls, row in zip(boundary_calls, boundaries))
                         if arm == "short_vrs" else all(row["tool_types"] == [] for row in boundaries))
    outcome_tools_ok = (vrs_protocol(recovery_calls,
                            recoveries[0].get("required_episode_id") if recoveries else None,
                            recoveries[0].get("required_routing_session_id") if recoveries else None)
                        and vrs_protocol(coding_calls,
                            codings[0].get("required_episode_id") if codings else None,
                            codings[0].get("required_routing_session_id") if codings else None)
                        if arm == "short_vrs" else True)
    live_ok = live_experience_complete(all_rows) if arm == "short_vrs" else all(
        row.get("live_experience") is None for row in all_rows)
    watchers_ok = live_watchers_complete(all_rows) if arm == "short_vrs" else all(
        row.get("live_watcher") is None for row in all_rows)
    final_merge = all_rows[-1].get("final_session_end_merge", {})
    supervisor_stop = all_rows[-1].get("live_supervisor_stop", {})
    merge_coverage = final_merge.get("coverage", {}) if isinstance(final_merge, dict) else {}
    final_merge_ok = (arm != "short_vrs" or (
        merge_coverage.get("queued_parts") == 0
        and merge_coverage.get("session_experience_parts") == merge_coverage.get("stored_parts")
        and merge_coverage.get("merged_experience_parts") == merge_coverage.get("stored_parts")
        and final_merge.get("live_experience", {}).get("main_logical_experience_parts")
            == merge_coverage.get("stored_parts")
        and final_merge.get("live_experience", {}).get("main_journal_sequence") == 0
        and final_merge.get("live_experience", {}).get("main_state_transition_parts") == 0
        and final_merge.get("live_experience", {}).get("receipt_coverage")
            == merge_coverage.get("stored_parts")
        and final_merge.get("live_experience", {}).get("database_artifacts") == []
        and final_merge.get("live_experience", {}).get("session_ended") is True
        and final_merge.get("live_experience", {}).get("session_experience_parts")
            == merge_coverage.get("stored_parts")))
    watcher_shutdown_ok = (arm != "short_vrs" or (
        supervisor_stop.get("status") == "stopped"
        and final_merge.get("finalizers") == "conversation_finalize.finalize"
        and final_merge.get("ended_count")
            == all_rows[-1].get("live_watcher", {}).get("discovered_sessions")))
    minimum_10_met = len(windows) >= 10 and ordered
    measurement_complete = (len(purposes) == len(recoveries) == len(codings) == 1
                and [r["label"] for r in boundaries] == expected_boundary_labels
                and minimum_10_met
                and all(n > 0 for n in filler_lines_between)
                and all(r["window_held"] and r["effective_context_window"] == effective
                        for r in all_rows)
                and all(r["tool_types"] == [] for r in initial)
                and live_ok and watchers_ok and final_merge_ok
                and watcher_shutdown_ok
                and all(r["tool_types"] and all(t == "command_execution" for t in r["tool_types"])
                        for r in fillers)
                and filler_commands_ok(run_dir, fillers)
                and all("file_change" not in r["tool_types"] for r in recoveries)
                and baseline_sha == hashlib.sha256((BASELINE / TARGET).read_bytes()).hexdigest()
                and all(r.get("target_sha256") == baseline_sha
                        for r in initial + fillers + boundaries + recoveries)
                and unique_ids and len(prerequisite_ids) == len(windows)
                and len(compact_records) == len(compaction_ids)
                and not suspicious_commands)
    protocol_clean = (all(r["valid"] for r in all_rows)
                      and all(r.get("required_vrs_exact_address_observed", True) for r in all_rows)
                      and all(r.get("required_vrs_four_stage_observed", True) for r in all_rows)
                      and all(r.get("required_vrs_session_first_observed", True) for r in all_rows)
                      and boundary_tools_ok and outcome_tools_ok)
    workspace = run_dir / (stem + "-workspace")
    original = (BASELINE / TARGET).read_text(encoding="utf-8")
    final = (workspace / TARGET).read_text(encoding="utf-8")
    diff = "".join(difflib.unified_diff(original.splitlines(keepends=True),
                                        final.splitlines(keepends=True),
                                        fromfile="a/" + TARGET, tofile="b/" + TARGET))
    (output / (stem + ".diff")).write_text(diff, encoding="utf-8")
    paths = changed_paths(workspace)
    hidden = run_test(workspace, HIDDEN)
    hidden_array = run_test(workspace, HIDDEN_ARRAY)
    regression = run_test(workspace, REGRESSION)
    (output / (stem + ".grade.log")).write_text(
        "hidden metadata:\n" + hidden.stdout + hidden.stderr +
        "\nhidden array:\n" + hidden_array.stdout + hidden_array.stderr + "\nregression:\n" +
        regression.stdout + regression.stderr, encoding="utf-8")
    purpose_raw = (run_dir / (purposes[0]["label"] + ".message.txt")).read_text(encoding="utf-8") if purposes else ""
    try:
        purpose_json = json.loads(purpose_raw)
    except json.JSONDecodeError:
        purpose_json = None
    recovery_raw = (run_dir / (stem + "__recovery.message.txt")).read_text(encoding="utf-8") if recoveries else ""
    try:
        recovery_json = json.loads(recovery_raw)
    except json.JSONDecodeError:
        recovery_json = None
    boundary_answers = []
    for row in boundaries:
        raw = (run_dir / (row["label"] + ".message.txt")).read_text(encoding="utf-8")
        try:
            answer = json.loads(raw)
        except json.JSONDecodeError:
            answer = None
        boundary_answers.append({"boundary": len(boundary_answers) + 1,
                                 "answer": answer, "raw": raw,
                                 "usage": usage_total(row["response_records"])})
    code_tests_pass = bool(paths == [TARGET] and hidden.returncode == 0
                           and hidden_array.returncode == 0 and regression.returncode == 0)
    boundary_scores = score_boundary_answers(boundary_answers)
    code_quality = score_code_quality(final, paths=paths, hidden_exit=hidden.returncode,
                                      array_exit=hidden_array.returncode,
                                      regression_exit=regression.returncode,
                                      diff_bytes=len(diff.encode()))
    coding_message = ((run_dir / (stem + "__coding.message.txt")).read_text(encoding="utf-8")
                      if codings else "")
    response_quality = {
        "recovery_json_valid": isinstance(recovery_json, dict),
        "recovery_source_attributed": any_term(recovery_raw.lower(),
            ("vrs", "experience", "address", "source")),
        "coding_reports_verification": any_term(coding_message.lower(),
            ("test", "pytest", "pass")),
        "coding_claim_matches_executable_result": (not any_term(coding_message.lower(),
            ("pass", "passed", "success")) or code_tests_pass),
    }
    return {"model": model, "arm": arm, "minimum_10_compactions_met": minimum_10_met,
            "measurement_complete": measurement_complete,
            "vrs_protocol_clean": protocol_clean,
            "confirmation_valid": bool(measurement_complete and protocol_clean and code_tests_pass),
            "post_minimum_compaction_count": max(0, len(windows) - 10),
            "compaction_count": len(windows), "compaction_windows": windows,
            "compaction_count_total": len(compaction_ids),
            "filler_lines_between_compactions": filler_lines_between,
            "response_records_unique": unique_ids,
            "suspicious_external_commands": suspicious_commands,
            "provider_calls_total": len(records), "provider_calls_compaction": len(compact_records),
            "provider_calls_other": len(normal_records),
            "usage_all_provider_responses": totals, "usage_compaction_responses": compact_usage,
            "usage_other_responses": normal_usage,
            "usage_before_first_compaction": pre_first_usage,
            "usage_post_terminal_boundary_outcomes": outcome_usage,
            "recovery_compaction_count": len(recovery_compacts),
            "coding_compaction_count": len(coding_compacts),
            "api_price_equivalent_usd_all": usd(totals, model),
            "api_price_equivalent_usd_compaction": usd(compact_usage, model),
            "api_price_equivalent_usd_post_terminal_boundary_outcomes": usd(outcome_usage, model),
            "peak_input_tokens": max((x["usage"]["input_tokens"] for x in records), default=None),
            "effective_context_window": effective,
            "wall_seconds": sum(r["elapsed_ns"] for r in all_rows) / 1e9,
            "live_experience_complete": live_ok,
            "live_watchers_complete": watchers_ok,
            "live_experience_final": all_rows[-1].get("live_experience"),
            "live_watcher_final": all_rows[-1].get("live_watcher"),
            "live_supervisor_stop": supervisor_stop,
            "final_session_end_merge": all_rows[-1].get("final_session_end_merge"),
            "actual_vrs_sequences_boundaries": [row["mcp_sequence"] for row in boundaries],
            "boundary_vrs_protocol_complete": [vrs_protocol(calls, row.get("required_episode_id"),
                                                              row.get("required_routing_session_id"))
                                                for calls, row in zip(boundary_calls, boundaries)],
            "actual_vrs_sequence_recovery": recoveries[0]["mcp_sequence"] if recoveries else [],
            "actual_vrs_sequence_coding": codings[0]["mcp_sequence"] if codings else [],
            "target_only": paths == [TARGET], "changed_paths": paths,
            "hidden_exit": hidden.returncode, "regression_exit": regression.returncode,
            "hidden_array_exit": hidden_array.returncode,
            "executable_success": code_tests_pass,
            "goal_consistency": boundary_scores["goal_consistency"],
            "code_quality": code_quality,
            "response_quality_checks": response_quality,
            "dialogue_context_consistency": boundary_scores["dialogue_context_consistency"],
            "purpose_json": purpose_json, "purpose_raw": purpose_raw,
            "boundary_answers": boundary_answers,
            "recovery_json": recovery_json, "recovery_raw": recovery_raw,
            "recovery_vrs_calls": recovery_calls,
            "coding_vrs_calls": coding_calls,
            "recovery_vrs_protocol_complete": vrs_protocol(
                recovery_calls, recoveries[0].get("required_episode_id") if recoveries else None,
                recoveries[0].get("required_routing_session_id") if recoveries else None),
            "coding_vrs_protocol_complete": vrs_protocol(
                coding_calls, codings[0].get("required_episode_id") if codings else None,
                codings[0].get("required_routing_session_id") if codings else None),
            "coding_message": coding_message,
            "diff_bytes": len(diff.encode())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--plain-runs", type=Path,
                        default=Path("/var/tmp/vrs22-auto-compaction-confirmation-005"))
    args = parser.parse_args()
    if args.output_dir.exists():
        parser.error("output directory already exists")
    data = json.loads((args.runs / "results.json").read_text(encoding="utf-8"))
    if (data.get("schema_version") !=
            "vrs22-auto-compaction-stress-v12-final-observation-accounting"
            or data.get("runtime_product_commit") != RUNTIME_PRODUCT_COMMIT
            or data.get("runtime_repository_commit") != RUNTIME_REPOSITORY_COMMIT
            or data.get("runtime_wheel_sha256") != RUNTIME_WHEEL_SHA256):
        parser.error("run is not from the frozen native VRS 2.2 runtime")
    grouped = defaultdict(list)
    for row in data["rows"]:
        grouped[(row["model"], row["arm"])].append(row)
    expected = {(model, "short_vrs") for model in RATES}
    if set(grouped) != expected:
        parser.error(f"expected the three new short_vrs cells, found {sorted(grouped)}")
    args.output_dir.mkdir(parents=True)
    results = []
    for (model, arm), rows in sorted(grouped.items()):
        result = grade_cell(model, arm, rows, args.runs, args.output_dir)
        results.append(result)
        print(model, arm, "compactions", result["compaction_count"],
              "minimum-10-complete", result["minimum_10_compactions_met"],
              "code", result["executable_success"], flush=True)
    plain_data = json.loads((args.plain_runs / "results.json").read_text(encoding="utf-8"))
    if plain_data.get("schema_version") != "vrs22-auto-compaction-stress-v1":
        parser.error("plain comparison is not the frozen v005 result schema")
    plain_grouped = defaultdict(list)
    for row in plain_data["rows"]:
        if row.get("arm") in ("short_plain", "long_plain"):
            plain_grouped[(row["model"], row["arm"])].append(row)
    expected_plain = {(model, arm) for model in RATES
                      for arm in ("short_plain", "long_plain")}
    if set(plain_grouped) != expected_plain:
        parser.error(f"expected six frozen plain cells, found {sorted(plain_grouped)}")
    plain_results = []
    for (model, arm), rows in sorted(plain_grouped.items()):
        result = grade_cell(model, arm, rows, args.plain_runs, args.output_dir)
        plain_results.append(result)
        print(model, arm, "compactions", result["compaction_count"],
              "minimum-10-complete", result["minimum_10_compactions_met"],
              "code", result["executable_success"], flush=True)
    indexed = {(row["model"], row["arm"]): row for row in [*results, *plain_results]}
    comparison = []
    for model in RATES:
        cells = {arm: indexed[(model, arm)] for arm in
                 ("short_plain", "long_plain", "short_vrs")}
        initial_hashes = {}
        for arm in cells:
            source_rows = grouped[(model, arm)] if arm == "short_vrs" else plain_grouped[(model, arm)]
            initial_hashes[arm] = next(row["prompt_sha256"] for row in source_rows
                                       if row["label"].endswith("__initial"))
        comparison.append({"model": model,
            "initial_task_hash_match": len(set(initial_hashes.values())) == 1,
            "initial_prompt_sha256": initial_hashes,
            "headline_axes": {arm: {
                "goal_consistency_first_ten": cell["goal_consistency"]["matched_first_ten"],
                "code_quality": cell["code_quality"],
                "strict_executable_success": cell["executable_success"],
                "dialogue_context_first_ten":
                    cell["dialogue_context_consistency"]["matched_first_ten"],
                "token_usage": cell["usage_all_provider_responses"],
                "compaction_token_usage": cell["usage_compaction_responses"],
                "wall_seconds": cell["wall_seconds"],
            } for arm, cell in cells.items()}})
    (args.output_dir / "summary.json").write_text(json.dumps({
        "schema_version": "vrs22-auto-compaction-summary-v9-final-observation-accounting",
        "vrs_cells": results, "plain_cells": plain_results,
        "comparison": comparison,
        "plain_comparison_source": str(args.plain_runs),
        "plain_results_sha256": hashlib.sha256(
            (args.plain_runs / "results.json").read_bytes()).hexdigest()},
        indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
