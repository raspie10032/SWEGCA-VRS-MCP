from __future__ import annotations

import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "compaction_score", ROOT / "local/bench/compaction/score.py")
score = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(score)


def test_structural_compactions_ignore_false_string(tmp_path):
    path = tmp_path / "a.jsonl"
    path.write_text(json.dumps({"type": "assistant", "message": {
        "model": "m", "content": [{"type": "text", "text": "compact_boundary"}],
        "usage": {"input_tokens": 4, "output_tokens": 1}}}) + "\n", encoding="utf-8")
    parsed = score.parse_transcript(path)
    assert not parsed["boundaries"]
    assert parsed["tokens_total"] == 5


def test_anthropic_compactions_and_tokens(tmp_path):
    path = tmp_path / "a.jsonl"
    rows = [{"type": "assistant", "message": {"model": "test-model",
             "content": [{"type": "text", "text": "start"}],
             "usage": {"input_tokens": 10, "output_tokens": 2}}}]
    for index in range(10):
        rows += [{"type": "system", "subtype": "compact_boundary",
                  "compactMetadata": {"trigger": "auto", "preTokens": 100 + index,
                                      "postTokens": 20, "durationMs": 3}},
                 {"type": "assistant", "message": {"model": "test-model", "content": [{
                     "type": "tool_use", "name": "Read", "input": {
                         "file_path": "/tmp/SQLITE-session-log.md", "offset": 1, "limit": 200}}],
                     "usage": {"input_tokens": 20, "cache_creation_input_tokens": 1,
                               "cache_read_input_tokens": 2, "output_tokens": 3}}}]
    path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
    parsed = score.parse_transcript(path)
    assert len(parsed["boundaries"]) == 10
    assert parsed["models"] == ["test-model"]
    assert parsed["tokens_total"] == 12 + 10 * 26
    assert parsed["context_peak"] == 23


def test_codex_compactions_use_adjacent_usage(tmp_path):
    path = tmp_path / "codex.jsonl"
    rows = [{"type": "turn_context", "payload": {"model": "gpt-5.6-luna"}},
            {"type": "token_usage_record", "payload": {"response_id": "normal",
                "usage": {"input_tokens": 99, "cached_input_tokens": 80,
                          "cache_write_input_tokens": 0, "output_tokens": 1,
                          "reasoning_output_tokens": 1, "total_tokens": 100}}},
            {"type": "compacted", "payload": {"compaction_response_id": "compact"}},
            {"type": "token_usage_record", "payload": {"response_id": "compact",
                "usage": {"input_tokens": 21, "cached_input_tokens": 10,
                          "cache_write_input_tokens": 2, "output_tokens": 2,
                          "reasoning_output_tokens": 0, "total_tokens": 23}}}]
    path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
    parsed = score.parse_transcript(path)
    assert parsed["tokens_total"] == 123
    assert parsed["boundaries"][0]["pre_tokens"] == 99
    assert parsed["boundaries"][0]["post_tokens"] == 21
    assert parsed["provider_response_records"] == 2
    assert parsed["provider_response_records_unique"] is True
    assert parsed["token_usage"]["cached_input_tokens"] == 90
    assert parsed["compaction_token_usage"]["input_tokens"] == 21
    assert parsed["compaction_token_usage"]["cache_write_input_tokens"] == 2


def test_truth_seal_bom_crlf_and_completion(tmp_path):
    inputs = tmp_path / "input"
    inputs.mkdir()
    (inputs / "SQLITE-session-log.md").write_bytes(
        b"\xef\xbb\xbf# x\r\n- 2026-09-01: alpha, one\r\n- 2026-08-03: excluded\r\n")
    (inputs / "ME-session-log.md").write_text("- 2026-09-02 beta\n", encoding="utf-8")
    truth = score.seal_truth(inputs, tmp_path / "truth.json")
    assert len(truth["items"]) == 2
    assert truth["per_file"]["SQLITE"] == {"items": 1, "excluded": 1, "lines": 3}
    run = tmp_path / "run_x"
    run.mkdir()
    (run / "chunk_SQLITE_1.csv").write_text("SQLITE,2,2026-09-01,alpha one\n", encoding="utf-8")
    (run / "chunk_ME_1.csv").write_text("ME,1,2026-09-02,beta\n", encoding="utf-8")
    result, _ = score.score_completion(run, truth)
    assert result["recall"] == 1.0
    assert result["chunk_files"] == "2/2"


def test_off_by_one_pair(tmp_path):
    truth = {"exclude": [], "input_hashes": {},
             "per_file": {"SQLITE": {"lines": 2}, "ME": {"lines": 1}},
             "items": [{"file": "SQLITE", "line": 2,
                        "date": "2026-09-01", "text30": "x"}]}
    run = tmp_path / "run"
    run.mkdir()
    (run / "chunk_SQLITE_1.csv").write_text("SQLITE,1,2026-09-01,x\n", encoding="utf-8")
    result, _ = score.score_completion(run, truth)
    assert (result["missing"], result["spurious"], result["off_by_one"]) == (1, 1, 1)
