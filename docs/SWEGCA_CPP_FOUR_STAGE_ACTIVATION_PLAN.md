# SWEGCA C++ four-stage memory activation — design v1.1 (for cross-review, no code yet)

Status: draft for Claude–Codex cross-review. Nothing here is implemented.
It replaces the single-stage `ExperienceSelector::select` with Déjà vu → Recall → Replay → Re-evidence.

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

## 1. One snapshot for all four stages
All four stages run over one published journal universe U, and the VRS strength root is named by the same Main HEAD (b2c2f33). A stage handed a result from another snapshot fails, as :663-664 and :733-734 do. The query text is the same in every stage (:936-943). This is the user's one memory+VRS pair swapped atomically (:463-508): our Main HEAD names the memory watermark and the VRS strength root in one CAS.
- **Not true of today's API (Codex 19:51).** `for_each_index_match`, `resolve` and `replay_at_head` each take a fresh `snapshot()` (journal_store.cpp:1815-1826 and others). Several cue lookups and up to 5 replays could see different HEADs.
- **Needed first:** a Main-owned pinned read lease. It is taken once per activation, and every cue lookup, resolve, replay and strength read (the main root and the session VRS, §4) takes that lease. Codex designs and implements the journal side. The four-stage code is built on the lease, never on `snapshot()` per call.

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
- **Familiarity without text** (a DINOv2-like signal) is a later input to this stage. It stays deferred as multimodal Q3.

### 2.1 Cue normalization: Unicode casefold (user decision)
- The user's `_cue` (:34-35, with `_text` :27-31) strips, rejects empty, replaces each `\s+` run with one space, then casefolds. Python casefold is Unicode full case folding: CaseFolding.txt status C and F. For example ß→ss, final sigma→σ, and Greek, Cyrillic and other scripts fold.
- a106ddc folds ASCII only, and the user chose 「유니코드 casefold 그대로」 (19:5x, asked after Codex 19:45). So a106ddc's normalized_cue changes before integration.
- **Plan.** A static C++ table of (code point → 1..3 code points) for status C and F, generated from Unicode CaseFolding.txt 17.0.0 (/usr/share/unicode/ucd, Fedora unicode-ucd-17.0.0-2.fc44). The user's implementation pins no Unicode version (tinylm pyproject.toml:9, requires-python >=3.11). The host python3 folds by 16.0.0, so the two differ only on characters added in 17.0. The C++ store starts new memory. Generated by a C++ or awk tool, not Python (zero Python). The version is recorded beside the table. Lookup is binary search over a sorted constexpr array, with no allocation. Normalization order as `_cue`: strip, reject empty, collapse Python `\s` runs to one space, then casefold.
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
  - The user's RecallCandidate also carries revision, verification_state and historical outcomes (:620-627). Those need the record read, so they are filled at Replay, not on the hot path. This difference is deliberate and recorded here.
- **Ordering.** The user's recall sorts by cue_overlap = |matched| / |episode.cues ∪ current| (Jaccard, :689, :695). That needs every cue bound to each candidate, which means reading its bindings.
  - The user fixed the tie measure as the matched count (「맞은 cue 개수」), so Recall orders by matched count here.
  - cue_overlap stays available as a cold function over a replayed memory. It is not used for the tie rule.

## 4. Replay
- **Strength input:** a borrowed `StrengthView` from Main.
  - It reads the f32 strength of a memory's synapse group from the VRS strength root at the same HEAD.
  - **C, closed by the user (20:0x).** A memory appended after the main strength root was last updated has no strength there yet. The user's answer, verbatim: 「실시간 라이브용 세션 vrs를 실시간으로 돌려서 갱신을 기다리지 않고 반영한다」.
    - So a Main-owned **session VRS** runs live beside the durable main strength root and is updated as each memory is appended.
    - The StrengthView reads (main root + session VRS) under the same lease. Replay never waits for the durable update and never drops the memory.
    - The receipt records, for each strength, whether it came from the main root or from the session VRS.
  - The user's existing live path to read before designing it: mosaic_live_vrs_pipeline.py (Main-owned nonblocking incremental VRS generations, :1), mosaic_live_action_vrs_transaction.py (:1), mosaic_live_durable_vrs.py (:1), mosaic_vrs_event_hot_publication.py (:1-7).
  - **Session end, user (20:1x):** 「세션 종료되면 라이브로 만들어진 VRS를 병합이 아니라 하나의 블록으로 치면 되잖아」. At session end the live session VRS is not merged into the main root. It is kept as one VRS block. This matches the user's VRS definition (2026-09-23): VRS = blocks with connection points between them, strengthened and weakened, with a cap on block size.
  - User, continuing (20:1x): 「연결부만 만들면 저장소나 메모리에 무리도 안갈거고」. Closing a session block writes only the block and its connection points to the existing blocks. The main root is not rewritten and nothing is copied whole, so storage and memory stay bounded by the session's own size plus its connections.
  - Still to design with Codex before code: the block's storage and its connection-point record, how the StrengthView reads (main root + closed session blocks + the live session VRS), what happens when a session's block passes the size cap, and crash behaviour of a live session.
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
- **Trigger:** the replayed memory's phase differs from the current input's phase, or the current input conflicts with it (user). Otherwise no judge is called.
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
- CompositeMemoryActivationIndex and the hot-index layout wrappers (:233-351) correspond to the journal's published universe. They are not ported as types.

## 9. Open questions
- A: closed. 「구절 cue + 질의 토큰」.
- B: closed. The user answered 「맞은 cue 개수」.
- C: closed. The live session VRS, kept as one block with connections at session end.
- D: closed with Codex. Integrate after typed steps.
- E: closed. The user chose Option 1 (「Re-evidence 로 옮김」).
- F: casefold. Closed by the user: 「유니코드 casefold 그대로」. See §2.1.
