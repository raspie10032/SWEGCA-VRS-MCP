# local/ — the Windows Claude Code adapter around the vrs2 store (snapshot 2026-09-18)

Copies of the scripts that run *around* this store on the author's PC. Paths inside them are
machine-specific (`C:\Users\asm\...`); they are versioned here so the branch carries the whole
loop, not only the engine. Logs, ledgers (`*.log`, `*.json` state) and company data are not included.

## hooks/ (`~/.claude/hooks`, wired in `~/.claude/settings.json`)

Since 2026-09-18 these are six-line shims; the bodies are `src/swegca_vrs2/harness/*.py` (see `docs/ADAPTER_SPEC.md`).

| event | script | what it does |
| --- | --- | --- |
| UserPromptSubmit | `recall_context_v2.py` | recall receipts per prompt: verdicts (asks-gated) + one record, 「열기」 line per item, 「※ 재검증 필요」 with both sides, `[열림 m/n]`, `⚠ 반복 n회` |
| SessionStart | `session_start.py` | last session-log entries on startup/resume/compact (nearest ancestor project via `project_dir.py`) |
| PreCompact | `precompact_snapshot.py` | writes a `[압축 직전 자동]` entry (recent requests, last answer, touched files, 「다음」, `로그 위치: <log>#l0-l1` of the turn the tail cuts) and leaves a marker the tail's cut row links back to (item 16, 2026-09-21) — no output (the harness rejects PreCompact context) |
| (no hook) Antigravity | `vrs2-tail.py --register "~/.gemini/antigravity/conversations/*.db" --agent antigravity --format antigravity --idle 15` | the daemon's sweep tails the SQLite conversation databases (protobuf steps) every 15 s; `--show <db> A B` opens a turn (2026-09-21) |
| (terminal) Antigravity driver | `agy.py new/send/meta`, `ag_run.py new/send/wait/steps` | the language server's `agentapi` with the address and CSRF token read from the live process (never printed) and the project id from `~/.gemini/config/projects/`; `ag_run.py` waits for the conversation database to go quiet and prints the steps (2026-09-21) |
| PostToolUse Read/Bash/PowerShell/MCP | `memory_use_log.py` | logs each Read of a memory file or an offered path with offset/limit; a shell read (`sed -n`/`cat`/`head`…) of an offered path counts too (2026-09-21) |
| Stop | `stop_reindex_v2.py`, `usage_ledger.py`, `repeat_ledger.py --flush`, `hook_change_check.py` | index changed memory docs (its own failure / daily success → a `stop-hook` result, pending run-ledger lines flushed); injected-vs-opened ledger → `usage` journal rows; guard-blocked repeats → one `gate` observation per session; warn when a changed hook has no receipt |
| PreToolUse | `bash_backslash_guard.py`, `log_label_guard.py`, `unopened_edit_guard.py` | gates: doubled backslashes in Bash; stale session-log time labels; editing an injected-but-unopened memory doc |
| SessionEnd | `session_end.py` | 2.2 (2026-09-21): tells the daemon the session (producer) finished → its proposal journal (`<state>/sessions/<id>/`, where every write of a live session went) merges into main as one generation with a receipt; a session that ends without the hook merges when idle > 30 min |

`recall_replay.py` / `recall_report.py`: replay real prompts through the hook and report hit rates.

## tools/ (`C:\Users\asm\mcp`)

* `vrs2-import.py` — memory docs / session logs / verdicts into the store (supersedes by revision; binds `metadata.origin` = byte/line span + sha256, G3)
* `vrs2-install.py` — `~/.claude/vrs2.json` + hook shims; `--bundle-limit`, `--bundle ID=DIR`, `--bundle-of SLUG=ID`, `--hot-bundles` (G7 bundles)
* `vrs2-verify-origin.py` — every live file-backed record against its file: intact / moved / changed / missing; `--retire` closes orphans with a superseding retirement row
* `swegca-verdict.py`, `swegca-verdict-axes.py` — verdicts (signed v0.2 + vrs2 mirror), axes, `supersedes`
* `vrs2-produce.py` — generic evidence producer; `vrs2-probe.py`, `vrs2-recall-bench.py`, `vrs2-delegate-record.py` — producers
* `vrs2-identity.py` — signed producers: `keygen` (Ed25519 pair; public key into `~/.claude/vrs2-producers.json`, private key in `~/.claude/vrs2-keys`; `--add` for a second machine's key beside the first), `list`, `audit` (live evidence rows per producer: verified / unverified / legacy / proxy)
* `vrs2-run.py` — a command as a machine result for a hypothesis (G11): axis intervention / counterfactual / observational, `--expect`, tree digest in `metadata.run`, run ledger + `--flush`; body in `harness/results.py`
* `vrs2-tail.py` — real-time transcript ingestion: a conversation log → one row per turn (user words, assistant text, tool calls and errors, results' first line, the user's rejections, compaction boundaries and the host's summary, tokens), bound to its span of the log, secrets masked; `--register "<glob>" --agent NAME` for agents without hooks (the live daemon sweeps registered and tailed logs every 60 s), `--watch`, `--backfill "<glob>"`, `--sweep`, `--status`, `--show LOG FIRST LAST`; formats claude-code / messages-jsonl / messages-json / text; body in `harness/transcripts.py`; the hook shim `transcript_tail.py` runs the same body on Stop / SubagentStop / PreCompact / SessionStart
* `vrs2-confirm.py` — the session's current evidence against a recalled proposition (`--holds/--fails`)
* `vrs2-alias.py` — hypothesis registry (alias propositions → canonical)
* `vrs2-delegate.py` — delegation packet (task + receipts + tail) for subagents
* `vrs2-sheet-index.py` — spreadsheets as schema records (headers/types/file lists, never values)
* `vrs2-rebuild-graph.py` — rebuild the graph chain from the journal (`--drop-consolidations`)
* `receipt-use.py` — how often injected receipts were opened, per session transcript
* `agy.py`, `ag_rpc.py`, `ag_run.py` — Antigravity from the terminal: the live language server's `agentapi` (new conversation by model tier, send, metadata), a LanguageServerService RPC caller (Connect JSON; the CSRF token is read from the process and never printed), the continuity-card driver
* `ag_eval.py` (+ `ag_step_types.json`) — 2.2 evaluation runs on Antigravity: `StartCascade` + `SendUserCascadeMessage` with `cascadeConfig.plannerConfig.planModel` (any served model: `models` lists them with their default checkpointer) and `checkpointConfig{maxTokenLimit, tokenThreshold}` (the compaction knob — the checkpointer compacts for real, `CORTEX_STEP_TYPE_CHECKPOINT` = 23 with a session summary), waits until idle, denies a step left waiting for approval with the card's rule (never approves a model's shell command), optionally tails the conversation into an evaluation store (`--vrs-state/--vrs-receipts`) and ends the session there, dumps `transcript.jsonl` for `bench/compaction/antigravity/score_ag.py`

## bench/compaction/

Task cards and scorer for the goal-consistency-under-compaction test (needs a logged-in CLI to
run with `--autocompact 100000`; inputs are not included). `GRADING.md` is the grading spec; its §0 is the
premise gate — a run without real-time VRS receipts is 「VRS 없음」 and is not scored for continuity.

## bench/compaction/antigravity/

The same task on Antigravity (2026-09-21): `task_template.md` + `make_cards.py` (A/B × VRS paragraph, tool names
`view_file` / `write_to_file`), `score_ag.py` (GRADING on trajectory steps: CHECKPOINT = boundary, `view_file` on an
input = Read, `write_to_file` = Write, `call_mcp_tool memory_context` = recall; `--receipts` / `--state-dir` for the §0
gate). A pilot at `maxTokenLimit` 40k compacted after nearly every read (a 200-line chunk of Korean log ≈ 70k Gemini
tokens) and never finished a chunk; the runs use 100k / 10k.

## bench/vrs2-stream-refine.py

The 10^9-parameter pass (2.2 phase 5b): one `vrs_refine` cycle over a synthetic VRS of 10^9 edges / 10^8 nodes streamed
from the SSD in the FlatGraph in-edge layout (CSR by target, signed strength; 16 threads, fixed per-thread buffers, no
memmaps). Measured 55 s per cycle, 18 M edges/s, 0.34 GB/s, peak working set 2.8 GB (docs/VRS_REGIONS.md, 2.2).

## bench/vrs2-scale-curve.py

Scale curve (2026-09-18): grows a synthetic store (records mixed from live text, no company data
leaves the lab dir) to 10k/20k/50k/100k/200k and records ingest ms/record, consolidation seconds,
regions, edges, recall latency, RSS and disk per step. `--batch 500` uses batch generations
(`docs/VRS_REGIONS.md`, "Batch generations"); `--batch 1` is the old one-record-one-generation
path that the first run measured at 150 → 651 ms/record between 2k and 10k.
