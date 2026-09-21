# 2.2 evaluation — goal persistence under real compactions, on Antigravity (2026-09-21)

The goal's phase 6: after the repair, run Luna/Terra/Sol-class models at medium thinking against a 100k+ record VRS,
through ten or more **real** context compactions, and grade goal persistence, conversation context, coding quality,
instruction fit and token use; the stock (no-VRS) results are reused as the baseline. GPT-family models were replaced
by the user with Antigravity's Gemini 3.8 Flash, Claude Sonnet 5 and Claude Haiku 4.5 (16:21).

## What could and could not be run

| asked | served on this Antigravity account (`GetAvailableModels`, 17:3x) | used |
|---|---|---|
| Gemini 3.8 Flash, medium thinking | `gemini-3.8-flash-medium` — Gemini 3.8 Flash (Medium) | yes |
| Claude Sonnet 5 | not served; `claude-sonnet-4-6` — Claude Sonnet 4.6 (Thinking) is the newest Claude | Sonnet 4.6 stands in |
| Claude Haiku 4.5 | not served (the enum `MODEL_CLAUDE_4_5_HAIKU` exists in the binary; the account's list has no Haiku) | **no run** — no Antigravity model fills the slot |

Quota: Claude's daily quota fell 1.4 % on two one-line probes, so one full Claude run is what fits before the reset
(13:22 UTC); Gemini's fell 4 % on the 400k-token pilot.

## How a run is made (all in `local/tools/ag_eval.py`, `local/bench/compaction/antigravity/`)

* **Model** — `SendUserCascadeMessage.cascadeConfig.plannerConfig.planModel` (+ `requestedModel.model`) on every
  message; `StartCascade.requestedModel` alone fails with "neither PlanModel nor RequestedModel specified".
* **Real compactions** — Antigravity's checkpointer, `cascadeConfig.checkpointConfig{enabled, tokenThreshold,
  maxTokenLimit, maxUserRequests, strategy}` (defaults 256k/50k Gemini, 160k/50k Claude). A compaction is a
  `CORTEX_STEP_TYPE_CHECKPOINT` (23) step whose `intent_only` is false and whose `session_summary` is filled; the
  prompt-token count of the next planner step shows the shrink (probe at 30k/5k: 19,585 → 15,980 tokens, summary
  1,479 chars). The runs use **100,000 / 10,000**: a pilot at 40,000 compacted after nearly every read (8
  compactions in 65 steps; a 200-line chunk of this Korean log is ≈ 70k Gemini tokens, 50 lines ≈ 18k) and never
  completed a chunk — the same 201–400 range was read three times and lost each time.
* **Task** — the bench's item-extraction task (`ab-compaction`: two session logs, 2,783 lines, 738 truth items, 15
  chunk CSVs, two excluded dates), cards A (no ledger) / B (`progress.txt` ledger) × VRS paragraph off / on, tool
  names `view_file` / `write_to_file`; `make_cards.py` writes them, only the output folder and the two paragraphs
  differ. Inputs sealed by sha256 before the runs (`inputs.sha256.json`).
* **Approvals** — a step left `WAITING` (a shell command's approval, a question) is **denied** with the card's rule
  through `HandleCascadeUserInteraction{permission:{allow:false, userDenyInstruction}}`; the harness never approves
  a model's command. Idle without `report.txt` → at most two "계속" nudges.
* **VRS** — every run's conversation db is tailed into the evaluation store (`vrs2-eval-100k`, the live store's
  11,670 real records + synthetic log entries to 100,000; a daemon of its own) by `vrs2-tail.py --watch --cut
  --interval 3` (the open turn is cut every 3 s into partial rows — the PreCompact hook's role for a hookless
  agent), rows land in the cascade's proposal journal (2.2), and the session ends with `session_end` (merge) when
  the run ends. The MCP bridge runs with `--session-agent antigravity`, so `memory_context` judges that journal
  first (verified end to end on the live daemon at 18:06: `layer: session`, the cascade's own rows with their step
  spans). In the VRS-off condition the card does not mention memory; any MCP call the model makes anyway is counted.
* **Grading** — `score_ag.py`: ① from the bench's `score.py` (same truth), §4-1 constraints and §4-2 read
  discipline from the trajectory (Read = `view_file` on an input, Write = `write_to_file`, scan tools = grep /
  find / run_command / search), §5 one observation per compaction boundary (expected resume point from the last
  written chunk, first read after the boundary → correct / reread / skipped / wrong_file / none, steps to resume,
  a `within_chunk_reread` column because Antigravity reads a chunk in slices, constraint kept, reasked, scope
  drift, items lost in the chunk spanning the boundary, goal restated), §0 gate (`vrs_tail`: a tail receipt before
  the boundary reaching its step; `vrs_recall`: a `memory_context` call in the window after it; `vrs_rows`: the
  daemon's rows for the conversation covering every boundary). `report_ag.py` makes the table.

## Baselines reused (stock, no VRS)

From `GRADING.md` §10 and the continuity card: A2 (no ledger, Claude Code sonnet subagent) recall 0.9851, B2 (ledger)
0.9986 — both **0 compactions**, both 「VRS 없음」, so they anchor ① only; continuity card 1 (Antigravity, 15:xx): B1
(no VRS) 10/10, B2″ (VRS) 10/10 with 0 outside reads.

## Results

(filled after the runs — see the table below)
