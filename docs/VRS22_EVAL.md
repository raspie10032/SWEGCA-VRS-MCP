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

Quota (measured): the account's Claude quota is token-metered at ≈ 570k prompt tokens per period — 16 planner calls of
the Sonnet run exhausted it (see Results); a full run needs ~200. Gemini: ≈ 15 % of the quota per 30-min run (3–4M
tokens); five runs fit.

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

## Results (runs of 2026-09-21 18:54–22:11; store `vrs2-eval-100k`, 100,000 records; checkpointer 100k/10k)

<!-- table -->
| run | model | ledger | VRS | end | recall | missing / gaps | spurious | excl | dup | fmt | chunks | scan tools | outside writes | reasked | rereads / out-of-order / skipped chunks | compactions | resume correct/n | steps | within-chunk reread | constraint kept | items lost | recall called/n | VRS gate | tokens in / out | context peak | duration |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| agA | gemini-3.8-flash-medium | A | off | done, nudges 0 | 1.0000 (738/738) | 0 /  | 0 | 0 | 0 | 0 | 15/15 | 0 | 0 | 0 | 40 / 14 / 1 | 19 | 13/19 | 0.63 | 0.37 | 1.00 | 0 | 0/19 | 0 (tail 1 · recall 0 · rows 1) | 3,032,091 / 449,443 | 99,627 | 1711.0 s |
| agAV | gemini-3.8-flash-medium | A | on | done, nudges 2 | 0.9986 (737/738) | 1 / ME:564 | 0 | 0 | 0 | 0 | 15/15 | 2 | 0 | 0 | 42 / 19 / 0 | 21 | 19/21 | 1.95 | 0.57 | 0.90 | 0 | 2/21 | 0 (tail 1 · recall 0 · rows 1) | 3,800,205 / 483,806 | 92,867 | 1919.0 s |
| agB | gemini-3.8-flash-medium | B | off | done, nudges 1 | 0.9593 (708/738) | 30 / ME:14-16, ME:36, ME:42-46, ME:58… | 26 | 0 | 0 | 0 | 14/15 | 2 | 0 | 0 | 29 / 16 / 1 | 15 | 14/15 | 1.00 | 0.53 | 0.87 | 30 | 0/15 | 0 (tail 1 · recall 0 · rows 1) | 3,620,475 / 352,905 | 98,525 | 1639.0 s |
| agBV-v1 | gemini-3.8-flash-medium | B | on | done, nudges 2 | 0.9201 (679/738) | 59 / SQLITE:550-599, SQLITE:1501-1600 | 1 | 0 | 0 | 0 | 14/15 | 0 | 0 | 0 | 47 / 20 / 1 | 20 | 18/20 | 1.50 | 0.85 | 1.00 | 102 | 2/20 | 0 (tail 1 · recall 0 · rows 1) | 4,258,147 / 447,773 | 96,775 | 1794.0 s |
| agBV | gemini-3.8-flash-medium | B | on | done, nudges 1 | 0.8943 (660/738) | 78 / ME:655, SQLITE:597, SQLITE:601-798 | 1 | 0 | 0 | 76 | 15/15 | 1 | 0 | 0 | 46 / 17 / 0 | 18 | 17/18 | 3.61 | 0.67 | 0.94 | 159 | 4/18 | 0 (tail 1 · recall 0 · rows 1) | 3,932,415 / 499,356 | 97,287 | 1827.0 s |
| agBV-sonnet | claude-sonnet-4-6 | B | on | not done, nudges 0 | 0.0000 (0/738) | 738 / ME:3-777, SQLITE:413-2004 | 0 | 0 | 0 | 0 | 2/15 | 1 | 0 | 0 | 8 / 0 / 12 | 9 | 8/9 | 0.00 | 0.89 | 1.00 | 342 | 0/9 | 0 (tail 0 · recall 0 · rows 1) | 536,376 / 7,712 | 109,684 | 1095.0 s |
<!-- /table -->

Columns: ① = recall over the 738 truth items (bench `score.py`), missing / gaps, spurious rows, exclusion
violations, duplicates, format errors, chunk files present; §4-1 scan tools / outside writes / re-asks; §4-2 rereads /
out-of-order reads / skipped chunks; ② per boundary: compactions (real checkpoints), resume correct / n, mean steps to
the resume read, within-chunk reread rate, constraint kept rate, items lost in the chunks spanning a boundary, boundaries
followed by a `memory_context` call; §0 gate; cost from the trajectory's generation metadata.

### What the runs showed

**The 2.2 write path held under real compactions.** In every Gemini run the tail delivered the last conversation step
before each checkpoint to the session journal 0–3 s after the checkpoint step was created (mean 1.3–1.6 s, the 3 s
poll) and always before the next planner step — agA 19/19, agAV 21/21, agB 15/15, agBV-v1 20/20, agBV 18/18
boundaries (93/93); `session_end` merged each journal into main as one transaction (149 / 170 / 182 / 175 / 173 rows;
the aborted Sonnet run 27) and the daemon's main rows cover every boundary's last line. Nothing of a conversation was
lost between its live journal and main.

**The read path failed the first time it was used for real, and the run is what found it.** In agBV-v1 all four
`memory_context` calls came back `memory_context_cardinality_changed`: the bounded session judgment listed every
candidate in `recall.candidates` while replay / re-evidence held the judged K, and `memory_context` binds evidence by
index across the three. The 18:06 end-to-end check had passed on a journal of two rows (K = 10). Fixed in 68e4f01
(candidates = the judged K, the rest addressable in `unjudged_ids`; a test that fails on the old code with exactly that
reason; verified on the 100k daemon with rows landing every 0.5 s while the bridge reads: 5/5, ~140 ms per
status → context → pages → release). agBV was rerun on the fixed daemon: 4/4 `memory_context` calls succeeded, `layer:
session`, the cascade's own rows with exact step addresses (`#15-15 write_to_file progress.txt`). The v1 row stays in
the table as the run that exposed the defect; it is not a VRS result.

**Whether the agent uses the read path is the weak link, and it is the agent's, not the layer's.** With the tool
working, Gemini called `memory_status → memory_context → memory_release` after boundaries 2–5 of agBV and never
again for the remaining 13, and after 2 of agAV's 21 boundaries; the card's paragraph did not survive the
checkpointer's summaries. When it did call, the
ritual cost 11–14 steps before the resume read (the model also re-reads its own MCP request/response files from the
brain folder) against 1 step in agB — mean steps to resume 3.6 vs 1.0 — and it read only the first page of three rows,
where the bounded judgment ranked the task card's turn first (cue match, not recency); the `progress.txt` write it was
looking for was row 3. The §0 gate is therefore 0 for every Antigravity run: the tail and the rows pass, `vrs_recall`
does not. On Claude Code the SessionStart hook injects the cut turn after a compaction without the model's
cooperation; a hookless agent has no such path, and this is the difference the runs measure.

**Completion quality under 10+ real compactions (Gemini 3.8 Flash Medium).** All five Gemini runs finished with
`report.txt` through 19 / 21 / 15 / 20 / 18 compactions. Recall: **agA 1.0000** (no ledger, VRS off), **agAV 0.9986**
(no ledger, VRS on; one item, ME:564), agB 0.9593, agBV-v1 0.9201, agBV 0.8943 — against the stock anchors A2 0.9851
and B2 0.9986, which had **0** compactions. One run per condition, so the ordering is not a measured effect; what the
runs do show is *how* recall is lost: in whole chunks, not scattered items. agB lost ME 14–196 (chunk_ME_1 written
thin after boundary 10), agBV-v1 lost SQLITE 550–599 and 1501–1600 (two chunks written seconds apart), agBV lost
SQLITE 601–798 to a **format drift right after the boundary-3 recall** — the `file` column became
`SQLITE-session-log.md`, 76 rows unparseable. The two no-ledger runs lost nothing to a boundary: without
`progress.txt` the model re-read after each compaction (resume 13/19 correct + 6 rereads in agA, 19/21 + 2 in agAV;
40–42 rereads in all) and the rereads carried the content across. The ledger runs resumed at the right position more
often (14/15, 18/20, 17/18 — one `wrong_file`: agB re-wrote chunk_ME_401 after boundary 14) but trusted the summary
for the chunk in flight, and that is where the items went. On this task, at this limit, position was never the
problem; the content of the chunk in flight and the format details of the card were. Two habits were the same in
every Gemini run and are unrelated to VRS: the last chunk SQLITE 2001–2005 (5 lines) was skipped by all three ledger
runs and written by both no-ledger runs, and the largest chunk (ME 601–778 ≈ 64k Gemini tokens, more than half the
limit) put every run in a read → compact → re-read loop — 8 reads in 5 min in agB, 8 boundaries without a write in
agBV-v1, 5 min in agA and agAV, 4 reads per chunk in the Sonnet run — until it happened to write. Runaway outputs (a
62,918-token answer cut by the output limit) appeared once or twice per run; the harness's "계속" nudge recovered each.
Shell attempts (`echo "test"` after a rejected write, a `Select-Object` on the brain transcript) were denied by the
harness with the card's rule and count as scan-tool violations (agA 0, agAV 2, agB 2, agBV-v1 0, agBV 1).

**Cost.** VRS on added prompt tokens (ledger: 3.62M → 3.93M / 4.26M, +9–18 %; no ledger: 3.03M → 3.80M, +25 %) and
output tokens (353k → 447k–499k; 449k → 484k), partly the recall ritual, partly the model's habit of reading its own
tool I/O; context peaks were the same (93–100k, the limit). Duration 27–32 min per Gemini run. On the store side a session-layer hit is ~0.4–0.8 ms in-process and ~1 ms over the
loopback (2.2 measurements, 17:0x); main's complete-miss path on the 100k synthetic store is 4–7 s because the synthetic
rows mix real words and common cues match 35–56k candidates — bounded top-K is deliberately not applied to main (it
would bypass region / connector navigation), and no run hit the miss path for a cue-bearing query.

**Claude Sonnet 4.6 could not complete a run on this account.** The quota is token-metered: 16 planner calls (536k
prompt tokens, 109k peak — the 200-line Korean chunk plus thinking exceeds the 100k limit in Claude's tokenization, so
the checkpointer fired after every read and each chunk was read four times before it was written) took the daily
quota from 0.961 to 0.025 (RESOURCE_EXHAUSTED 429 at 21:02, 17 min, 9 compactions, chunks 2/15, no recall call, one
`find_by_name`). A full run needs ~200 calls; the account's Claude quota is ≈ 570k tokens. The run is recorded as
aborted, not as a result. Claude Haiku 4.5 is not served on the account (no run). The 100k/10k checkpointer, chosen
from the Gemini token measurements, is tighter for Claude and would need a Claude-specific setting before any
comparison across models is fair.

**Against the goal's criteria.** 10+ real compactions per run: met (19 / 21 / 15 / 20 / 18). Goal persistence and
instruction fit: the task was completed in every Gemini run; position survived every compaction (by ledger or by
re-reading); the card's format and its VRS paragraph decayed across compactions. Conversation context: in the store at
every boundary (gate `vrs_tail` / `vrs_rows`, 93/93), read by the agent at 4/18 and 2/21 boundaries. Coding quality is
not exercised by this task. Token use: measured per run above. What the runs did not show is a VRS effect on ① or ②
for a hookless agent — with the read path fixed, the layer answered correctly every time it was asked, and it was
asked at 6 of 39 boundaries; the single best run (recall 1.0000, 19 compactions) had neither a ledger nor VRS.

### Runs and artefacts

`C:\Users\asm\mcp\ab-compaction\<run>\` holds each run's `run.json` (cascade id, model, card sha256, checkpoint
config, tail pid, `session_end` / `origins` receipts), `transcript.jsonl` (one record per step), `summary.json`,
`score.json`, `tail.log`, `report.txt` and the chunk files; `observations.jsonl` has one line per boundary (§5-2
columns plus `last_conv_line`, `tail_lag_s`, `vrs_row_covers`); `GRADING.md` §0 carries the Antigravity gate arithmetic.
The failed and aborted runs are kept as `agBV-v1` and `agBV-sonnet` (`run.json.aborted` says why).
