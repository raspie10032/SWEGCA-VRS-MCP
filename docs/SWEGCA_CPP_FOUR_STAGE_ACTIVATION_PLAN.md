# SWEGCA C++ four-stage memory activation — design v1.10 (for cross-review, no code yet)

Status: historical draft for Claude–Codex cross-review. The four-stage route
is not implemented. Later user corrections are recorded in
`SWEGCA_CPP_VRS_LAYER_PLAN.md` §6.2 and the approved order document; those
decisions govern implementation where this draft differs. In particular,
Re-evidence is conditional on a difference between the current and replayed
three-state judgments, and the opposing-original Replay path proposed in §5
was withdrawn. The route replaces the single-stage
`ExperienceSelector::select` with Déjà vu → Recall → Replay → Re-evidence.

## 0. Sources, in order
1. The user's current directives (2026-09-23):
   - Four stages. Recall gives the original memory's address. Replay opens the original memory. Re-evidence runs only when the current input conflicts with the experience.
   - Re-evidence runs whenever the phase differs.
   - Replay opens the memory with the highest VRS strength. On a tie it opens up to 5. 「원저자 replay가 강도순위 안쓰는건 맞음. 현재 재구축하면서 넣는 개념임」, so this rule is tagged `user@2026-09-23`.
   - With 6 or more tied: 「Recall 일치도 → 최근 순」 (the Recall match first, then the most recent in journal order).
   - The match is measured as 「맞은 cue 개수」: the number of distinct current cues that matched (user answer, asked again 19:4x after Codex 19:39). It is not the user's existing cue_overlap ratio (:689).
   - Cues live beside the memory in cue bindings (「가. 승인」). Duplicate memory = 1 (same address). Experience = synapse strength, and the strength table is primary data.
2. The user's existing implementation, tinylm-slicer-sanabi-bazzite@3bddcb7:
   - mosaic_memory_activation.py:
     - :22 VERDICTS
     - :463-508 FullCurrentMemoryVrsSnapshot and its atomic owner (memory and VRS swapped as one pair)
     - :510-531 DejaVuSignal
     - :557-592 select_runtime_cues
     - :595-616 detect_deja_vu
     - :620-698 RecallCandidate and recall_memory
     - :701-746 replay_memory
     - :751-807 CurrentEvidenceVerdict and current_experience_verdict
     - :811-907 ReEvidenceResult and re_evidence_memory
     - :911-968 MemoryActivationReceipt and activate_memory
   - mosaic_semantic_family_directory.py:1-67 (family expansion in Recall)
3. SWEGCA-Architecture@5901a5a mosaic_unrestricted_experience.py:179-290, :440-532. This is the runtime selection that the current C++ `select` follows: every retrieved candidate is judged, and a receipt is kept.
4. The user-approved flow docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md (「순서맞음 ㄱㄱ」 2026-09-22): session layer :62-78 (see §4).

## 1. One pinned universe per route for all four stages
All four stages of one route run over one pinned published universe (the session U_s or the main U_m, see below) and that universe's VRS strengths (main: named by Main HEAD, b2c2f33). A stage handed a result from another snapshot fails, as :663-664 and :733-734 do. The query text is the same in every stage (:936-943). This is the user's one memory+VRS pair swapped atomically (:463-508): our Main HEAD names the memory watermark and the VRS strength root in one CAS.
- **Not true of today's API (Codex 19:51).** `for_each_index_match`, `resolve` and `replay_at_head` each take a fresh `snapshot()` (journal_store.cpp:1815-1826 and others). Several cue lookups and up to 5 replays could see different HEADs.
- **Needed first:** a Main-owned activation lease, taken once per activation. It pins **both** the session universe U_s (the session-local native journal and its session VRS) and the main universe U_m (the main journal and its VRS root), because the approved flow keeps them apart (:59-69). Déjà vu runs on U_s. Its `matched_cues` decides one route: U_s, or U_m on a miss (:13-15, :46). Every later stage (navigation, Recall, Replay, strength reads, Re-evidence) uses that one chosen universe. The same-U contract holds per route. Codex designs and implements the journal side. The four-stage code is built on the lease, never on `snapshot()` per call.
- **Publication boundary (v1.9, Claude msg 240 / Codex 20:37-20:38).** Three layers, kept apart.
  - **User rules (fixed).**
    - Approved flow :61-70: a session-local native journal, a session VRS generation published at admission, an atomic link of complete session journals to main ownership at SessionEnd, the session VRS kept as one block with only its connection points, and some blocks merged with one VRS run in periodic idle time. Original session journals stay available (:78).
    - User 2026-09-23 20:0x: the live session VRS reflects each memory in real time, without waiting for an update. An active session therefore has no normal lag between a published memory and its strength. The user's old pipeline lag (tinylm HEAD mosaic_live_vrs_pipeline.py:424-428) is what this directive replaces.
  - **User source principles (existing implementation, tinylm origin/main 3bddcb7).**
    - A lower record's digest is a storage identity, not authority. Main selects the committed digest. Files are never discovered or adopted automatically (mosaic_vrs_event_durable.py:3-5, :150-157).
    - A commit is ordered: pending marker fsync → pair CAS → rename to committed and directory fsync (mosaic_paper_resident_assimilation.py:502-530). A partial pending marker is no authority (:508-509). A leftover pending marker refuses further commits until explicit reconciliation (:447-449).
    - The user's code never finished restart selection of the committed digest (user ledger VRS2_EVENT_DURABLE_PUBLICATION_LEDGER_20260911.md:123-125).
  - **C++ candidate (agreed direction, not built).**
    - One Main-owned immutable publication root is the authority. It names (a) the current session journal generation and its session VRS generation, published together; a memory and its live strength appear in the same root or neither does. (b) The main journal generation, the VRS block list and the connection-point root. (c) The CognitiveState publication.
    - Lower files (journal segments, VRS blocks, connection points) may reach disk first. They carry no authority until a committed root names them.
    - The input hook pins the root once. Session Déjà vu runs on it, and a miss falls back to main in the same root. SessionEnd and idle merges become visible only through a successor root. Old roots stay readable while a lease holds them.
  - **Preconditions in today's code (Codex 20:37-20:38).**
    - `JournalStore::open` loads its own HEAD. There is no path to open the historical generation that a root names by manifest digest.
    - `load_published_head` removes segment, manifest and page-log bytes outside its own HEAD and truncates to the published end (codex/swegca-cpp-vrs journal_store.cpp). Under root authority this must not happen. Journal recovery and cleanup must first become subordinate to the root-selected generation and live leases, and bytes past the root are kept, not adopted.
    - Resuming writes after recovery (Claude msg 242 candidate, Codex 20:45; not buildable as is). `SegmentExtent` already carries ordinal, first_sequence, record_count and byte_length (journal_format.hpp:261-267), so a root can name each authoritative segment's committed end and where a new segment's sequence starts. But the next logical ordinal after a root-selected tail may already exist as a physical file of an unadopted later generation. Ordinal is the file name today, `ExtentIndex::apply_manifest` requires tail+1 contiguity, and `load_published_head` deletes files past the tail and truncates the last one. A new segment needs either logical ordinal separated from a physical file id/epoch, or unadopted files moved to preserved names. Codex 20:47 found the first is not a path change. Ordinal doubles as logical number and physical id across the manifest codec, `ExtentIndex` keys, `RecordPosition`, segment headers, file names, `stage_from` and replay/verify/read, so it touches format versions. The second needs its own treatment in `is_published_name`, disk accounting and recovery. `stage_from` also prefers appending to the existing tail, so after root recovery a mode that seals the tail is needed. Codex's current candidate keeps the format and starts with preserved names plus a sealed tail. Neither no deletion of original bytes, nor history references, nor storage accounting is proven for it yet, so it stays a design candidate.
    - Agreed direction: segment files are kept; only extents a root names are read; every orphan byte (segments, manifests, page logs) counts toward storage. No bound on orphans per crash is claimed: today several unadopted segments, manifests and page logs can exist.
    - **v1.10: separating the segment ordinal from the physical file (Codex 21:02, e0d29ff).**
      - Codex's decision, 21:02: the logical ordinal is kept apart from a physical file id (journal format v8). On restart the next physical id is max+1. After root recovery the tail is sealed, and appending continues under a new ordinal and a new physical id.
      - Claude's review (msg 257) found no new functional defect for the lower journal. Two open points came out of it:
        - The "never reused" claim does not hold across restarts. Cold open deletes orphans whose id is above the tail and keeps the maximum only in memory, so a deleted id can come back.
        - `load_published_head` deleted files above the tail before it had checked anything. A tail file renamed to a higher id was deleted, and published data was lost. This was already true before e0d29ff.
      - Codex 95e1656 adds a preflight for the store's own HEAD: it checks that every segment file the HEAD names exists, and checks its extent chain, before any orphan is deleted or any tail is truncated. This is not root-selected recovery and not orphan keeping (under review).
    - **v1.10: manifest log and page log (Codex 21:20).** A physical segment id alone does not make a publish safe after rolling back to a root.
      - `follows()` (journal_format.cpp) requires each new manifest log ordinal to be the previous one + 1.
      - `stage_from` appends to the current manifest log or opens the +1 log. `PageWriter` does the same for page logs.
      - Keeping unadopted manifest and page logs, and the tail suffix, past a chosen earlier root would therefore collide with those paths at the same file and offset.
      - Required before root recovery: manifest and page logs get physical names, an epoch, or a separate index that cannot collide with kept orphans. A new log is sealed. `follows` checks it, and storage counts it.
    - **Required of every recovery path:** nothing outside the chosen generation is deleted before that generation is fully checked (Codex 21:24).
  - **Open.**
    - Pending reconciliation. Keep the user's fail-closed rule and preserve the pending file. No automatic "abandoned" rule is fixed.
    - How experiences received or answered around a restart are related explicitly (correction relation).
    - Root file format, root history retention and its storage bound.
  - Codex 683fe27: `for_each_index_match_in`, `resolve_in` and `replay_in` take one pinned `PublishedSnapshot` through a private Main path. The upper lease that pins the memory and the strength root together (main root, closed session blocks, live session VRS) is not built yet, and it is a precondition here.

## 1.5 The user-approved read route (ORDER_FOR_REVIEW, 「순서맞음 ㄱㄱ」 2026-09-22)
Versions up to v1.5 left these approved rules out. They are binding, and the stages below follow them. Lines are docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md.
- **Session first, main on a miss** (:13-15, :46, :96-97). Déjà vu runs first on the pinned **session** VRS. Only when the session's matched cues are empty (`matched_cues == ()`) do Déjà vu, navigation and Recall run on the pinned **main** VRS with the same input. Main is not probed otherwise (:35-36).
- **Navigation between Déjà vu and Recall** (:16-19, :37-38, :98-100). Region preactivation from the matched cues, membership lookup, the coactivation witness query, portal planning and one local-navigation cue page follow Déjà vu. They feed navigation cues into Recall and precede its candidate selection. Only the single eligible portal and one cue page are used, and deferred regions stay explicit, so no complete transitive search is claimed (:47-50). These are SWEGCA elements (regions, portals, shared experience). The C++ rebuild must carry them. They are not optional.
- **Recall keeps the complete address set** (:20-23, :50-52). Every Recall address is kept in `memory_selection`. A transport page limit never removes an address.
- **Replay** (:24-25, :101). The approved flow opened "the first current original from author Recall order". The user's 2026-09-23 rule replaces that: highest VRS strength, up to 5 on a tie, then matched cues and recency (§4).
- **Re-evidence with the opposing side** (:26-29, :101-102). Re-evidence runs against the current generation. If there is relevant opposing evidence or a conflict, only the relevant opposing originals are also replayed, Re-evidence runs with both sides, and an unresolved conflict is preserved.
- **Receipt** (:30-32, :50-53). The Recall, Replay and Re-evidence rows bind one-to-one to the opened originals. The full Recall set is paged separately in `memory_selection`. There is no action or truth authority.
- **Timing** (:38-44). The <1 ms gate covers Déjà vu, navigation and the completion of Recall before Replay. It is measured from the host receiving the input. Main-fallback latency is reported separately and never hidden by session timing.
- **Input admission** (:103-105). The current input is a recall key at once. It becomes an admitted observation when the host-visible transcript record is captured, and it never holds up the pre-Replay boundary.

## 2. Déjà vu (anonymous)
- **Input:** the query text, plus the current cues as phrases, normalized by the user's `_cue` (:34-35, as in a106ddc).
  - **A, closed by the user (20:0x): 「구절 cue + 질의 토큰」.** The current cues are the caller's phrase cues followed by the query text's tokens under the cue rule. A repeat counts once, and the order is phrases first, then tokens in query order. The user's `detect_deja_vu` takes only the caller's cues (:603). Adding the query tokens is the user's current directive (`user@2026-09-23`).
- **Lookup:** each cue is looked up in the cue view, in both forms 'c' and 'h' where the length needs it. Binding hits fold onto the memory's own address (the first 75 bytes), as the selector does now.
- **Output: `DejaVuSignal`.**
  - current_cues
  - matched_cues: the cues with at least one posting
  - recognition_strength = |matched| / max(1, |cues|)
  - candidate_count: distinct memory addresses
  - It exposes no address and grants no authority (:521-528).
- **Cost:** one index lookup per cue. The distinct count is bounded by `max_retrieved`. Going over it fails closed, as now.
  - **No second lookup (Codex 19:55 (1)).** Déjà vu keeps its per-cue postings under the pinned U as an internal candidate cursor, and Recall continues from it. The public DejaVuSignal stays anonymous. Only navigation cues and family expansion add lookups in Recall.
- **Familiarity without text** (a DINOv2-like signal) is a later input to this stage. It stays deferred as multimodal Q3.

### 2.1 Cue normalization: Unicode casefold (user decision)
- The user's `_cue` (:34-35, with `_text` :27-31) strips, rejects empty, replaces each `\s+` run with one space, then casefolds. Python casefold is Unicode full case folding: CaseFolding.txt status C and F. For example ß→ss, final sigma→σ, and Greek, Cyrillic and other scripts fold.
- a106ddc folds ASCII only, and the user chose 「유니코드 casefold 그대로」 (19:5x, asked after Codex 19:45). So a106ddc's normalized_cue changes before integration.
- **Plan.** A static C++ table of (code point → 1..3 code points) for status C and F, generated from the official Unicode CaseFolding.txt 16.0.0 (downloaded with the user's approval, 2026-09-23, kept in third_party/unicode with its SHA-256). 16.0.0 is the host python3's unicodedata version, which runs the user's `_cue`; the user's pyproject pins no version (tinylm pyproject.toml:9). Pinned at 16.0 on Codex's advice (19:57); 17.0 adds mappings, e.g. U+A7CE and U+16EA0. Generated by a C++ or awk tool, not Python (zero Python). The version is recorded beside the table. Lookup is binary search over a sorted constexpr array, with no allocation. Normalization order as `_cue`: strip, reject empty, collapse Python `\s` runs to one space, then casefold.
- **Check.** Decoding a binding already checks `is_normalized_cue`, which has to use the same fold. The C++ store starts new memory, so no old binding has to be re-read.

## 3. Recall
- **Candidates:** the memories hit by matched_cues ∪ navigation_cues.
  - Navigation cues are Main-owned, added after Déjà vu, and recorded apart from the current observations (:655-659).
  - Original matched cues stay in, and the signal is never rewritten.
- **Family expansion** (:671-679, semantic_family_directory :50-67). This is not dropped. It maps onto the derived-experience links that already exist:
  - Children of a candidate: its lineage view 'l'<address>, the derived records that name it.
  - Parents of a derived candidate: its `derived_from`.
  - A parent brings in all of its children, as `family_spans` does.
  - Each added (member, parent) pair is recorded as a source dependency (:679, :697-698).
  - **Boundary.** Family members are recall candidates only. Each is its own memory, counted once by its address (duplicate memory = 1), and one reached twice is one candidate. A derived memory never adds an evidence vote for its parent, and a parent never adds one for its children (episode_atoms :1-5, "not new episodes or evidence votes"). The lineage stays as recorded: derived_from and the 'l' view are read, never rewritten.
- **Per candidate:**
  - address
  - exact RecordPosition. Its sequence is the recency key.
  - matched_cues: the distinct current and navigation cues that retrieved it, direct or through a binding.
  - **Family members get no copied cues (Codex 19:55 (2)).** matched_cues counts only the cues that retrieved that memory itself, directly or through its own binding. A member brought in by family expansion keeps its own direct count, 0 if none, and records separately that it came in through the family, with the parent address. The tie rule uses the direct count only, so a parent's cues never raise a member's rank.
  - The user's RecallCandidate also carries revision, verification_state and historical outcomes (:620-627). Those need the record read, so they are filled at Replay, not on the hot path. This difference is deliberate and recorded here.
- **Ordering.** The user's recall sorts by cue_overlap = |matched| / |episode.cues ∪ current| (Jaccard, :689, :695). That needs every cue bound to each candidate, which means reading its bindings.
  - The user fixed the tie measure as the matched count (「맞은 cue 개수」), so Recall orders by matched count here.
  - cue_overlap stays available as a cold function over a replayed memory. It is not used for the tie rule.

## 4. Replay
- **Strength input:** a borrowed `StrengthView` from Main.
  - It reads the f32 strength of a memory's synapse group from the VRS strength root at the same HEAD.
  - **Live session VRS: the user's words (fixed).** These are the user's current directive, verbatim (2026-09-23). Question C (rank of a memory without strength) was answered with this, and no rank rule for such a memory is chosen.
    - 20:0x 「실시간 라이브용 세션 vrs를 실시간으로 돌려서 갱신을 기다리지 않고 반영한다」
    - 20:1x 「세션 종료되면 라이브로 만들어진 VRS를 병합이 아니라 하나의 블록으로 치면 되잖아」
    - 20:1x 「연결부만 만들면 저장소나 메모리에 무리도 안갈거고」
    - 20:2x 「주기적으로 유휴시간이 생길때 일부 블럭을 병합해서 vrs 한번씩 돌려주면 되지 않겠나」
  - **Claude's reading (NOT fixed; to check against the user's code and Codex).**
    - A Main-owned session VRS updates strength as each memory is appended. The StrengthView reads it together with the durable strengths under the same lease, and the receipt records where each strength came from.
    - At session end the session VRS is kept as one block. Only its connection points to existing blocks are written. Nothing is rewritten or copied whole.
    - In periodic idle time some blocks are merged and VRS runs once over them. The merge replaces its inputs in one HEAD CAS.
    - "Main root = block list + connection points" is a guess about storage, not a decided structure.
  - **To settle before any structure (Codex 19:57).**
    - When a memory record and its live strength become visible together in Main HEAD, and whether a read can fall between them.
    - **C++ candidate (Codex 20:03; not a user rule):** the memory record and its live strength are published in the same generation. So no public generation holds a memory without a strength. A failed publication publishes neither and is closed explicitly. This continues the user's atomic memory+VRS pair (mosaic_memory_activation.py:463-507: readers never see a half-updated pair). The user's live path (mosaic_live_action_vrs_transaction.py:129-179) published memory first and queued VRS later. That lag is what the user's new directive removes.
    - Three different things, kept apart: the session **logical block** (new, user directive), the physical byte block of mosaic_vrs_block_store.py (a copy-on-write storage chunk, 4 MiB cap at mosaic_lossless_blocks.py:23), and SharedExperienceBridge (mosaic_vrs_connectivity_regions.py:122, one memory in several regions). The user's code has no counterpart of the session block, its connection points or idle merging. They are new implementation from the user's words.
    - **The user's approved flow (docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md, approved 「순서맞음 ㄱㄱ」 2026-09-22) already has the session layer.** Correction of v1.4, which said session-first reads had no source.
      - :62 publish a session VRS generation and derived cue, region and portal addresses.
      - :64 session-local admission and **session-first reads** until SessionEnd. Memory records also live in the session-local native journal during a session, so both memory and strength are read session first.
      - :68 at SessionEnd, atomically link the complete native session journals to main ownership. :78 the original session journals stay available.
      - :69 then, in the background, replay the VRS observations through main append and graph update (an immediate merge).
    - **What the user's 2026-09-23 words change:** only :69. At SessionEnd the session VRS becomes one block plus its connection points; replay into main (merging) happens only for some blocks in periodic idle time. And :62's session VRS generation runs without lag (20:0x). The user approved amending the approved document itself (「고쳐서 반영」); done in fcab35b, edited in place with an amendment section.
    - Keep the session-end block, and keep the existing lookup path: session first, main fallback.
    - Compare in full with the user's live path and the user's actual block and connection-point code first. The live-path files are mosaic_live_vrs_pipeline.py (:1), mosaic_live_action_vrs_transaction.py (:1), mosaic_live_durable_vrs.py (:1) and mosaic_vrs_event_hot_publication.py (:1-7). A survey is running.
    - Then design with Codex: block storage, the connection-point record, the size cap, idle selection and the idle signal, and crash behaviour of a live session.
- **Selection:**
  1. Take the highest strength among the recalled candidates. Equality is exact f32 equality.
  2. If one memory has it, open it.
  3. If 2 to 5 share it, open them all.
  4. If 6 or more share it, order them by matched count (descending), then by journal sequence (descending, most recent first), and open the first 5.
  5. The receipt records how many were tied and how many were left unopened.
- **Open:** `replay_at_head` on the same snapshot. The memory is decoded exactly, with its parts checked when read.
  - The replayed memory carries its steps (phase, observation, relations, judgment, outcome, evidence_refs) once the multimodal envelope exists (plan v3.2 §1).
  - Until then it carries raw/structured, as now.
- **Authority:** replay is reconstruction, not historical truth (:709-713). It grants no authority (:722-724).

## 5. Re-evidence
- **The first Replay and the opposing Replay are two sets, and both rules are kept (Codex 20:15).**
  - First set: the originals the user's 2026-09-23 strength rule picks, the highest strength with up to 5 on a tie (§4). The cap of 5 applies to this set only.
  - Opposing set: when Re-evidence finds relevant opposing evidence or a conflict, the relevant opposing originals are replayed as well (approved flow :27-29, :101-102). They are not counted against the cap of 5, and they are not chosen by strength. They are chosen by being relevant and opposing.
  - Neither set cuts the other, and no first-set original is dropped to make room.
  - Every opened original, from either set, gets its own one-to-one receipt row (:50-52). The row says which set it came from.
  - **Open.** How "relevant opposing" is identified waits for the typed step and proposition schema (the input contract below is a C++ candidate). Whether the opposing set needs a size bound is not decided, and no bound is invented. If one is needed, the user is asked.
- **Trigger:** the replayed memory's phase differs from the current input's phase, or the current input conflicts with it (user). Otherwise no judge is called.
  - **Input contract — C++ candidate, not a user rule (Codex 19:55 (3), 20:03). No external LLM and no string comparison stand in for it. The user's existing judgment is the EvidenceJudge over each replayed episode (:867, :879-881). The proposition identity and polarity below wait for the typed schema and are not fixed.** Main hands Re-evidence the current phase (a typed step phase) and zero or more current propositions, each with a polarity and its current evidence refs. The phase trigger compares typed phases for equality. The conflict trigger fires when a current proposition has the opposite polarity to the same proposition in a replayed step. "Same proposition" is an exact match of the canonical proposition identity that both sides carry. That identity comes with the typed step schema (multimodal plan). Until both exist, the conflict trigger is not wired, and nothing guesses it.
  - The memory's verdict is then the user's `current_experience_verdict`: **retained** if the current strength is ≥ 1.0, **available** if not (:781-807).
  - The verdict's evidence refs are the memory snapshot, the VRS snapshot and the memory's source addresses.
- **When triggered:** the judge gives one verdict per replayed memory. The verdict is one of support / refute / insufficient / conflict / available / retained (:22).
  - support/refute need current evidence refs.
  - conflict needs current or contradiction refs (:759-778).
  - The judge's episode identity must match (:880-881).
- **Result:**
  - conflicting_propositions = explicit conflicts ∪ (supported ∩ refuted).
  - insufficient = no conflict, no support, no refutation and no usable experience.
  - should_abstain = conflict or insufficient (:824-864, :883-907).
  - A missing fresh observation is never a refutation (:790, :895).
- **Dependency, D closed with Codex (19:51).** The phase comparison needs typed steps. "Always judge until steps exist" would change the user's phase condition, so it is not done. No interim operating path is needed before the whole rebuild, so the four-stage code is integrated after the typed step/phase envelope exists.

## 6. What happens to today's `select` judge (closed by the user)
Today's C++ `select` follows SWEGCA-Architecture: Rozephine's runtime cognition judges every retrieved candidate for relevance and contradiction. No candidate is dropped unjudged, and a receipt is kept (:179-290, :475-532). The user's four stages judge only what was replayed, at Re-evidence.
- **Option 1 — chosen by the user (2026-09-23 19:5x, 「Re-evidence 로 옮김」).** The judge moves to Re-evidence. Its CandidateVerdict (relevance, contradiction, rationale, rejection evidence) becomes the Re-evidence verdict input, alongside the user's verdict set.
  - The Architecture guarantee "no candidate is dropped silently" is kept in the receipt: every recalled candidate appears with its stage outcome (replayed, tied-unopened, lower strength).
  - Reason: the user's current directive fixes Replay by strength. Judging every candidate before Replay would put runtime cognition on each candidate on the hot path, and its selection would not decide what is replayed.
- **Option 2.** Keep the judge over every recalled candidate in Recall, as today. Replay then picks by strength among the ones the judge selected.
  - This keeps the Architecture method literally, but it adds a second selection the user did not name.
- Neither option deletes the receipt, its digest or its no-authority flags.
- Asked directly because the citation review judged that Option 1 changes the Architecture's "judge every retrieved candidate" method. The user chose Option 1 knowing that a lower-strength candidate is recorded, not judged.

## 7. Receipt
`MemoryActivationReceipt` holds:
- schema, stage order (deja_vu, recall, replay, re_evidence)
- the one universe and HEAD
- the signal, the recalled list with dependencies, the strength of each candidate as read, the tie record, the replayed set, the verdicts and the abstention
- a digest over all of it

It carries no authority (:927-946). This extends today's SelectionReceipt<NoAuthority>.

## 8. Other user logic kept
- `select_runtime_cues` (:557-592) picks one hot key: current-evidence cues first, otherwise the minimum non-empty fanout. Callers use it (dialogue_evidence :106-109, experience_organization :788). It becomes a Main helper over the same cue view, not a stage.
  - **Position (Codex 19:55 (4)).** It never runs before the user-input hook or before Déjà vu. It runs only inside Recall, as Main's choice among navigation cues after the anonymous signal exists.
- CompositeMemoryActivationIndex and the hot-index layout wrappers (:233-351) union their sources and refuse overlapping ids. They correspond to the journal's one published universe and are not ported as types. They give no read precedence between sources.

## 9. Open questions
- A: closed. 「구절 cue + 질의 토큰」.
- B: closed. The user answered 「맞은 cue 개수」.
- C: answered by the user with the live session VRS directive (§4, verbatim). No rank rule for a memory without strength is chosen. In an active session a memory and its live strength publish in the same root (§1 publication boundary), so none lacks a strength.
- D: closed with Codex. Integrate after typed steps.
- E: closed. The user chose Option 1 (「Re-evidence 로 옮김」).
- F: casefold. Closed by the user: 「유니코드 casefold 그대로」. See §2.1.
