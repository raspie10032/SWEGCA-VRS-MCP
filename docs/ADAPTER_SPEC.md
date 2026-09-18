# Adapter spec — the loop around the store, harness-independent (2026-09-18)

The store (`swegca_vrs2`) is one contract: journal, evidence accumulator, kernel, recall packet, loopback
daemon. The *loop* around it — what a harness must do at which moment — was scattered across Claude Code
hook scripts. This spec names that loop as six interception points so that any harness (Claude Code
hooks, an API driver for another model, another CLI) implements the same behaviour and the harness stops
being a variable when models are compared. `swegca_vrs2.adapter` is the API; the bodies live in `swegca_vrs2.harness.*` (one module per point or gate,
receipts under `~/.claude/hooks` or `$VRS2_RECEIPTS`); `local/hooks` are the Claude Code shims (six lines each,
`from swegca_vrs2.harness.<module> import main`); `local/tools/vrs2-harness-demo.py` runs one full turn with no Claude Code.

## Interception points

| point | when | payload | returns | receipt (one line per call) |
| --- | --- | --- | --- | --- |
| `before_prompt` | before the model sees a user prompt | prompt, cwd, session_id | context text or None | `recall_context.log` `{injected|skip, session, opens}` |
| `on_read` | after the model reads a memory file | path, offset, limit, session_id | bool | `recall_context.log` `{use: read, path, offset}` |
| `before_action` | before a tool runs | tool, tool_input, session_id | None or deny reason | `repeat_ledger.log` on deny |
| `before_context_loss` | before the harness compacts/truncates context | transcript path, cwd, session_id, trigger | the log entry written or None | `precompact_snapshot.log` `{written|skip}` |
| `after_context_loss` | first thing after compaction (and on start/resume) | cwd, source, session_id | context text or None | `session_start.log` `{source, entries, session}` |
| `on_stop` | when the model's turn ends | cwd, session_id | per-step results | `stop_reindex_v2.log`, `usage_ledger.log`, `repeat_ledger.receipts.log`, hook check |

Evidence entry points (not tied to a moment): `emit(producer, hypothesis, outcome, axes, context, source, …)`,
`confirm(hypothesis, holds, evidence_path, session_id)`, `alias(canonical, aliases)`.

## What each point must contain

* `before_prompt`: verdicts gated by their asks (rare words), one record of the current project, a
  「열기」 line per item (`Read file_path offset limit`, or `memory_read <id>` for a verdict; 「토막이
  전문」 when the snippet is the whole record), 「※ 재검증 필요」 with both sides of a same-proposition
  conflict and the exact `confirm` call, `[열림 m/n]` usage counts, `⚠ 반복 n회·세션 m개`.
* `before_action`: the gates in force — doubled backslashes in Bash, stale session-log time labels,
  editing a memory doc that was injected in this session and never opened. A denial names the fix and
  is counted as a repeat of the verdict that settled the mistake.
* `before_context_loss`: recent requests, last answer gist, touched files, the 「다음」 line (and any
  later request), appended to the project's session log as one `[압축 직전 자동]` entry.
* `on_stop`: index changed memory docs; reconcile injected-vs-opened into usage counts and push them;
  flush guard-blocked repeats as one `gate` observation per session; warn when a changed hook has
  left no receipt since.

## Degradation rules

A harness that lacks a moment does not "not support" the point; it degrades in a stated way:

| missing moment | degradation |
| --- | --- |
| no pre-prompt injection | the harness prepends `before_prompt()` itself; if it cannot, it still calls `on_stop()` (ledgers and index stay current for the next harness that can inject) |
| no pre-action hook | call `before_action()` after the action; the denial cannot prevent, it is still counted |
| no context-loss event | call `before_context_loss()` from `on_stop()` every N stops (periodic snapshot) |
| no read event | usage stays at "injected", never "opened" — the report must say so |

## Conformance

`adapter.conformance(session_id)` counts the receipts each point left for a session. A harness
conforms to a point when it leaves that receipt; zero means the point is not wired. The demo harness
scores 6/6; Claude Code scores 6/6 once a compaction has happened (`before_context_loss` fires only
then). This count is the first column of any cross-harness comparison table.

## Portability (2026-09-18)

Nothing in the store is OS-specific (TCP loopback, numpy, sqlite). The harness resolves every path
through `swegca_vrs2.harness.paths` — environment `VRS2_*` → `~/.claude/vrs2.json` → defaults — and the
tools through `_vrs2_env.py` with the same order; `vrs2-install.py` writes the json, the shims and the
settings.json hook entries for the current machine (the interpreter path is the one thing that differs
per OS: `venv\Scripts\python.exe` vs `venv/bin/python`). The daemon is spawned detached on Windows and in
a new session on POSIX. The memory-doc regex accepts Windows and POSIX absolute paths. The doubled-
backslash gate is on only where its cause was measured (the Windows Bash tool) unless
`VRS2_BACKSLASH_GUARD=1`. Claude Code's project slug rule (every non-alphanumeric → `-`) is the same on
all three OSes. **Not yet run on Linux or macOS** — this machine has neither; the OS-neutral paths are
covered by unit tests only.

### Installing on Linux or macOS (first real run planned 2026-09-19)

Python 3.11+. The repo carries everything the harness needs under `local/` (shims, tools, bench);
`torch` is an optional `core` extra and is not required for the harness or the store.

```bash
git clone -b vrs-regions https://github.com/raspie10032/SWEGCA-VRS-MCP.git && cd SWEGCA-VRS-MCP
python3 -m venv ~/vrs2-venv && ~/vrs2-venv/bin/python -m pip install -e ".[test]"
~/vrs2-venv/bin/python local/tools/vrs2-install.py      # finds --src from <repo>/local/tools; the interpreter that runs it becomes the hooks' interpreter
~/vrs2-venv/bin/python -m pytest tests/standalone -q     # 46 on Windows
~/vrs2-venv/bin/python local/tools/vrs2-harness-demo.py  # one turn without Claude Code: conformance 6/6 on a temp store
```

Then one real turn in Claude Code from any project. Three receipts say the loop is alive:
`~/.claude/hooks/recall_context.log` gets a row per prompt, `~/.claude/hooks/stop_reindex_v2.log` a
row per stop, and `<state>/loopback.port` exists (the daemon spawned). The store starts empty, so
injections appear only after a few session-log entries have been indexed by the Stop hook.

Where a first POSIX run is most likely to break (all untested guesses, in order of suspicion):

1. the daemon's detached start (`start_new_session`) finishing inside the hook's 30 s budget;
2. the project slug — Claude Code's POSIX slug (`/home/u/proj` → `-home-u-proj`) has never been
   seen on a real machine here, only assumed in `project_dir.slug_of`;
3. quoting of the hook commands `vrs2-install.py` writes into `settings.json` under `sh`;
4. `swegca-verdict.py` / `swegca-verdict-axes.py` need the v0.2 store paths (`--v02-*` at install);
   without them the verdict tools do not run — everything else does.

Bring back the receipt files and the traceback; nothing else is needed to diagnose.
