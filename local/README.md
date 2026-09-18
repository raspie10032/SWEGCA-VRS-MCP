# local/ — the Windows Claude Code adapter around the vrs2 store (snapshot 2026-09-18)

Copies of the scripts that run *around* this store on the author's PC. Paths inside them are
machine-specific (`C:\Users\asm\...`); they are versioned here so the branch carries the whole
loop, not only the engine. Logs, ledgers (`*.log`, `*.json` state) and company data are not included.

## hooks/ (`~/.claude/hooks`, wired in `~/.claude/settings.json`)

| event | script | what it does |
| --- | --- | --- |
| UserPromptSubmit | `recall_context_v2.py` | recall receipts per prompt: verdicts (asks-gated) + one record, 「열기」 line per item, 「※ 재검증 필요」 with both sides, `[열림 m/n]`, `⚠ 반복 n회` |
| SessionStart | `session_start.py` | last session-log entries on startup/resume/compact (nearest ancestor project via `project_dir.py`) |
| PreCompact | `precompact_snapshot.py` | writes a `[압축 직전 자동]` entry (recent requests, last answer, touched files, 「다음」) — no output (the harness rejects PreCompact context) |
| PostToolUse Read/MCP | `memory_use_log.py` | logs each memory-file Read with offset/limit |
| Stop | `stop_reindex_v2.py`, `usage_ledger.py`, `repeat_ledger.py --flush`, `hook_change_check.py` | index changed memory docs; injected-vs-opened ledger → `usage` journal rows; guard-blocked repeats → one `gate` observation per session; warn when a changed hook has no receipt |
| PreToolUse | `bash_backslash_guard.py`, `log_label_guard.py`, `unopened_edit_guard.py` | gates: doubled backslashes in Bash; stale session-log time labels; editing an injected-but-unopened memory doc |

`recall_replay.py` / `recall_report.py`: replay real prompts through the hook and report hit rates.

## tools/ (`C:\Users\asm\mcp`)

* `vrs2-import.py` — memory docs / session logs / verdicts into the store (supersedes by revision)
* `swegca-verdict.py`, `swegca-verdict-axes.py` — verdicts (signed v0.2 + vrs2 mirror), axes, `supersedes`
* `vrs2-produce.py` — generic evidence producer; `vrs2-probe.py`, `vrs2-recall-bench.py`, `vrs2-delegate-record.py` — producers
* `vrs2-confirm.py` — the session's current evidence against a recalled proposition (`--holds/--fails`)
* `vrs2-alias.py` — hypothesis registry (alias propositions → canonical)
* `vrs2-delegate.py` — delegation packet (task + receipts + tail) for subagents
* `vrs2-sheet-index.py` — spreadsheets as schema records (headers/types/file lists, never values)
* `vrs2-rebuild-graph.py` — rebuild the graph chain from the journal (`--drop-consolidations`)
* `receipt-use.py` — how often injected receipts were opened, per session transcript

## bench/compaction/

Task cards and scorer for the goal-consistency-under-compaction test (needs a logged-in CLI to
run with `--autocompact 100000`; inputs are not included).
