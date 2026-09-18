# Adapter spec — the loop around the store, harness-independent (2026-09-18)

The store (`swegca_vrs2`) is one contract: journal, evidence accumulator, kernel, recall packet, loopback
daemon. The *loop* around it — what a harness must do at which moment — was scattered across Claude Code
hook scripts. This spec names that loop as six interception points so that any harness (Claude Code
hooks, an API driver for another model, another CLI) implements the same behaviour and the harness stops
being a variable when models are compared. `swegca_vrs2.adapter` is the reference API; `local/hooks` is the
Claude Code implementation; `local/tools/vrs2-harness-demo.py` runs one full turn with no Claude Code.

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
