# v2.1.0 standalone main on Windows at a real store size: cost grows with the store, restart replays everything, recall ranking on Korean text — measurements and a working local adapter

> 아래는 업스트림 이슈로 올릴 영문 본문이다. 한국어 요약: v2.1.0 을 Windows·실제 스토어(세션 로그 2,287·문서 306·판정 27 = 2,620건, 한국어)에 붙여 보니 ① 적재가 스토어 크기에 비례해 느려지고(30번째 2.2 s, 137건에서 건당 10 s) ② 관측 저널만 저장해 재기동이 전량 재생(40건 69 s)이며 ③ 연결 영역을 적재마다 전체 재구축하고 ④ 무손실 압축·아카이브가 없어 디스크 284 MB ⑤ 회수 순서가 한국어 조사·활용형에 흔들리고 VRS 점수를 쓰지 않는다. 규칙은 그대로 두고 표현·계산·저장만 바꾼 로컬 브랜치(`local-windows-scale`)에서 정산 비트 동일을 확인한 채 적재 0.65 s/건·재기동 1.6 s·디스크 30.7 MB 로 내렸다. 각 항목에 원인 위치·실측·제안을 적었다.

## Environment

* Windows 11 Pro 10.0.26200, CPython 3.11.16 (uv-managed), numpy/scipy in a venv, no torch.
* Upstream tag `v2.1.0` (`7395deb`), package `swegca_vrs2`, `engine/` untouched.
* Store: 2,620 real records — 2,287 session-log entries (Korean, median ~1,000 chars), 306 memory-doc sections, 27 verdicts with explicit propositions. Ingested through `Main.ingest` only; no transcripts.
* Client: Claude Code hooks (UserPromptSubmit / Stop) + the stdio MCP server, i.e. several short-lived processes wanting the same state directory.

Everything below was measured on this machine; scripts and numbers are in the local branch `local-windows-scale` of this clone (`docs/WINDOWS_SCALE.md`, `tools/compare_flat_vrs.py`).

## Summary

| # | Issue | Symptom at our size | Where in v2.1.0 |
|---|---|---|---|
| 1 | No Windows transport for a resident main | v2.0 transports need `AF_UNIX`; v2.1 bundles the main in-process but leaves one process per state dir (`owner.lock`), so hooks and the MCP server cannot share it | `native_transport.py`, `store.Main` owner lock |
| 2 | Ingest cost grows with the store | record 1: 175 ms → record 30: 2,216 ms (+~70 ms per stored record); ~10 s/record at 137 records over stdio | `engine/mosaic_vrs_event_signal.settle_event_signal` reached through `store.Graph.append` |
| 3 | Connectivity regions rebuilt in full on every ingest | 5.9 s of a 32.2 s / 5-record profile; the whole component is one region build | `engine/...connectivity_regions.build` called per ingest |
| 4 | Only the observation journal is persisted | reopening a 40-record state: 69.1 s (full replay); 2,620 records would be hours | `store.Main.__init__` replays every journal row |
| 5 | No lossless compaction / archive | 284 MB on disk at 2,620 records; 85k nodes / 660k edges at 1,275 records; superseded revisions stay hot; nothing bounds the journal | `HotIndex`, journal table, string node names |
| 6 | Recall order on real (Korean) text | one-character particles (`왜`, `안`: 38 % of records) open proposition closure to the whole store; Jaccard order puts short records above the rich one that matches most of the question; VRS strengths are computed but never used for ordering; mixed-script tokens (`270인지`, `모델8`) get neither a split nor substrings | `store.keys`, `store.Main.recall`, `recall_memory` |
| 7 | Benchmark basis in the docs | "22 ms median" comes from 50 short synthetic records; real records with hundreds of distinct tokens behave differently by two orders of magnitude | `docs/VRS2_STANDALONE_VALIDATION.md` |

None of these is a wrong rule; they are representation, persistence and ordering choices that stop holding once records share hundreds of cue nodes. The local adapter below keeps every rule (bit-identical settlement, same receipts, same contracts) and changes only how the same arithmetic is stored and executed.

---

## 1. Windows has no `AF_UNIX`; one process owns the state

v2.0.0 added three transports that all bind Unix sockets (`native_transport.py:82`, `agent_service.py:174`, `agent_client.py:24`). Windows CPython 3.11 has neither `socket.AF_UNIX` nor `socketserver.UnixStreamServer`, so `agent_service` does not import. v2.1.0 bundles the main in-process, which makes the standalone tests pass here (21/21), but a state directory is still owned by exactly one process (`owner.lock`). Our setup has three clients — a prompt hook, a stop hook and the stdio MCP server — and each would need to open the store, which the lock forbids and which would cost a full open (0.4 s at 800 records, seconds at 2,620) per hook call.

What we did: `loopback.py` — one resident main bound to `127.0.0.1` on an ephemeral port written to `loopback.port`, the resident protocol commands plus `hook_recall` / `ingest` / `checkpoint` / `ping` / `shutdown`, persistent connections (a `memory_context` call went from 1.7 s to 60 ms once the 197 per-page connections became one), idle shutdown after 8 h, `ensure_daemon()` that spawns it detached if absent; `server.py --loopback` turns the stdio server into a thin bridge. `sqlite3.connect(..., check_same_thread=False)` was needed because the server is threaded; the daemon serialises requests with one lock.

Suggestion: ship a loopback TCP (or named-pipe) resident transport for platforms without `AF_UNIX`, and let the stdio server bridge to it, so several hook processes can share one main.

## 2. Ingest cost grows linearly per record (quadratic overall)

Measured with real session-log chunks (median 992 chars, Korean):

| record # | `Main.ingest` | note |
|---|---|---|
| 1 | 175 ms | |
| 30 | 2,216 ms | ≈ +70 ms per already-stored record |
| 105 (stdio) | 4 records / 20 s | |
| 137 (stdio) | ~10 s per record | |

Profile of the last 5 ingests at that point (32.2 s total): `settle_event_signal` 23.9 s with `_same` called 4.07 M times and numpy scalar `.view` 8.1 M times; `connectivity_regions.build` 5.9 s; the rest is `HotIndex` string work.

Cause: the incremental design is present (`prepare_event_delta` appends only new nodes/edges, `settle_event_signal` receives `changed_nodes`, immutable successors), but `keys()` turns every Hangul word into all 2–4-character substrings, so a real record shares hundreds of cue nodes with most stored records. The settlement therefore propagates from the new record through cue nodes to nearly the whole graph each round, and each round is a per-element Python loop over numpy scalars (`_same`, `.view`, `tanh` one node at a time), up to 512 rounds.

What we did (`flat_vrs.py`): the same rule — `next = f32(.8·old + .2·tanh(direct + .2·signal/max(1, Σ|strength|)))`, synchronous rounds, a node whose float32 bits changed revisits its outgoing targets, ≤ 512 rounds, values applied only to pending nodes — expressed over flat immutable arrays with the per-round signal as one `scipy.sparse` CSR matvec and the pending-set propagation as an adjacency product. `tools/compare_flat_vrs.py` drives the tagged v2.1.0 `store.py` (via `git show v2.1.0:src/swegca_vrs2/store.py`) and the flat engine on the same real records for 20 generations: **0 bit differences** in the settled vectors; 77× faster at 4k nodes. At 800 records (60k nodes, 500k edges) ingest is 0.6–0.7 s per record; at 2,620 records through the daemon ~2.3 s per record.

Suggestion: keep the trie/dict representation as the interface if you like, but settle over a flat CSR view; and consider whether 2–4-char Hangul substrings must become VRS nodes at all (they are needed as retrieval keys; as graph nodes they are what connects everything to everything — see §5).

## 3. Connectivity regions rebuilt in full on every ingest

`build` runs the whole Louvain-style procedure over the entire component on each ingest. With one giant component (everything is connected through cue nodes) that is the whole graph, in Python loops: 5.9 s per ingest at ~140 records, growing.

What we did (`fast_regions.py`): components ≤ 2,000 nodes use the engine's `ConnectivityRegions.build` unchanged; larger ones run a few synchronous (even/odd) local-move sweeps with the engine's gain and tolerance, then finish with the engine's sequential rule on the remaining active set (which guarantees convergence — a purely synchronous version oscillated), warm-started from the previous generation's labels; multi-level aggregation, association-mass memberships and the topology digest are the engine's. Modularity on a 6.6k-node component: engine 0.5231 in 691 ms, cold 0.5233 in 135 ms, warm 0.549 in 92 ms. Regions are built per changed component at ingest and unrelated components share their frozen arrays; `graph.regions` / `graph.components` keep the `Map` contract and the receipt gains `region_backend`, `region_sweeps`, `changed_component_nodes/edges`.

Suggestion: rebuild only the changed component (the receipt already knows it) and warm-start from the previous labels.

## 4. Restart replays the whole journal

`Main.__init__` reads every observation row and re-runs `ingest` for each, because the settled VRS state, the hot index and the regions are never persisted. Reopening a 40-record state took 69.1 s; our 2,620-record store would take hours, and every hook invocation is a process start.

What we did (`store.Main`): a `checkpoint` table (pickle protocol 4 + zlib, magic `Z1`) written every 8 ingests and on `close()`, holding the compact index, the flat generation, the graph, the request table and the pair id. On open the checkpoint is loaded, **every** journal row (archived and live) is re-validated against its fingerprint and request id exactly as before, and only rows after the checkpoint are replayed. Restart: 69 s → 16–30 ms at 40 records, 1.6–2.2 s at 2,620 (checkpoint 22–28 MB after the compression in §5). A commit failure truncates the successor index so the in-memory state is bit-exactly the pre-ingest one (a test caught the case).

Suggestion: persist the settled generation (it is already immutable) with the journal as the source of truth and fingerprint re-validation on load — the current "journal only" rule can be kept as the *recovery* path.

## 5. Nothing is compacted: disk, memory and node count

At 1,275 records: 178 MB on disk, 85k nodes and 660k edges, importer working set 424 MB. At 2,620 records with v2.1.0's layout: 284 MB on disk; `HotIndex` holds every cue string per record, node names are Python strings, superseded revisions stay in the hot index and postings, and the journal only grows. There is no archive, no way to drop a superseded revision from the hot path while keeping it addressable, and (before §4) no checkpoint at all — our first pickled checkpoints were 130 MB every 8 records.

What we did:

* `compact_index.py` — a dictionary-coded replacement for `HotIndex` with the same interface (`episode`, `episode_ids_for_cue`, `records`, `append`, `propositions`, `superseded`, generation visibility by count): vocabulary in 128-string zlib blocks (partial decompression, string→id through a sorted 64-bit hash table with verification), each record's cues as a `uint32` array, postings as sorted `uint32` rows (CSR), text/metadata as one zlib blob per record decompressed only on replay, `MemoryEpisode` rebuilt on demand behind an LRU (4,096). Bulk conversion from an old `HotIndex` checkpoint (`from_hot`).
* `Graph.nodes` as a `NodeDirectory` (cue-id arrays, names decoded lazily) and region `terms` as `LazyTerms`; region objects share edge arrays instead of copying them.
* `Main.compact()` — moves the journal into 64-row zlib segments (`journal_archive`, magic `A1`), keeps the last N live rows, `VACUUM`; archive + live rows are the complete journal, so §4's re-validation and replay still pass (lossless), and one archived row can be read in 1.5 ms without touching the rest.

Numbers at 2,620 records: index 11 MB (vocab 1.1, ids 3.3, postings 3.3, blobs 3.3); checkpoint 142.5 MB → 28.2 MB (22 MB with protocol 4 and no blob-view retention); disk 284 MB → 30.7 MB; daemon RSS 286–346 MB (Python-tracked ~162 MB, of which arrays 116 MB); warm `hook_recall` 72–130 ms.

Suggestion: a compact, dictionary-coded index and a journal archive belong in the store proper; both are lossless and keep every address reachable, which is the contract the docs already state.

## 6. Recall ordering on real Korean text

`Main.recall` generates candidates from the union of all matching lexical keys (no top-k) and orders them by cue-set Jaccard. On this store:

* One-character particles are keys: `안` is carried by 1,005 of 2,620 records (38 %), `왜` similarly. Because closure opens through *any* matched cue, a single particle pulls every proposition in the store into the candidate set, and the `proposition:` cues it adds then inflate unrelated records' match counts.
* Jaccard penalises the rich record: for "why did the compact hook not work" the right verdict matched the most cues (8) and ranked 7th behind short records.
* Cues are sets, so tf is always 1; several instruction words (`설명해`, `이유를`, `건드리면` — each rare as an exact inflected form) outweigh one topical identifier.
* Mixed-script tokens are single keys: `270인지` never matches a record that says `big_chunk 270`; `모델8` (a Hangul word with a trailing digit) gets no Hangul substrings.
* VRS strengths and promotions are returned in the receipt but never influence candidate order.

What we did (local adapter in `Main.recall`, all reported in `memory_selection` so a client can see the rule): closure opens only through *informative* cues (fanout below half the store); candidate order = BM25 over matched informative *words* (a matched Hangul n-gram that is a substring of a longer matched cue is the same word, scored once by its longest form; idf from postings fanout; length norm from the record's cue count; k1 1.2, b 0.3), then the engine's Jaccard. `keys()` additionally emits the script runs of mixed tokens (`270인지` → `270`, `인지`). Measured over 15 known-answer queries: MRR 0.625 (b 0.75) → 0.694 (b 0.3), top-3 10 → 12 of 15; a "rare anchor word first" tier was tried and was worse (3/8 vs 6/8) because inflected instruction words are as rare as identifiers. The candidate *set* is unchanged; the engine's statement that order is not semantic acceptance still holds.

Suggestion: a function-word rule based on fanout (not a stop list), word-level rather than n-gram-level scoring, script-run splitting in `keys()`, and — the real gap — some use of the VRS strengths the engine already computes.

## 7. Benchmark basis

`VRS2_STANDALONE_VALIDATION.md` reports a 22 ms median on 50 short synthetic records. With real records (hundreds of distinct tokens each, heavy cue sharing) ingest is 100× slower at 30 records and still growing, and restart is minutes. It would help to publish numbers on a corpus with realistic token overlap, or to state the corpus assumptions next to the numbers.

---

## What the local branch changes (for reference)

Branch `local-windows-scale` on top of `v2.1.0`; `engine/` untouched; 8 files, +1,859/−105 in the first commit, then compression and recall commits (`f174619`, `a35612d`, `aa541dd`, `3a4ef5e`, `cba320c`, `bde7303`, `cd58736`, `0e1f03c`, `dc48e67`).

| Piece | Rule kept | Change |
|---|---|---|
| `flat_vrs.py` | settlement arithmetic, rounds, pending propagation, ≤512 | flat immutable arrays, CSR matvec per round; bit-identical over 20 generations |
| `store.Graph` | node/edge construction, direct .1, same-proposition re-evidence ×1.01/×.995/conflict abstain, settled-snapshot ids, receipts | `NodeDirectory`, per-component regions, shared frozen arrays |
| `fast_regions.py` | engine gain/tolerance, aggregation, memberships, digest | synchronous sweeps + engine sequential finish, warm start, only for components > 2,000 nodes |
| `store.Main` | journal is the source of truth; every row re-validated on open; single owner | checkpoint table (zlib pickle), journal archive segments, `compact()` |
| `compact_index.py` | `HotIndex` interface and generation visibility | dictionary-coded index, per-record blobs, LRU episodes |
| `store.Main.recall` | all matching keys, closure, four stages, hot path without I/O/JSON/hash | closure through informative cues only; BM25-over-words order; rules in `memory_selection` |
| `loopback.py`, `server.py --loopback` | resident protocol | loopback TCP resident main + stdio bridge for Windows |

Verification: `tests/standalone` 21/21 on every commit; `tools/compare_flat_vrs.py` bit-identical against the tagged store; identity/snapshot/conflict/duplicate/restart behaviour unchanged.

Measurements (this machine):

| records | nodes | edges | ingest | restart | query |
|---|---|---|---|---|---|
| 40 | 6.6k | 25k | 50–120 ms | 16–30 ms | 2 ms |
| 805 | 60k | 500k | 0.6–0.7 s | 0.4 s | 22 ms |
| 2,620 | 168k | 1.63M | ~2.3 s (daemon) | 1.6–2.2 s | 72–130 ms warm (`hook_recall`) |

Disk at 2,620: 284 MB (v2.1.0 layout) → 30.7 MB after `compact()`.

## Reproduction

```
git clone https://github.com/raspie10032/SWEGCA-VRS-MCP && git checkout v2.1.0
python -m venv venv && venv/Scripts/pip install -e .
# ingest ~150 real text records (≥800 chars each, shared vocabulary) through Main.ingest and time each call
# reopen the state directory and time Main(...) — it replays the journal
# profile the last ingests: settle_event_signal and connectivity_regions.build dominate
```

I can open a PR from `local-windows-scale` if any of the pieces are wanted upstream; each is separable (flat settlement, checkpoint, regions, compact index/archive, loopback transport, recall ordering).
