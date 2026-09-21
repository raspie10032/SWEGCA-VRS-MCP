# -*- coding: utf-8 -*-
"""The harness loop as a package: gates decide, snapshots build, conformance counts (docs/ADAPTER_SPEC.md)."""
import json
import os
import time

from swegca_vrs2 import adapter
from swegca_vrs2.harness import guard_backslash, guard_label, precompact, project_dir

LOG = "memory/session-log.md"


def test_gates_decide_without_a_harness(tmp_path, monkeypatch):
    monkeypatch.setenv("VRS2_RECEIPTS", str(tmp_path))          # ledgers land in a scratch dir
    monkeypatch.setenv("VRS2_BACKSLASH_GUARD", "1")            # exercise the measured Windows gate on this host
    assert guard_backslash.decide("Bash", "echo hi", "t") is None
    assert guard_backslash.decide("Bash", "python -c \"print('a\\\\b')\"", "t")   # doubled backslash -> reason
    assert guard_backslash.decide("Bash", "x \\\\ y # backslash-ok", "t") is None
    stale = "cat >" + "> " + LOG + " <<'EOF'\n- 2001-01-01 00:0x: x\nEOF"
    assert "오늘 날짜가 아니다" in guard_label.decide("Bash", stale, "t")
    now = time.strftime("%Y-%m-%d %H:%M")[:-1] + "x"
    fresh = "cat >" + "> " + LOG + f" <<'EOF'\n- {now}: y\nEOF"
    assert guard_label.decide("Bash", fresh, "t") is None
    assert guard_label.decide("Bash", "sed -i 's/a/b/' " + LOG, "t") is None   # no append: not checked


def test_precompact_entry_carries_requests_answer_and_next(tmp_path):
    t = tmp_path / "t.jsonl"
    rows = [{"type": "user", "message": {"content": "첫 요청"}},
            {"type": "assistant", "message": {"content": [{"type": "text", "text": "답. 다음: 둘째 단계"}]}},
            {"type": "user", "message": {"content": "둘째 요청"}}]
    t.write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in rows) + "\n", encoding="utf-8")
    entry = precompact.build_entry(precompact.tail_records(str(t)), "test", "sess")
    assert "「첫 요청」" in entry and "「둘째 요청」" in entry and "다음: 둘째 단계" in entry and "그 뒤 요청" in entry


def test_project_dir_walks_up_to_the_nearest_project(tmp_path, monkeypatch):
    monkeypatch.setattr(project_dir, "PROJECTS", str(tmp_path))
    root = tmp_path / "work"
    sub = root / "deep" / "er"
    sub.mkdir(parents=True)
    mem = tmp_path / project_dir.slug_of(str(root)) / "memory"
    mem.mkdir(parents=True)
    (mem / "session-log.md").write_text("- x\n", encoding="utf-8")
    slug, memory = project_dir.resolve(str(sub))
    assert slug == project_dir.slug_of(str(root)) and os.path.samefile(memory, mem)


def test_conformance_counts_receipts_per_point(tmp_path, monkeypatch):
    monkeypatch.setattr(adapter, "RECEIPTS_DIR", str(tmp_path))
    (tmp_path / "recall_context.log").write_text(
        json.dumps({"injected": ["a"], "session": "s1", "ts": "2026-09-18 10:00:00"}) + "\n"
        + json.dumps({"use": "read", "session": "s1", "ts": "2026-09-18 10:00:01"}) + "\n", encoding="utf-8")
    (tmp_path / "repeat_ledger.log").write_text(json.dumps({"slug": "x", "session": "s1"}) + "\n", encoding="utf-8")
    c = adapter.conformance("s1")
    assert c["before_prompt"] == 1 and c["on_read"] == 1 and c["before_action"] == 1 and c["on_stop"] == 0


def test_memory_doc_regex_accepts_posix_and_windows_paths():
    from swegca_vrs2.harness import guard_unopened as g
    assert g.MEMORY_DOC.fullmatch("/home/u/.claude/projects/-home-u-proj/memory/notes.md")
    assert g.MEMORY_DOC.fullmatch("/Users/u/.claude/projects/-Users-u-proj/memory/session-log.md")
    assert g.MEMORY_DOC.fullmatch("C:/Users/u/.claude/projects/C--Users-u-proj/memory/notes.md")
    assert not g.MEMORY_DOC.fullmatch("/home/u/proj/notes.md")


def test_backslash_gate_is_windows_only_unless_forced(monkeypatch):
    from swegca_vrs2.harness import guard_backslash as g
    doubled = "printf 'a" + "\\" + "\\" + "b'"          # two backslash characters in the command
    monkeypatch.setattr(os, "name", "posix")
    monkeypatch.delenv("VRS2_BACKSLASH_GUARD", raising=False)
    assert g.decide("Bash", doubled, "t") is None
    monkeypatch.setenv("VRS2_BACKSLASH_GUARD", "1")
    assert g.decide("Bash", doubled, "t")


def test_paths_resolve_env_over_config_over_default(tmp_path, monkeypatch):
    from swegca_vrs2.harness import paths
    monkeypatch.setattr(paths, "CONFIG", str(tmp_path / "vrs2.json"))
    (tmp_path / "vrs2.json").write_text(json.dumps({"state": "/from/config"}), encoding="utf-8")
    monkeypatch.delenv("VRS2_STATE", raising=False)
    assert paths._get("state", "/default") == "/from/config"
    monkeypatch.setenv("VRS2_STATE", "/from/env")
    assert paths._get("state", "/default") == "/from/env"
    assert paths._get("nothing", "/default") == "/default"
