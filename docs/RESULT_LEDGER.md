# Result ledger — measurements on the `vrs-regions` branch (2026-09-14 → 09-18)

Every number below was measured on this PC (i5-13400, 16 GB, Windows 11) on the live store or a
copy of it, and is written in the project session log the day it was taken. This file only
collects them so paper 2 does not have to dig through the log. Source column: session-log
entry (date/tick), commit, or verdict slug in the v0.2 store.

## 1. Store and footprint

| what | value | source |
| --- | --- | --- |
| live store | 5,158 → 5,393 records, ~102k nodes, ~1.05M edges, 23.5 MB on disk | 09-15 20:1x, 09-18 |
| daemon | working set 364 MB, CPU 152 s over 3 h idle-consolidating | 09-17 19:0x |
| full rebuild (`--drop-consolidations`) | 1,146 s for 5,158 rows; compact 3.3 s | 09-15 20:1x |
| consolidation generation (16 cycles, region-wise + connector bundles) | 3.6–5.5 s idle, 8.3 s after usage backfill | 09-15, 09-18 |
| hook recall latency | 100–440 ms wall (store recall 105–180 ms) | 09-15 19:1x, 09-17 |
| hook process start | 650 → 85 ms after lazy store import | 09-15 (47a76e4) |

## 2. Retrieval ranking (BM25 b=.3 + gates)

| set | metric | before → after | source |
| --- | --- | --- | --- |
| tuned 15 known-answer queries | MRR | .550 → .673 (promotion gate, old kernel) → .956 (asks gate) → .843 (description gate, fresh-14 trade-off) → .834 (SWEGCA layer, promotion 0) | 09-15 18:5x–20:1x |
| fresh 14 user-worded questions (`ab-sonnet-main/fresh_questions.py`) | MRR / top-1 / top-3 / hook injection | .416 → .813 / 10 / 13 / 10 of 14 | 09-15 19:1x (29ec709) |
| description gate weight sweep (fresh 14) | MRR at 0/.25/.5/1.0 | .416 / .524 / .605 / .742 | 09-15 19:1x |
| tuned-set inflation | tuned .956 vs fresh .416 on the same rules | warning: questions written to match sealed answers inflate | 09-15 19:1x |
| region scope (auto, 100-row activation) | fresh MRR / latency | .849 @120 ms vs full .813 @180 ms; 0 answers lost (50 rows lost one) | 09-15 19:1x |
| promotion gate on vs off (fresh 14, evidence layer, promoted 0) | MRR | .813 = .813, ranks moved 0 | recall-bench 09-15 |

## 3. Kernel and evidence layer

| what | value | source |
| --- | --- | --- |
| old adapter rule (binary direct, run to convergence) | exactly two strengths: floor .1875 × 1,051,024 edges, cap 3.0 × 4,008 | 09-15 19:1x; refuted by user |
| SWEGCA layer, first live generation | 29 hypotheses, 31 observations, all `abstain: minimum_effective_samples`; record w ∈ {.25, .5}; pending edges .05 → .0426; verdict edges .6875 (w .5) after 3 generations | 09-15 20:1x |
| after producers (probe-runner, pytest-runner, recall-bench, session-main, user-report) | 38 hypotheses, 46 observations, all abstain; 4 hypotheses with source diversity 2–3; one unresolved (directional conflict) then closed by supersedes | 09-15 21:3x, 09-17 19:2x–19:3x |
| record weights | {.25, .5, .75} | 09-17 |
| kernel-tier promotion (strength ≥ 1.0, `retained`) | 0 on 09-15 → 4,589 edges by 09-18 after ~36 h of idle generations (≈139 stable cycles) | 09-18 09:2x |
| latent bug | record states never re-seeded after first consolidation (records are only sources) — fixed 09-18 (ee2cc9e) | 09-18 |

## 4. Producers and revalidation

| what | value | source |
| --- | --- | --- |
| verdicts | 37 live, 0 mixed polarity (`swegca-verdict.py --check`) | 09-17 |
| axes re-declared on 31 verdicts | observational 31, intervention 13, counterfactual 6, cross_context 4 | 09-15 21:1x |
| probes | AF_UNIX absent → refutes "upstream v2 applies"; 12 obs / 1 producer → still abstain | 09-15 21:2x |
| revalidation path | current-vs-past collision rendered with both sides; session refute recorded; old verdict closed by `@rev2026-09-17` supersedes → conflict gone, abstain, unresolved False | 09-17 19:2x–19:3x |

## 5. Delegation (Sonnet subagents)

| set | A: instruction only | B: instruction + receipt | C: hand briefing | source |
| --- | --- | --- | --- | --- |
| dev 6 tasks | 5/6, 19/24 key elements, 551k tok, 759 s, 28 files | 6/6, 23/24, 487k, 753 s, 27 files | 6/6, 24/24, 543k, 628 s, 20 files; my chars 3,243 vs 238 | verdict `attached-recall-receipt-replaces-hand-briefing-for-sonnet-delegation` |
| held-out 6 tasks (rules frozen) | 6/6, 26/26, 560k, 784 s, 26 files | 6/6, 25/26, 518k, 645 s, 18 files | 6/6, 26/26, 608k, 810 s, 30 files; packet held the answer 4/6 | verdict `…-held-out` |
| Sonnet as main, 3 tasks | 3/3 | — | — | `ab-sonnet-main/scoreboard.md` |

## 6. Compaction and hook reliability

| what | value | source |
| --- | --- | --- |
| compaction quiz (09-11) | 12 questions: summary alone 10, recall filled 1, SessionStart injection added 0, lost 1 (a value written nowhere) | verdict `compact-hook-output-rejected-by-harness-schema` |
| PreCompact/PostCompact hook output | rejected by the harness schema; SessionStart(source=compact) injects | same |
| T2M session 91791fe5 (53 prompts, 1 auto compaction at 967k → 13k) | recall injected 50/70; compact-time injection 0 because the cwd was a sub-folder (slug mismatch) — fixed (`project_dir.resolve`); receipts opened 1/22; direct MCP calls 0 | 09-17 16:2x; verdict `hook-project-slug-from-cwd-breaks-in-subfolders` |
| receipt opening habit (4 sessions) | opened/injected docs 1/9, 1/21, 3/4, 2/2; direct MCP 30/0/0/0 | verdict `receipt-path-line-is-rarely-opened-before-use` |
| Sonnet 5 subagent context | 748k and 871k tokens reached with 0 compactions (1M-class window); 1.3 MB of logs read sequentially = 27 min, 874k tokens | 09-16 15:4x–15:5x |
| usage backfill (hook receipts 09-11 → 18) | 255 injected sources, 10 opened; 201 live sources journaled; opened doc edges .0125 → .0669 after 3 generations | 09-18 09:2x |

## 7. Open items the numbers point at

* accept-tier promotion needs ≥ 4 effective samples per axis, ≥ 2 source families, ≥ 4 contexts on the
  same proposition string — a hypothesis registry (aliases) is the practical bottleneck.
* the main-session benefit of the memory layer is measured once (+1/12); the two axes agreed with the
  user are tokens saved and items lost across compaction.
* the goal-consistency-under-compaction benchmark exists as scaffolding (`mcp/ab-compaction/`) but
  has not run with a compaction window (`--autocompact 100000` needs a logged-in CLI).
