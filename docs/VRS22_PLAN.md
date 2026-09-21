# VRS 2.2 — the session producer layer, composed after the SWEGCA architecture (2026-09-21)

Status: plan, then the ledger of what was built and measured. Vocabulary follows
`Desktop/SWEGCA-Architecture/paper/swegca/ARCHITECTURE_SPEC.md` and `TERMINOLOGY.md`;
the numerical core (vrs-regions) already runs the vendored accumulator/arbiter rules
(`vrs_evidence.py`). 2.2 adds the *producer boundary* the architecture draws around every
session, and nothing of the existing store — region division, connector/portal, shared
experience, main — is removed or bypassed.

## 1. The ask (goal text, 2026-09-21)

* every session's content enters VRS as experience in real time, not as a log;
* an active session writes only to a temporary VRS; reads go temporary first, main only on a
  complete miss; the temporary VRS merges into main only after the session ends;
* the SQLite conversation outbox goes; the existing VRS experience is used as is;
* Déjà vu → Recall → Replay → Re-evidence stay, with origin, exact address, source,
  uncertainty and refutation preserved;
* region division, connector/portal, shared experience and main logic are neither deleted
  nor bypassed;
* 16 threads, ≤ 4 GB RAM, 5 Gbps SSD, ≤ 500 GB store;
* lookup → Replay complete < 1 ms regardless of main size; a 10^9-parameter VRS experience
  pass in seconds;
* afterwards: evaluation with Antigravity's Gemini 3.8 Flash, Claude Sonnet 5 and Claude
  Haiku 4.5 (the GPT-family Luna/Terra/Sol are replaced by these — user, 2026-09-21 16:21),
  medium thinking, 100k+ VRS, ≥ 10 real context compactions; stock (순정) results reused.

## 2. SWEGCA roles → 2.2 objects

| SWEGCA (spec §) | 2.2 object | where |
|---|---|---|
| Single-World persistent Cognitive State `S_t`, one owner (§4.1) | the **main** bundle — unchanged | `store.Main` at `VRS2_STATE` |
| authority-limited producer `i`, request-local, works on `detach(S_t)` (§3.1, §4.3) | a **session** (Claude Code session id, Antigravity conversation id) | `session_producer.SessionProducer` |
| the producer's transient proposals `p_{i,t}` — a candidate delta, never a second Cognitive State | the session's **proposal journal**: its own small store under `<state>/sessions/<id>/` (journal + hot index + VRS graph, so the four stages run over it unchanged). It is not authoritative: main never reads it; it exists to be merged or dropped | `SessionProducer.main` |
| `Select(q_t, U_t) → (C_t, J_t, ρ_t)`, `Authority(ρ_t) = ∅` (§3.1, §4.2) | `hook_recall(session=…)`: **session first** (Déjà vu → Recall → Replay → Re-evidence over the proposal journal); on a **complete miss** (no query cue has a posting in the session index) the lock-free read of main's current generation; the packet is the selection receipt, tagged `layer` and `authority: none` | `loopback.hook_recall` |
| evidence accumulation → `D_t ∈ {accept, reject, abstain}` (§4.4) | the vendored accumulator, run by consolidation over main after the merge — unchanged | `vrs_evidence`, `engine.mosaic_evidence_accumulator` |
| Single-World Arbiter, conflict abstention (§4.6) | at merge, rows of the same slot (same source address, or the same proposition) with opposing direction from *different* producers are all **admitted as experience** (status-unfiltered, §4.2) but the slot's decision is `conflict`/`abstain` — no truth is committed; the receipt names the slots | `merge.arbitrate` |
| bounded commit with receipt `R_t` (§4.7): before/after state hashes, applied-delta hash, evidence refs | the **merge**: the session's rows enter main as *one* batch generation (`ingest_many`); the receipt binds `before_pair`, `after_pair` (pair snapshot ids are the state hashes), `delta_digest` (the rows' fingerprints in order), `evidence_refs` (request ids + origins), the producer (session) and the counts | `merge.MergeTransaction` |
| journaled promotion protocol (§4.8): `prepared → memory_committed → state_committed → completed`; rollback / recovery of incomplete journals at startup | `merge_journal` table in main's SQLite. `prepared`: the session journal is read and its digest fixed; `memory_committed`: main's `ingest_many` transaction committed; `state_committed`: every request id of the session verified known in main with the same fingerprint; `completed`: the session directory removed. Daemon start recovers: `prepared` → re-run (idempotent) or roll back (journal row → `rolled_back`, session kept); `memory_committed` → verify and complete | `merge.recover` |
| rollback / retraction (§4.9) | a merge is all-or-nothing in one SQLite transaction; a session whose rows were refused (`request_id_reused_with_different_content` = another producer already committed that slot) is **not** merged silently: the conflicting rows are re-issued as superseding revisions of the *same* source (the record keeps both, as the existing `reissue_row` does for re-cut turns) and the receipt lists them | `merge.reissue` |

What stays exactly as it is: `store.Main`, `flat_vrs`, `fast_regions` (region division), portals and
region navigation in `Main.recall`, the shared-experience path (other bundles via `resident.Resident`),
the transcript adapters (`harness/transcripts.py`), the origin binding (`harness/origin.py`), the
signed producers (`harness/identity.py`).

## 3. Phases

1. **Cut** (user, 2026-09-21 16:3x: "헤르메스 코드도 싹 없애버려. 필수가 아닌건 전부 쳐내"): the legacy
   `src/swegca_vrs_mcp/` package (Hermes adapter with its delivery outbox, agent service/client,
   core server, stateful core, runtime, plasticity — the "SQLite conversation outbox" path), its
   tests, tools, examples, integrations and docs. The package is not imported by `swegca_vrs2`;
   the verdict tools use the v0.2 package from the *other* repository (`V02_SRC`).
2. **Session producer** (`session_producer.py`): open/close a session's proposal journal; route
   `ingest`/`ingest_many` with `session=` to it; `hook_recall` session first, main on a complete
   miss; `sessions` status; LRU of open sessions (`SESSION_HOT`), closed sessions stay on disk
   until merged.
3. **Merge transaction** (`merge.py`): journal table, stages, receipt, arbitration, re-issue,
   recovery; triggers: `session_end` command (SessionEnd hook), the sweeper (a session idle
   longer than `SESSION_IDLE_S` whose log has not grown), daemon start (recovery + idle sessions).
4. **Hooks**: `transcript_tail.py`/`stop_reindex_v2.py`/`recall_context_v2.py` pass `session`;
   a SessionEnd hook calls `session_end`; `vrs2-tail.py` (Antigravity) passes the conversation id.
5. **Measure**: (a) lookup → Replay on the session layer, in-daemon (`judged`, `rowed` marks) and
   end to end through the socket, at 10 / 100 / 1,000 session rows with a 200k-row main —
   the target is < 1 ms in-daemon on a hit; a miss costs main's recall and is reported as such;
   (b) a 10^9-parameter pass: a streaming settle over memmapped edge arrays in 16 threads,
   RSS ≤ 4 GB, timed against the SSD's bandwidth (a 4 GB float32 array at 625 MB/s is 6.4 s
   of I/O alone — the honest floor).
6. **Evaluate** (after 1–5): the continuity harness (`local/bench/continuity/`) with Antigravity
   Gemini 3.8 Flash, Claude Sonnet 5, Claude Haiku 4.5; medium thinking; ≥ 100k VRS rows;
   ≥ 10 real compactions; scored on goal persistence, conversation context, coding quality,
   instruction fit, token use; stock results reused as the baseline.

## 4. Ledger

(filled in as each phase lands — commit, tests, numbers)
