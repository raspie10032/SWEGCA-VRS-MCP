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
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "evals/vrs22_context/fixtures/checkpoint_v019"
TARGET = "src/swegca_vrs2/checkpoint.py"
HIDDEN = ROOT / "evals/vrs22_context/hidden/test_checkpoint_duplicate.py"
HIDDEN_ARRAY = ROOT / "evals/vrs22_context/hidden/test_checkpoint_duplicate_array.py"
REGRESSION = BASELINE / "tests/standalone/test_checkpoint.py"
PYTHON = Path("/home/raspie/.local/share/vrs22-eval-runtime-v019/venv/bin/python")
PYTHON_RUNTIME_ROOT = Path(os.path.realpath(PYTHON)).parents[1]
GRADE_GUARD_SOURCE = ROOT / "tools/vrs22_shell_guard_v019.c"
RUBRIC = ROOT / "docs/VRS2_2_AUTO_COMPACTION_GRADING_RUBRIC_20260921.md"
BASELINE_SHA256 = "6b35f73d7241620a43e30811b86811a8a1ec5a92e7ffa4e5f295054817cb7b0c"
HIDDEN_SHA256 = "47ae830eb1fe6a715028ad703c3eabef19d3fdd7f9ea9608eae649c2a9701d15"
HIDDEN_ARRAY_SHA256 = "8d600a7e0dbb0d316af9ebe47f42d38a028e7e8ed113f7764d612c470aecebef"
GRADE_GUARD_SHA256 = "a3d1fd46ff8eaeeb0f0783f640f5fdd070fbea1424ae5f4e25b1aba1dc771cae"
RUBRIC_SHA256 = "ea03176616c8c981a654ab17e150d31c040d48e48e1aea8a662e35ebe0a9e1bd"
REAL_CODEX_HOME = Path.home() / ".codex"
LIVE_STATE = Path("/home/raspie/.local/share/swegca-vrs2-codex")
USER_RUNTIME_DIR = Path("/run/user") / str(os.getuid())
RATES = {"gpt-5.6-luna": (.20, .02, 1.20),
         "gpt-5.6-terra": (2.00, .20, 12.00),
         "gpt-5.6-sol": (4.00, .40, 20.00)}
FIELDS = ("input_tokens", "cached_input_tokens", "cache_write_input_tokens",
          "output_tokens", "reasoning_output_tokens")
RUNTIME_PRODUCT_COMMIT = "0db5817"
RUNTIME_REPOSITORY_COMMIT = "0db5817"
RUNTIME_WHEEL_SHA256 = "b99c1cbd4f8f51f3ff57836706db92ae2838554a90fb03c9d2844f29bcfed7c2"
INITIAL_SHA256 = "a9249ca6fed86315b83cb96f89cc5d0936b2775468c3e450c3258b32abb49523"
PRIVATE_PLAIN_ROOT = Path("/home/raspie/.local/share/vrs22-eval-runtime-v019/private")
PLAIN_ARCHIVE = PRIVATE_PLAIN_ROOT / "plain-v005-control-evidence.tar.zst"
PLAIN_EXTRACTED = PRIVATE_PLAIN_ROOT / "plain-v005-control-evidence"
PLAIN_ARCHIVE_SHA256 = "e817623972c8161d3f9985811465d4f084977d3c781d0bb79088560f315ae9bb"
PLAIN_RESULTS_SHA256 = "8ca3a90c50fa86202464efcf434e4c6ec33a11951c9f464ad2a2b2121d9f4f29"


def ensure_plain_controls(path):
    """Restore frozen private plain evidence if the old /var/tmp tree is gone."""
    if path == PLAIN_EXTRACTED and not (path / "results.json").is_file():
        if hashlib.sha256(PLAIN_ARCHIVE.read_bytes()).hexdigest() != PLAIN_ARCHIVE_SHA256:
            raise ValueError("frozen_plain_archive_changed")
        listing = subprocess.check_output(["tar", "--zstd", "-tf", str(PLAIN_ARCHIVE)],
                                          text=True).splitlines()
        if (not listing or any(name.startswith("/") or ".." in Path(name).parts
                or "__short_vrs" in name or name.endswith((".sqlite", ".sqlite3", ".db"))
                or name.endswith("/auth.json") for name in listing)
                or sum("-codex-home/sessions/" in name and name.endswith(".jsonl")
                       for name in listing) != 72):
            raise ValueError("frozen_plain_archive_members_invalid")
        with tempfile.TemporaryDirectory(prefix="plain-v005-restore-",
                                         dir=PRIVATE_PLAIN_ROOT) as temporary:
            extracted = Path(temporary) / "data"
            extracted.mkdir(mode=0o700)
            subprocess.run(["tar", "--zstd", "-xf", str(PLAIN_ARCHIVE),
                            "-C", str(extracted)], check=True)
            if hashlib.sha256((extracted / "results.json").read_bytes()).hexdigest() \
                    != PLAIN_RESULTS_SHA256:
                raise ValueError("frozen_plain_results_changed")
            os.replace(extracted, path)
    if hashlib.sha256((path / "results.json").read_bytes()).hexdigest() != PLAIN_RESULTS_SHA256:
        raise ValueError("frozen_plain_results_changed")
    return path


def usage_total(records):
    return {field: sum(r["usage"][field] for r in records) for field in FIELDS}


def frozen_grading_inputs():
    digest = hashlib.sha256()
    for path in sorted(item for item in BASELINE.rglob("*") if item.is_file()
                       and "__pycache__" not in item.parts
                       and ".pytest_cache" not in item.parts):
        digest.update(path.relative_to(BASELINE).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    actual = {"fixture": digest.hexdigest()}
    for name, path in (("hidden", HIDDEN), ("hidden_array", HIDDEN_ARRAY),
                       ("guard", GRADE_GUARD_SOURCE), ("rubric", RUBRIC)):
        actual[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    expected = {"fixture": BASELINE_SHA256, "hidden": HIDDEN_SHA256,
                "hidden_array": HIDDEN_ARRAY_SHA256,
                "guard": GRADE_GUARD_SHA256, "rubric": RUBRIC_SHA256}
    if actual != expected:
        raise ValueError("frozen_grading_inputs_changed")
    return actual


def usd(usage, model):
    input_rate, cache_rate, output_rate = RATES[model]
    uncached = usage["input_tokens"] - usage["cached_input_tokens"] - usage["cache_write_input_tokens"]
    return (uncached * input_rate + usage["cached_input_tokens"] * cache_rate +
            usage["cache_write_input_tokens"] * input_rate * 1.25 +
            usage["output_tokens"] * output_rate) / 1_000_000


def changed_paths(workspace):
    """Compare the complete coding fixture, including newly created subtrees."""
    ignored = {".grade-tmp", ".pytest_cache", "__pycache__"}
    allowed_runtime = {"ping.txt", "vrs-state"}

    def files(root, *, runtime=False):
        return {path.relative_to(root).as_posix(): path for path in root.rglob("*")
                if (path.is_file() or path.is_symlink())
                and not (set(path.relative_to(root).parts) & ignored)
                and (not runtime or path.relative_to(root).parts[0] not in allowed_runtime)}

    original = files(BASELINE)
    current = files(workspace, runtime=True)
    return sorted(path for path in original.keys() | current.keys()
                  if path not in original or path not in current
                  or original[path].is_symlink() != current[path].is_symlink()
                  or original[path].read_bytes() != current[path].read_bytes())


def grade_guard(output):
    if hashlib.sha256(GRADE_GUARD_SOURCE.read_bytes()).hexdigest() != GRADE_GUARD_SHA256:
        raise ValueError("frozen_grading_guard_changed")
    binary = output / "vrs22-grade-guard"
    if not binary.exists():
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-o", str(binary),
                        str(GRADE_GUARD_SOURCE)], check=True)
    return binary


def run_test(workspace, test, guard):
    temporary = workspace / ".grade-tmp"
    temporary.mkdir(exist_ok=True)
    home = temporary / "home"
    home.mkdir(exist_ok=True)
    hidden = temporary / "hidden"
    hidden.mkdir(exist_ok=True)
    if test.is_relative_to(workspace):
        test_path = test
    else:
        test_path = hidden / test.name
        test_path.write_bytes(test.read_bytes())
    command = "exec " + " ".join(shlex.quote(str(part)) for part in
        (PYTHON, "-m", "pytest", "-q", "-p", "no:cacheprovider", test_path, "--tb=short"))
    wrapper = ["bwrap", "--die-with-parent", "--unshare-pid", "--unshare-net",
        "--ro-bind", "/", "/", "--tmpfs", "/var/tmp", "--tmpfs", "/tmp",
        "--tmpfs", str(REAL_CODEX_HOME), "--tmpfs", str(LIVE_STATE),
        "--tmpfs", str(PRIVATE_PLAIN_ROOT),
        "--tmpfs", str(USER_RUNTIME_DIR),
        "--tmpfs", str(Path.home() / "Documents"), "--tmpfs", "/usr/local/bin",
        "--dir", str(ROOT), "--dir", str(workspace.parent),
        "--bind", str(workspace), str(workspace),
        "--ro-bind", "/usr/bin/bash", "/usr/local/bin/vrs22-real-bash",
        "--ro-bind", str(guard), "/bin/bash", "--proc", "/proc", "--dev", "/dev",
        "--setenv", "HOME", str(home), "--setenv", "TMPDIR", str(temporary),
        "--setenv", "PYTHONPATH", str(workspace / "src"),
        "--setenv", "PYTHONDONTWRITEBYTECODE", "1",
        "--setenv", "VRS22_WORKSPACE", str(workspace),
        "--setenv", "VRS22_TEST_ROOT", str(PYTHON.parent.parent),
        "--setenv", "VRS22_PYTHON_RUNTIME_ROOT", str(PYTHON_RUNTIME_ROOT),
        "--setenv", "VRS22_SHELL_SNAPSHOT_ROOT", str(temporary / "hidden"),
        "--chdir", str(workspace), "--", "/bin/bash", "-c", command]
    clean_env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
    result = subprocess.run(wrapper, cwd=workspace, env=clean_env,
                            capture_output=True, text=True, timeout=120)
    if result.returncode == 125 or result.stderr.startswith("bwrap:"):
        raise RuntimeError("isolated grading environment failed: " +
                           result.stderr[:500])
    return result


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
            expected_episode = row.get("required_episode_id")
            expected_session = row.get("required_routing_session_id")
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
                          "original_replay_valid": objective_replay_matches(
                              packet, expected_episode, expected_session),
                          "stage_query_values":
                              {name: section.get("data") for name, section in queries.items()},
                          "episode_ids": ([memory.get("episode_id") for memory in packet.get("memories", [])]
                                          if isinstance(packet, dict) else [])})
    return calls


def objective_replay_matches(packet, episode_id, session_id):
    """Independently verify the complete original objective in the MCP Replay."""
    if (not isinstance(packet, dict) or packet.get("status") != "memory_context_ready"
            or packet.get("candidate_count") != 1
            or not isinstance(episode_id, str)
            or not isinstance(session_id, str) or not session_id):
        return False
    memories = packet.get("memories")
    if not isinstance(memories, list) or len(memories) != 1:
        return False
    row = memories[0]
    section = row.get("replay") if isinstance(row, dict) else None
    if not isinstance(section, dict) or section.get("complete") is not True:
        return False
    replay = section.get("data")
    if not isinstance(replay, dict) or replay.get("episode_id") != episode_id \
            or replay.get("historical_truth_authorized") is not False:
        return False
    sources, steps = replay.get("source_addresses"), replay.get("steps")
    if (not isinstance(sources, list) or len(sources) != 1
            or not isinstance(sources[0], str)
            or not sources[0].startswith("transcript:codex:"
                + hashlib.sha256(session_id.encode("utf-8")).hexdigest() + ":")
            or not sources[0].endswith(":part:1")
            or not isinstance(steps, list) or len(steps) != 1):
        return False
    observation = steps[0].get("observation") if isinstance(steps[0], dict) else None
    metadata = observation.get("metadata") if isinstance(observation, dict) else None
    return bool(isinstance(metadata, dict)
        and metadata.get("origin") == "session_transcript"
        and metadata.get("host") == "codex"
        and metadata.get("role") == "user"
        and metadata.get("record_type") == "message"
        and metadata.get("part") == metadata.get("parts") == 1
        and metadata.get("epistemic_status") == "unverified_transcript"
        and observation.get("current_truth_claimed") is False
        and isinstance(observation.get("text"), str)
        and hashlib.sha256(observation["text"].encode("utf-8")).hexdigest()
            == INITIAL_SHA256)


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
    return bool(exact and routing and order and release_matches and clean
                and (expected_episode_id is None or packet["original_replay_valid"]))


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
                or evidence.get("main_journal_sequence") !=
                    evidence.get("original_main_journal_sequence", 0)
                or evidence.get("main_state_transition_parts") !=
                    evidence.get("original_main_state_transition_parts", 0)
                or evidence.get("main_logical_experience_parts") !=
                    evidence.get("original_main_primary_parts", 0) +
                    evidence.get("original_main_auto_parts", 0) +
                    evidence.get("original_main_linked_parts", 0)
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


def explicit_conflicts(answer):
    """Fail closed on direct reversals; subtler claims still need review."""
    purpose, constraints, superseded = answer_parts(answer)
    current = purpose + " " + constraints
    conflicts = set()
    if re.search(r"\b(?:do not|don't|never|must not|should not)\s+reject\b.{0,80}\bduplicate\b", current):
        conflicts.add("duplicate_before_reads")
    if re.search(r"\b(?:read|load)\b.{0,50}\b(?:metadata|arrays?)\b.{0,60}\bbefore\b.{0,50}\bduplicate\b", current):
        conflicts.add("duplicate_before_reads")
    if re.search(r"\b(?:do not|don't|never|must not|should not)\s+raise\b.{0,80}\bcheckpoint_integrity_failed\b", current):
        conflicts.add("exact_diagnostic")
    if (re.search(r"\b(?:do not|don't|never|must not|should not)\s+(?:allow|accept|permit)\b.{0,80}\b(?:extra|additional)\b", current)
            or re.search(r"\breject\s+(?:all|any|every)\b.{0,40}\b(?:extra|additional)\b", current)
            or re.search(r"\b(?:extra|additional)\b.{0,30}\b(?:must|should)\s+be\s+rejected\b", current)):
        conflicts.add("allow_valid_unrelated_extras")
    if re.search(r"\b(?:not|never)\s+superseded\b|\b(?:still applies|remains in force)\b", superseded):
        conflicts.add("superseded_reject_all_extras")
    return conflicts


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
    conflicts = explicit_conflicts(answer)
    for item in items:
        if item["item"] in conflicts:
            item["score"] = 0
            item["reason"] = "answer explicitly reverses this requirement"
    return {"score": sum(item["score"] for item in items), "items": items,
            "explicit_conflicts": sorted(conflicts)}


def contradiction_in(answer):
    return bool(explicit_conflicts(answer))


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
    content_integrity = final_merge.get("content_integrity", {}) if isinstance(final_merge, dict) else {}
    content_integrity_ok = (arm != "short_vrs" or (
        content_integrity.get("status") == "PASS"
        and type(content_integrity.get("expected_original_observations")) is int
        and content_integrity.get("expected_original_observations")
            == content_integrity.get("actual_original_observations")
        and content_integrity.get("expected_original_observations")
            == merge_coverage.get("stored_parts")
        and content_integrity.get("expected_sha256")
            == content_integrity.get("actual_sha256")
        and content_integrity.get("missing_requests") == 0
        and content_integrity.get("unexpected_requests") == 0
        and content_integrity.get("changed_requests") == 0))
    final_merge_ok = (arm != "short_vrs" or (
        merge_coverage.get("queued_parts") == 0
        and merge_coverage.get("session_experience_parts") == merge_coverage.get("stored_parts")
        and merge_coverage.get("merged_experience_parts") == merge_coverage.get("stored_parts")
        and final_merge.get("live_experience", {}).get("main_logical_experience_parts")
            == (merge_coverage.get("original_main_primary_parts", 0)
                + merge_coverage.get("original_main_auto_parts", 0)
                + merge_coverage.get("original_main_linked_parts", 0)
                + merge_coverage.get("stored_parts", 0))
        and final_merge.get("live_experience", {}).get("main_journal_sequence")
            == merge_coverage.get("original_main_journal_sequence", 0)
        and final_merge.get("live_experience", {}).get("main_state_transition_parts")
            == merge_coverage.get("original_main_state_transition_parts", 0)
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
    seed = initial[0].get("retained_main_seed", {})
    retained_main_ok = (arm != "short_vrs" or (
        seed.get("status") == "PASS"
        and seed.get("primary_observations", 0) + seed.get("auto_records", 0)
            + seed.get("linked_records", 0) > 0
        and seed.get("fallback_probe", {}).get("status") == "PASS"
        and seed.get("fallback_probe", {}).get("main_fallback") is True
        and seed.get("fallback_probe", {}).get("four_stage") is True
        and seed.get("database_artifacts") == []))
    minimum_10_met = len(windows) >= 10 and ordered
    measurement_complete = (len(purposes) == len(recoveries) == len(codings) == 1
                and [r["label"] for r in boundaries] == expected_boundary_labels
                and minimum_10_met
                and all(n > 0 for n in filler_lines_between)
                and all(r["window_held"] and r["effective_context_window"] == effective
                        for r in all_rows)
                and all(r["tool_types"] == [] for r in initial)
                and retained_main_ok and live_ok and watchers_ok and final_merge_ok
                and content_integrity_ok
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
                      and all(r.get("required_vrs_original_replay_observed", True)
                              for r in all_rows)
                      and boundary_tools_ok and outcome_tools_ok)
    workspace = run_dir / (stem + "-workspace")
    original = (BASELINE / TARGET).read_text(encoding="utf-8")
    final = (workspace / TARGET).read_text(encoding="utf-8")
    grade_workspace = workspace
    if arm != "short_vrs":
        # Historical plain workspaces include the retired substrate. Grade the
        # unchanged model output on the same clean substrate as new VRS cells.
        grade_workspace = output / (stem + "-grade-workspace")
        shutil.copytree(BASELINE, grade_workspace,
                        ignore=shutil.ignore_patterns("__pycache__", ".pytest_cache", "*.pyc"))
        shutil.copy2(workspace / TARGET, grade_workspace / TARGET)
    diff = "".join(difflib.unified_diff(original.splitlines(keepends=True),
                                        final.splitlines(keepends=True),
                                        fromfile="a/" + TARGET, tofile="b/" + TARGET))
    (output / (stem + ".diff")).write_text(diff, encoding="utf-8")
    paths = changed_paths(grade_workspace)
    guard = grade_guard(output)
    hidden = run_test(grade_workspace, HIDDEN, guard)
    hidden_array = run_test(grade_workspace, HIDDEN_ARRAY, guard)
    regression = run_test(grade_workspace,
                          grade_workspace / "tests/standalone/test_checkpoint.py", guard)
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
            "measurement_valid": bool(measurement_complete and protocol_clean),
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
            "session_content_integrity_complete": content_integrity_ok,
            "retained_main_complete": retained_main_ok,
            "retained_main_seed": seed if arm == "short_vrs" else None,
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
            "quality_review": {"status": "pending", "automated_scores_provisional": True,
                "required_evidence": ["all_boundary_answers", "recovery_and_final_messages",
                                      "complete_code_diff", "executable_test_log"]},
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
    parser.add_argument("--plain-runs", type=Path, default=PLAIN_EXTRACTED)
    args = parser.parse_args()
    if args.output_dir.exists():
        parser.error("output directory already exists")
    try:
        grading_inputs = frozen_grading_inputs()
    except (OSError, ValueError) as error:
        parser.error(f"frozen grading inputs unavailable: {error}")
    try:
        args.plain_runs = ensure_plain_controls(args.plain_runs)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(f"frozen plain evidence unavailable: {type(error).__name__}: {error}")
    data = json.loads((args.runs / "results.json").read_text(encoding="utf-8"))
    if (data.get("schema_version") !=
            "vrs22-auto-compaction-stress-v13-retained-main"
            or data.get("runtime_product_commit") != RUNTIME_PRODUCT_COMMIT
            or data.get("runtime_repository_commit") != RUNTIME_REPOSITORY_COMMIT
            or data.get("runtime_wheel_sha256") != RUNTIME_WHEEL_SHA256
            or data.get("retained_main_seed", {}).get("status") != "PASS"
            or data.get("retained_main_seed", {}).get("fallback_probe", {}).get("status") != "PASS"
            or data.get("retained_main_layer_selftest", {}).get("status") != "PASS"
            or data.get("retained_main_replay_selftest") != {
                "status": "PASS", "original_replayed": True, "four_stage": True,
                "internal_llm_calls": 0, "grants_authority": False}):
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
        same_task = len(set(initial_hashes.values())) == 1
        comparison.append({"model": model,
            "initial_task_hash_match": same_task,
            "comparison_eligible": same_task and all(
                cell["measurement_valid"] for cell in cells.values()),
            "quality_review_complete": False,
            "initial_prompt_sha256": initial_hashes,
            "headline_axes": {arm: {
                "measurement_valid": cell["measurement_valid"],
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
        "schema_version": "vrs22-auto-compaction-summary-v10-retained-main",
        "frozen_grading_input_sha256": grading_inputs,
        "vrs_cells": results, "plain_cells": plain_results,
        "comparison": comparison,
        "plain_comparison_source": str(args.plain_runs),
        "plain_results_sha256": hashlib.sha256(
            (args.plain_runs / "results.json").read_bytes()).hexdigest()},
        indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
