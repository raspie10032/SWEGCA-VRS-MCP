# -*- coding: utf-8 -*-
"""Portable goal-consistency-under-compaction scorer.

This extends the 2026-09-16 CSV scorer with truth sealing, structured Anthropic
and Codex transcript parsing, tool/read discipline, and one observation for each
real compaction boundary. Raw string searches never count compactions.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import statistics
import sys
import unicodedata
from typing import Any, Iterable

LABELS = ("SQLITE", "ME")
DEFAULT_EXCLUDE = ("2026-08-03", "2026-09-10")
ITEM_RE = re.compile(r"^- (\d{4}-\d{2}-\d{2})\s*(.*)$")
DATE_RE = re.compile(r"\d{4}-\d{2}-\d{2}\Z")
CHUNK_RE = re.compile(r"chunk_(SQLITE|ME)_(\d+)\.csv\Z", re.I)
NEXT_RE = re.compile(r"다음\s*:\s*(SQLITE|ME)\s+(\d+)", re.I)
SCAN_TOOLS = {"grep", "glob", "bash", "powershell"}
WRITE_TOOLS = {"write", "edit"}
READ_TOOLS = {"read"}
ASK_TOOLS = {"askuserquestion", "request_user_input", "request_user_input_async"}
GOAL_WORDS = ("항목", "csv", "청크", "제외", "2026-08-03", "2026-09-10")


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8-sig", errors="replace")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalize_text30(text: str) -> str:
    text = unicodedata.normalize("NFC", text).replace("**", "").replace("`", "")
    return re.sub(r"\s+", " ", text.lstrip(" :")).strip()


def seal_truth(inputs: Path, target: Path,
               exclude: Iterable[str] = DEFAULT_EXCLUDE) -> dict[str, Any]:
    truth: dict[str, Any] = {"exclude": list(exclude), "per_file": {},
                            "items": [], "input_hashes": {}}
    for label in LABELS:
        path = inputs / f"{label}-session-log.md"
        if not path.is_file():
            raise FileNotFoundError(f"missing input: {path}")
        lines = read_text(path).splitlines()
        kept = removed = 0
        for number, raw in enumerate(lines, 1):
            match = ITEM_RE.fullmatch(raw)
            if not match:
                continue
            date, body = match.groups()
            text30 = re.sub(r"[,\"\r\n]", " ", body).strip(" :")[:30]
            if date in truth["exclude"]:
                removed += 1
            else:
                truth["items"].append({"file": label, "line": number,
                                       "date": date, "text30": text30})
                kept += 1
        truth["per_file"][label] = {"items": kept, "excluded": removed,
                                     "lines": len(lines)}
        truth["input_hashes"][path.name] = sha256(path)
    shapes = {k: v["lines"] for k, v in truth["per_file"].items()}
    if shapes == {"SQLITE": 2005, "ME": 778}:
        counts = {k: (v["items"], v["excluded"])
                  for k, v in truth["per_file"].items()}
        expected = {"SQLITE": (521, 53), "ME": (217, 25)}
        if counts != expected or len(truth["items"]) != 738:
            raise ValueError(f"truth anchor mismatch: {counts}/{len(truth['items'])}")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(truth, ensure_ascii=False, indent=2) + "\n",
                      encoding="utf-8")
    return truth


def load_truth(path: Path) -> dict[str, Any]:
    truth = json.loads(read_text(path))
    missing = {"exclude", "per_file", "items", "input_hashes"} - truth.keys()
    if missing:
        raise ValueError(f"truth missing fields: {sorted(missing)}")
    return truth


def verify_inputs(inputs: Path, truth: dict[str, Any]) -> dict[str, Any]:
    actual, mismatch = {}, {}
    for name, expected in truth["input_hashes"].items():
        path = inputs / name
        value = sha256(path) if path.is_file() else None
        actual[name] = value
        if value != expected:
            mismatch[name] = {"expected": expected, "actual": value}
    return {"input_hashes": actual, "input_hash_mismatch": mismatch,
            "inputs_unchanged": not mismatch}


def truth_index(truth: dict[str, Any]) -> dict[tuple[str, int], dict[str, Any]]:
    return {(row["file"], int(row["line"])): row for row in truth["items"]}


def expected_chunks(truth: dict[str, Any]) -> list[str]:
    result = []
    for label in LABELS:
        if label in truth["per_file"]:
            total = int(truth["per_file"][label]["lines"])
            result += [f"chunk_{label}_{start}.csv"
                       for start in range(1, total + 1, 200)]
    return result


def parse_csv(path: Path, allowed: set[str]):
    rows, bad = [], []
    for raw in read_text(path).splitlines():
        raw = raw.lstrip("\ufeff")
        if not raw.strip():
            continue
        try:
            parts = next(csv.reader([raw]))
        except csv.Error:
            parts = raw.split(",")
        if (len(parts) < 4 or parts[0].strip().upper() not in allowed
                or not parts[1].strip().isdigit()
                or not DATE_RE.fullmatch(parts[2].strip())):
            bad.append([path.name, raw[:160]])
            continue
        rows.append({"file": parts[0].strip().upper(),
                     "line": int(parts[1].strip()), "date": parts[2].strip(),
                     "text30": ",".join(parts[3:]).strip(), "chunk": path.name})
    return rows, bad


def read_run(run_dir: Path, truth: dict[str, Any]):
    rows, bad, files, by_chunk = [], [], [], {}
    for path in sorted(run_dir.glob("chunk_*.csv")):
        parsed, errors = parse_csv(path, set(truth["per_file"]))
        rows += parsed
        bad += errors
        files.append(path.name)
        by_chunk[path.name] = {"rows": parsed, "format_errors": errors}
    return rows, bad, files, by_chunk


def gaps(keys: Iterable[tuple[str, int]], truth_by: dict[tuple[str, int], Any]) -> list[str]:
    grouped: dict[str, list[int]] = {}
    for label, line in sorted(keys):
        grouped.setdefault(label, []).append(line)
    result = []
    for label, lines in grouped.items():
        start = previous = lines[0]
        for line in lines[1:] + [None]:
            if line is None or (line != previous + 1 and any(
                    (label, value) in truth_by for value in range(previous + 1, line))):
                result.append(f"{label}:{start}-{previous}" if start != previous
                              else f"{label}:{start}")
                if line is not None:
                    start = line
            if line is not None:
                previous = line
    return result


def report_count(text: str) -> int | None:
    for pattern in (r"(?:합계|총|total|rows?)\s*[:=]?\s*([0-9][0-9,]*)",
                    r"([0-9][0-9,]*)\s*(?:건|rows?)"):
        match = re.search(pattern, text, re.I)
        if match:
            return int(match.group(1).replace(",", ""))
    return None


def progress_final(text: str | None, truth: dict[str, Any]) -> bool | None:
    if text is None:
        return None
    if "완료" in text:
        return True
    matches = list(NEXT_RE.finditer(text))
    if not matches:
        return False
    label, offset = matches[-1].group(1).upper(), int(matches[-1].group(2))
    return offset > int(truth["per_file"][label]["lines"])


def score_completion(run_dir: Path, truth: dict[str, Any]):
    truth_by = truth_index(truth)
    rows, bad, files, by_chunk = read_run(run_dir, truth)
    seen, duplicates = set(), 0
    for row in rows:
        key = (row["file"], row["line"])
        duplicates += key in seen
        seen.add(key)
    hit = seen & truth_by.keys()
    violations = [row for row in rows if row["date"] in set(truth["exclude"])]
    violation_keys = {(row["file"], row["line"]) for row in violations}
    spurious = seen - truth_by.keys() - violation_keys
    missing = truth_by.keys() - hit
    off_pairs, used = [], set()
    for extra in sorted(spurious):
        target = next((candidate for candidate in
                       ((extra[0], extra[1] - 1), (extra[0], extra[1] + 1))
                       if candidate in missing and candidate not in used), None)
        if target:
            used.add(target)
            off_pairs.append({"spurious": list(extra), "missing": list(target)})
    first = {}
    for row in rows:
        first.setdefault((row["file"], row["line"]), row)
    date_ok = sum(first[key]["date"] == truth_by[key]["date"] for key in hit)
    text_ok = sum(normalize_text30(first[key]["text30"])[:12]
                  == normalize_text30(truth_by[key]["text30"])[:12] for key in hit)
    expected = expected_chunks(truth)
    report_path, progress_path = run_dir / "report.txt", run_dir / "progress.txt"
    report = read_text(report_path).strip() if report_path.is_file() else None
    progress = read_text(progress_path).strip() if progress_path.is_file() else None
    reported = report_count(report or "")
    result = {
        "run": run_dir.name, "rows": len(rows), "truth": len(truth_by), "hit": len(hit),
        "recall": round(len(hit) / len(truth_by), 4) if truth_by else 1.0,
        "missing": len(missing), "gaps": gaps(missing, truth_by),
        "spurious": len(spurious), "spurious_examples": [list(x) for x in sorted(spurious)[:10]],
        "off_by_one": len(off_pairs), "off_by_one_pairs": off_pairs[:10],
        "exclusion_violations": len(violations), "duplicates": duplicates,
        "format_errors": len(bad), "format_examples": bad[:3],
        "date_match": f"{date_ok}/{len(hit)}", "text30_match": f"{text_ok}/{len(hit)}",
        "chunk_files": f"{len(files)}/{len(expected)}",
        "chunk_missing": [x for x in expected if x not in files],
        "chunk_unexpected": [x for x in files if x not in expected],
        "report_count": reported, "report_delta": None if reported is None else reported - len(rows),
        "progress_final": progress_final(progress, truth),
    }
    detail = {"rows": rows, "bad": bad, "files": files, "by_chunk": by_chunk,
              "missing": set(missing), "truth_by": truth_by}
    return result, detail


def timestamp(value: Any) -> float | None:
    if isinstance(value, (int, float)):
        return float(value)
    if value is None:
        return None
    try:
        from datetime import datetime
        return datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp()
    except (TypeError, ValueError):
        return None


def message_text(content: Any) -> str:
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""
    return "\n".join(str(item.get("text", "")) for item in content
                     if isinstance(item, dict) and item.get("type") in
                     {"text", "output_text", "input_text"})


def tool_event(name: str, arguments: Any, index: int, stamp: Any) -> dict[str, Any]:
    if isinstance(arguments, str):
        try:
            arguments = json.loads(arguments)
        except (json.JSONDecodeError, TypeError):
            arguments = {"_raw": arguments}
    if not isinstance(arguments, dict):
        arguments = {"_value": arguments}
    return {"kind": "tool", "name": name, "input": arguments,
            "index": index, "timestamp": timestamp(stamp)}


def parse_transcript(path: Path) -> dict[str, Any]:
    events, errors, models, token_calls, records = [], [], [], [], []
    first_ts = last_ts = None
    for number, raw in enumerate(path.open(encoding="utf-8", errors="replace"), 1):
        try:
            record = json.loads(raw)
        except json.JSONDecodeError:
            errors.append(number)
            continue
        records.append(record)
        index = len(records) - 1
        stamp = record.get("timestamp")
        ts = timestamp(stamp)
        if ts is not None:
            first_ts = ts if first_ts is None else min(first_ts, ts)
            last_ts = ts if last_ts is None else max(last_ts, ts)
        rtype = record.get("type")
        if rtype == "system" and record.get("subtype") == "compact_boundary":
            meta = record.get("compactMetadata") or {}
            events.append({"kind": "boundary", "index": index, "timestamp": ts,
                           "trigger": meta.get("trigger"), "pre_tokens": meta.get("preTokens"),
                           "post_tokens": meta.get("postTokens"), "duration_ms": meta.get("durationMs"),
                           "format": "anthropic"})
        elif rtype == "compacted":
            payload = record.get("payload") or {}
            events.append({"kind": "boundary", "index": index, "timestamp": ts,
                           "trigger": "auto", "pre_tokens": None, "post_tokens": None,
                           "duration_ms": None, "format": "codex",
                           "compaction_response_id": payload.get("compaction_response_id")})
        if rtype == "assistant":
            message = record.get("message") or {}
            if message.get("model"):
                models.append(str(message["model"]))
            content = message.get("content") or []
            text = message_text(content)
            if text:
                events.append({"kind": "assistant", "text": text,
                               "index": index, "timestamp": ts})
            for item in content if isinstance(content, list) else []:
                if isinstance(item, dict) and item.get("type") == "tool_use":
                    events.append(tool_event(str(item.get("name", "")),
                                             item.get("input", {}), index, stamp))
            usage = message.get("usage") or {}
            if usage:
                context = sum(int(usage.get(key, 0) or 0) for key in
                              ("input_tokens", "cache_creation_input_tokens",
                               "cache_read_input_tokens"))
                output = int(usage.get("output_tokens", 0) or 0)
                token_calls.append({"index": index, "context": context,
                    "output": output, "total": context + output,
                    "input_tokens": int(usage.get("input_tokens", 0) or 0),
                    "cache_creation_input_tokens": int(
                        usage.get("cache_creation_input_tokens", 0) or 0),
                    "cache_read_input_tokens": int(
                        usage.get("cache_read_input_tokens", 0) or 0),
                    "cached_input_tokens": 0, "cache_write_input_tokens": 0,
                    "reasoning_output_tokens": 0, "response_id": None})
        if rtype == "response_item":
            payload = record.get("payload") or {}
            ptype = payload.get("type")
            if ptype == "message" and payload.get("role") == "assistant":
                text = message_text(payload.get("content"))
                if text:
                    events.append({"kind": "assistant", "text": text,
                                   "index": index, "timestamp": ts})
            elif ptype in {"custom_tool_call", "function_call"}:
                events.append(tool_event(str(payload.get("name", "")),
                                         payload.get("input", payload.get("arguments", {})),
                                         index, stamp))
        if rtype == "turn_context":
            model = (record.get("payload") or {}).get("model")
            if model:
                models.append(str(model))
        if rtype == "token_usage_record":
            payload = record.get("payload") or {}
            usage = payload.get("usage") or {}
            context = int(usage.get("input_tokens", 0) or 0)
            output = int(usage.get("output_tokens", 0) or 0)
            total = int(usage.get("total_tokens", context + output) or context + output)
            token_calls.append({"index": index, "context": context,
                "output": output, "total": total, "input_tokens": context,
                "cached_input_tokens": int(usage.get("cached_input_tokens", 0) or 0),
                "cache_write_input_tokens": int(
                    usage.get("cache_write_input_tokens", 0) or 0),
                "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0,
                "reasoning_output_tokens": int(
                    usage.get("reasoning_output_tokens", 0) or 0),
                "response_id": payload.get("response_id")})
    boundaries = [event for event in events if event["kind"] == "boundary"]
    for boundary in boundaries:
        if boundary["format"] == "codex":
            before = [row for row in token_calls if row["index"] < boundary["index"]]
            after = [row for row in token_calls if row["index"] > boundary["index"]]
            boundary["pre_tokens"] = before[-1]["context"] if before else None
            boundary["post_tokens"] = after[0]["context"] if after else None
    identifiers = [row["response_id"] for row in token_calls
                   if row.get("response_id") is not None]
    unique_response_records = len(identifiers) == len(set(identifiers))
    if not unique_response_records:
        seen, deduplicated = set(), []
        for row in token_calls:
            identifier = row.get("response_id")
            if identifier is not None and identifier in seen:
                continue
            if identifier is not None:
                seen.add(identifier)
            deduplicated.append(row)
        token_calls = deduplicated
    usage_fields = ("input_tokens", "cached_input_tokens", "cache_write_input_tokens",
                    "cache_creation_input_tokens", "cache_read_input_tokens",
                    "output", "reasoning_output_tokens")
    usage = {field: sum(int(row.get(field, 0) or 0) for row in token_calls)
             for field in usage_fields}
    usage["output_tokens"] = usage.pop("output")
    compact_ids = {event.get("compaction_response_id") for event in boundaries
                   if event.get("compaction_response_id")}
    compact_calls = [row for row in token_calls if row.get("response_id") in compact_ids]
    compact_usage = {field: sum(int(row.get(field, 0) or 0) for row in compact_calls)
                     for field in usage_fields}
    compact_usage["output_tokens"] = compact_usage.pop("output")
    tools, opaque = {}, []
    for event in events:
        if event["kind"] == "tool":
            tools[event["name"]] = tools.get(event["name"], 0) + 1
            if event["name"].lower() in {"exec", "functions.exec", "exec_command"}:
                opaque.append(event["name"])
    return {"path": str(path), "events": sorted(events, key=lambda x: (x["index"], x["kind"])),
            "boundaries": boundaries, "parse_errors": errors, "models": sorted(set(models)),
            "tokens_total": sum(row["total"] for row in token_calls),
            "token_usage": usage, "compaction_token_usage": compact_usage,
            "provider_response_records": len(identifiers),
            "provider_response_records_unique": unique_response_records,
            "context_peak": max((row["context"] for row in token_calls), default=0),
            "duration_s": None if first_ts is None or last_ts is None else round(last_ts - first_ts, 3),
            "tool_calls": tools, "opaque_tool_calls": len(opaque),
            "opaque_tool_examples": opaque[:3]}


def tool_name(event: dict[str, Any]) -> str:
    return str(event.get("name", "")).lower()


def path_arg(event: dict[str, Any]) -> str | None:
    data = event.get("input") or {}
    return next((str(data[key]) for key in ("file_path", "path", "filePath")
                 if data.get(key) is not None), None)


def read_call(event: dict[str, Any]):
    if tool_name(event) not in READ_TOOLS:
        return None
    path = path_arg(event)
    if not path:
        return None
    name = PurePosixPath(path.replace("\\", "/")).name.upper()
    label = next((value for value in LABELS if name.startswith(value)), None)
    if not label:
        return None
    data = event.get("input") or {}
    try:
        offset = int(data.get("offset", 1))
    except (TypeError, ValueError):
        offset = 1
    try:
        limit = int(data["limit"]) if data.get("limit") is not None else None
    except (TypeError, ValueError):
        limit = None
    return label, offset, limit


def write_call(event: dict[str, Any]):
    if tool_name(event) not in WRITE_TOOLS:
        return None
    path = path_arg(event)
    if not path:
        return None
    data = event.get("input") or {}
    return path, str(data.get("content", data.get("new_string", "")))


def norm_path(path: str) -> str:
    return os.path.normcase(os.path.normpath(path.replace("\\", "/")))


def inside(path: str, root: Path) -> bool:
    try:
        return os.path.commonpath((norm_path(path), norm_path(str(root)))) == norm_path(str(root))
    except ValueError:
        return False


def chunk_from_path(path: str):
    match = CHUNK_RE.fullmatch(PurePosixPath(path.replace("\\", "/")).name)
    return (match.group(1).upper(), int(match.group(2))) if match else None


def is_question(text: str) -> bool:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return bool(lines and lines[-1].endswith("?"))


def tool_discipline(parsed: dict[str, Any], run_dir: Path,
                    truth: dict[str, Any]) -> dict[str, Any]:
    reads, scan, outside, asks, unexpected = [], [], [], [], []
    allowed = set(expected_chunks(truth)) | {"report.txt", "progress.txt"}
    last_tool_index = max((event["index"] for event in parsed["events"]
                           if event["kind"] == "tool"), default=-1)
    for event in parsed["events"]:
        if event["kind"] == "tool":
            if tool_name(event) in SCAN_TOOLS:
                scan.append({"name": event["name"], "input": event["input"]})
            if tool_name(event) in ASK_TOOLS:
                asks.append({"kind": "tool", "name": event["name"]})
            call = read_call(event)
            if call:
                reads.append(call)
            written = write_call(event)
            if written:
                path, _ = written
                name = PurePosixPath(path.replace("\\", "/")).name
                if not inside(path, run_dir):
                    outside.append(path)
                elif name not in allowed:
                    unexpected.append(path)
        elif (event["kind"] == "assistant" and event["index"] < last_tool_index
              and is_question(event["text"])):
            asks.append({"kind": "text", "text": event["text"][-200:]})
    by_file = {label: [] for label in LABELS}
    seen, previous, rereads, out_of_order, split_reads = set(), {}, 0, 0, 0
    for label, offset, limit in reads:
        rereads += (label, offset) in seen
        seen.add((label, offset))
        out_of_order += label in previous and offset < previous[label]
        previous[label] = offset
        by_file[label].append(offset)
        split_reads += limit != 200
    expected = {label: list(range(1, int(data["lines"]) + 1, 200))
                for label, data in truth["per_file"].items()}
    return {"scan_tools": len(scan), "scan_tool_examples": scan[:3],
            "outside_writes": len(outside), "outside_write_examples": outside[:3],
            "reasked": len(asks), "reasked_examples": asks[:3],
            "unexpected_outputs": len(unexpected), "unexpected_output_examples": unexpected[:3],
            "reads_total": len(reads), "read_offsets": by_file, "rereads": rereads,
            "out_of_order": out_of_order, "split_reads": split_reads,
            "skipped_reads": {label: [x for x in offsets if x not in by_file[label]]
                              for label, offsets in expected.items()},
            "opaque_tool_calls": parsed["opaque_tool_calls"],
            "opaque_tool_examples": parsed["opaque_tool_examples"],
            "tool_audit_complete": parsed["opaque_tool_calls"] == 0}


def sequence(truth: dict[str, Any]) -> list[tuple[str, int]]:
    result = []
    for name in expected_chunks(truth):
        match = CHUNK_RE.fullmatch(name)
        if match:
            result.append((match.group(1).upper(), int(match.group(2))))
    return result


def expected_before(events: list[dict[str, Any]], boundary_index: int,
                    ledger: str, truth: dict[str, Any]):
    completed: set[tuple[str, int]] = set()
    last_progress = pending = None
    for event in events:
        if event["index"] >= boundary_index:
            break
        read = read_call(event) if event["kind"] == "tool" else None
        if read:
            pending = (read[0], read[1])
        write = write_call(event) if event["kind"] == "tool" else None
        if write:
            target = chunk_from_path(write[0])
            if target:
                completed.add(target)
                if pending == target:
                    pending = None
            if PurePosixPath(write[0].replace("\\", "/")).name.lower() == "progress.txt":
                matches = list(NEXT_RE.finditer(write[1]))
                if matches:
                    last = matches[-1]
                    last_progress = (last.group(1).upper(), int(last.group(2)))
    expected = last_progress if ledger.upper() == "B" and last_progress else next(
        (item for item in sequence(truth) if item not in completed), None)
    return expected, completed, pending


def window_asks(window: list[dict[str, Any]]) -> bool:
    last_tool_index = max((event["index"] for event in window
                           if event["kind"] == "tool"), default=-1)
    return any((event["kind"] == "tool" and tool_name(event) in ASK_TOOLS)
               or (event["kind"] == "assistant" and event["index"] < last_tool_index
                   and is_question(event["text"])) for event in window)


def boundary_observations(run: str, model: str | None, ledger: str,
                          parsed: dict[str, Any], run_dir: Path,
                          truth: dict[str, Any], detail: dict[str, Any]):
    events = parsed["events"]
    boundaries = [event for event in events if event["kind"] == "boundary"]
    positions = {item: index for index, item in enumerate(sequence(truth))}
    allowed = set(expected_chunks(truth)) | {"report.txt", "progress.txt"}
    observations = []
    for number, boundary in enumerate(boundaries, 1):
        stop = boundaries[number]["index"] if number < len(boundaries) else math.inf
        window = [event for event in events
                  if boundary["index"] < event["index"] < stop]
        expected, completed, pending = expected_before(
            events, boundary["index"], ledger, truth)
        tools = [event for event in window if event["kind"] == "tool"]
        reads = [(event, read_call(event)) for event in tools]
        reads = [(event, call) for event, call in reads if call]
        first = (reads[0][1][0], reads[0][1][1]) if reads else None
        resume = "none"
        if first is not None and expected is not None:
            if first == expected:
                resume = "correct"
            elif first in completed or positions.get(first, -1) < positions.get(expected, -1):
                resume = "reread"
            elif first[0] != expected[0]:
                resume = "wrong_file"
            else:
                resume = "skipped"
        steps = None
        if expected is not None:
            for step, event in enumerate(tools):
                call = read_call(event)
                if call and (call[0], call[1]) == expected:
                    steps = step
                    break
        scan_violation = any(tool_name(event) in SCAN_TOOLS for event in tools)
        outside_violation = scope_drift = chunk_errors = False
        for event in tools:
            write = write_call(event)
            if not write:
                continue
            path, _ = write
            name = PurePosixPath(path.replace("\\", "/")).name
            if not inside(path, run_dir):
                outside_violation = True
            elif name not in allowed:
                scope_drift = True
            if name in detail["by_chunk"]:
                chunk = detail["by_chunk"][name]
                chunk_errors |= bool(chunk["format_errors"] or any(
                    row["date"] in set(truth["exclude"]) for row in chunk["rows"]))
        reasked = window_asks(window)
        constraint_kept = not (scan_violation or outside_violation or reasked or chunk_errors)
        lost = []
        if pending:
            label, start = pending
            end = min(start + 199, int(truth["per_file"][label]["lines"]))
            lost = [key for key in detail["missing"]
                    if key[0] == label and start <= key[1] <= end]
        first_assistant = next((event["text"] for event in window
                                if event["kind"] == "assistant"), "")
        observations.append({
            "run": run, "model": model, "ledger": ledger.upper(), "boundary": number,
            "trigger": boundary.get("trigger"), "pre_tokens": boundary.get("pre_tokens"),
            "post_tokens": boundary.get("post_tokens"), "duration_ms": boundary.get("duration_ms"),
            "expected_next": list(expected) if expected else None,
            "first_read": list(first) if first else None, "resume": resume,
            "steps_to_resume": steps, "constraint_kept": int(constraint_kept),
            "reasked": int(reasked), "scope_drift": int(scope_drift),
            "items_lost": len(lost), "lost_ranges": gaps(lost, detail["truth_by"]),
            "goal_restated": int(sum(word in first_assistant for word in GOAL_WORDS) >= 3),
        })
    return observations


def aggregate(observations: list[dict[str, Any]]) -> dict[str, Any]:
    count = len(observations)
    if not count:
        return {"n_boundaries": 0, "retention": "unmeasured"}
    steps = [row["steps_to_resume"] for row in observations
             if row["steps_to_resume"] is not None]
    return {"n_boundaries": count,
            "resume_correct_rate": round(sum(row["resume"] == "correct"
                                             for row in observations) / count, 4),
            "mean_steps_to_resume": round(statistics.mean(steps), 3) if steps else None,
            "constraint_kept_rate": round(sum(row["constraint_kept"]
                                              for row in observations) / count, 4),
            "reasked_rate": round(sum(row["reasked"] for row in observations) / count, 4),
            "scope_drift_rate": round(sum(row["scope_drift"]
                                          for row in observations) / count, 4),
            "items_lost_total": sum(row["items_lost"] for row in observations),
            "retention": "measured" if count >= 10 else "insufficient_boundaries"}


def load_conditions(path: Path | None) -> dict[str, dict[str, Any]]:
    if path is None:
        return {}
    data = json.loads(read_text(path))
    if isinstance(data, list):
        return {str(row["run"]): row for row in data}
    if isinstance(data, dict) and isinstance(data.get("runs"), list):
        return {str(row["run"]): row for row in data["runs"]}
    if isinstance(data, dict):
        return {str(key): value for key, value in data.items()
                if isinstance(value, dict)}
    raise ValueError("conditions must be an object, list, or {runs:[...]}")


def run_dirs(root: Path) -> list[Path]:
    children = sorted(path for path in root.iterdir() if path.is_dir())
    return children if children else [root]


def transcript_files(root: Path | None) -> list[Path]:
    if root is None:
        return []
    return sorted(root.rglob("*.jsonl")) if root.is_dir() else ([root] if root.is_file() else [])


def pair_transcript(run_dir: Path, candidates: list[Path],
                    condition: dict[str, Any]) -> Path | None:
    explicit = condition.get("transcript")
    if explicit:
        candidate = Path(explicit)
        if candidate.is_absolute():
            return candidate
        base = Path(condition.get("transcript_base", candidates[0].parent if candidates else "."))
        return base / candidate
    matches = []
    needle = run_dir.name.encode("utf-8")
    for path in candidates:
        try:
            if needle in path.read_bytes()[:4096]:
                matches.append(path)
        except OSError:
            pass
    if len(matches) == 1:
        return matches[0]
    return candidates[0] if len(candidates) == 1 else None


def evaluate(args: argparse.Namespace) -> dict[str, Any]:
    truth = load_truth(args.truth)
    hashes = verify_inputs(args.inputs, truth)
    conditions = load_conditions(args.conditions)
    candidates = transcript_files(args.transcripts)
    results, all_observations = [], []
    for run_dir in run_dirs(args.runs):
        completion, detail = score_completion(run_dir, truth)
        condition = conditions.get(run_dir.name, {})
        path = pair_transcript(run_dir, candidates, condition)
        result = dict(completion)
        result.update(hashes)
        result.update({"model": condition.get("model"),
                       "ledger": str(condition.get("ledger", "A")).upper(),
                       "autocompact": condition.get("autocompact"),
                       "effort": condition.get("effort"),
                       "transcript": str(path) if path else None})
        invalid = []
        if hashes["input_hash_mismatch"]:
            invalid.append("input_hash_mismatch")
        if path is None or not path.is_file():
            result["transcript_error"] = "missing_or_ambiguous"
            result["retention"] = aggregate([])
            invalid.append("transcript_missing_or_ambiguous")
        else:
            parsed = parse_transcript(path)
            discipline = tool_discipline(parsed, run_dir, truth)
            observations = boundary_observations(run_dir.name, result["model"],
                                                 result["ledger"], parsed,
                                                 run_dir, truth, detail)
            all_observations += observations
            result.update({key: parsed[key] for key in
                           ("parse_errors", "models", "tokens_total", "context_peak",
                            "token_usage", "compaction_token_usage",
                            "provider_response_records", "provider_response_records_unique",
                            "duration_s", "tool_calls", "opaque_tool_calls",
                            "opaque_tool_examples")})
            result.update(discipline)
            result["retention"] = aggregate(observations)
            if len(observations) < 10:
                invalid.append("fewer_than_10_compactions")
            if parsed["parse_errors"]:
                invalid.append("transcript_parse_errors")
            if not parsed["provider_response_records_unique"]:
                invalid.append("duplicate_provider_response_records")
            if result["model"] and parsed["models"] and result["model"] not in parsed["models"]:
                invalid.append("model_mismatch")
            if not discipline["tool_audit_complete"]:
                invalid.append("opaque_tool_calls_prevent_constraint_audit")
        result["valid"] = not invalid
        result["invalid_reasons"] = invalid
        results.append(result)
    if args.observations:
        args.observations.parent.mkdir(parents=True, exist_ok=True)
        with args.observations.open("w", encoding="utf-8", newline="\n") as stream:
            for row in all_observations:
                stream.write(json.dumps(row, ensure_ascii=False) + "\n")
    return {"truth": str(args.truth), "runs": results,
            "observations": len(all_observations), "minimum_compactions": 10}


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--runs", type=Path)
    parser.add_argument("--truth", type=Path, required=True)
    parser.add_argument("--transcripts", type=Path)
    parser.add_argument("--conditions", type=Path)
    parser.add_argument("--observations", type=Path)
    parser.add_argument("--seal-truth", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    if args.seal_truth:
        truth = seal_truth(args.inputs, args.truth)
        print(json.dumps({"truth": str(args.truth), "items": len(truth["items"]),
                          "per_file": truth["per_file"],
                          "input_hashes": truth["input_hashes"]},
                         ensure_ascii=False, indent=2))
        if args.runs is None:
            return 0
    if args.runs is None:
        raise SystemExit("--runs is required unless --seal-truth is used alone")
    output = evaluate(args)
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0 if all(row["valid"] for row in output["runs"]) else 2


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
