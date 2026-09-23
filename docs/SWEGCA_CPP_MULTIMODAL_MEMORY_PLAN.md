# SWEGCA C++ multimodal memory — design v3.3 (for cross-review, no code yet)

Status: draft for Claude–Codex cross-review. Nothing here is implemented.

## 0. Sources, in order
1. The user's current directive (2026-09-23 19:0x): 「그리고 기억은 단순 텍스트로 들어오는게 아니라 텍스트 이미지 사운드 영상 모두를 포함하는 것임. 이거 매우 중요. 원저장소에도 있을거임」.
2. The user's existing implementation (the original author is the user):
   - tinylm-slicer-sanabi-bazzite@3bddcb7:
     - mosaic_memory_activation.py:19-21 (OUTCOMES), :34-35 (`_cue`), :63-87 (MemoryStep), :89-106 (MemoryEpisode)
     - mosaic_episode_atoms.py:1-5 (atoms are not new episodes or evidence votes), :21-26 (canonical step bytes), :50-62 (step artifact address)
     - mosaic_media_atoms.py:20-71 (extents), :74-82 (MediaSelector), :102-108 (selector identifier), :123-186 (build)
     - mosaic_semantic_encoding.py:6-7 (Main retains the original separately), :22 (roles), :78-134 (Anchor)
   - SWEGCA-Architecture@5901a5a:
     - mosaic_external_memory.py:33-41 (ExternalMemoryResource)
     - mosaic_unrestricted_experience.py:23-60 (ExperienceArtifact)
3. Cross-review corrections that are already folded in:
   - Codex 19:10: modality belongs to parts, not to the whole memory; char_range counts code points; no hot-path model inference.
   - Codex 19:18: keep raw bytes only when they were received; keep storage_path as provenance; keep the original item_index; use a canonical step schema, not an opaque blob; selectors are metadata that depends on its memory.
   - Codex 19:34: Q4 closed as below.

## 1. What one memory is
One memory is one user episode, appended once under one digest-bound address:
- **episode identity:** the memory address (Main-sealed, from the C++ canonical content), plus episode_id (the producer's id, kept as a provenance field), revision, verification_state, source_addresses (unique, at least one).
- **steps:** one or more. Each step has:
  - phase
  - observation (canonical structured payload)
  - relations
  - judgment
  - outcome, one of success / failure / negative / uncertain / conflict / pending
  - evidence_refs (at least one, each non-empty)
- **resources:** zero or more observed resources (§2). Text, image, audio and video may all appear in one memory.
- **no modality field on the memory itself.**
- **no cues in the memory.** Cues live beside it in cue bindings (user decision 「가. 승인」). The user's MemoryEpisode.cues (:92) is the one deliberate difference, and it is recorded here.

A step and a selector are never a separate memory or a separate evidence vote (episode_atoms :1-5). Evidence counts the memory's address once.

## 2. Observed resource
`ObservedResource`:
- `resource_id`: identity text.
- `modality`: one of text, image, audio, video.
- `bytes`: optional `BlobInput`, present only when the original bytes were actually received. Main keeps them as the original (semantic_encoding :6-7). Large ones use the existing parts, so video fits. When absent, the record says so and names its provenance. Bytes are never made up, and a derived record never claims bytes can be recovered from it (wd14 :21-24, video_native :115 `raw_media_retained_as_experience = False`).
- `content_digest`: optional SHA-256 of the source bytes (the user's `content_sha256` / `storage_sha256`).
  - When bytes are present it is SHA-256(bytes), computed here.
  - When bytes are absent it is kept only if the source record actually names one. If the source names none, the absence is recorded. A digest is never estimated or made up. (Codex 19:25)
- `storage_locator`: optional text. This is the source's storage_path, kept as provenance only and never opened by the store.
- `item_index`: optional u64, the source's own value, kept as given (not rewritten to list order).
- `descriptor`, by modality, checked like Anchor (:111-134) and media_atoms:
  - text: UTF-8, code point count.
  - image: native width and height (> 0), and the coordinate frame (native or oriented source pixels).
  - audio: frames, sample_rate (> 0), duration_ns = frames·10^9 // sample_rate (:163-164).
  - video: native width and height (> 0), duration_ns.
- `metadata`: canonical structured payload.

The resource list is part of the payload, so it is part of the address identity. The order is the order the producer gave.

## 3. Canonical encoding
- The step list and the resource list are typed sections of the envelope (a new envelope version). Each is decoded and checked on read. None is an opaque blob.
- Step observation and resource metadata use the core's canonical structured payload (the same CanonicalPayload the state uses). Decode checks that they are canonical.
- The user's step bytes are JSON with sorted keys (episode_atoms :21-26). The C++ canonical form does not promise the same bytes (Q1).
- A step's derived address is `<memory address>/step:<index>:<digest of its C++ canonical bytes>`. It is a view, not a record. The name differs from the user's `memory-step-artifact:` (:59-60) on purpose, so the two are never mixed.
- Sections larger than a record go to parts, as blobs do now.

## 4. Selectors (parts of a memory)
- A selector names one place in one memory. Its fields: the step index, the resource index, kind, path inside the observation, role, and extents.
- The kinds are the user's (media_atoms :123-186): retained_observation, image, screen_frame, visual_feature_region, tag_proposal, caption_proposal, audio_feature_record, system_audio_feature_segment.
- The roles are: original, tag, caption, subtitle, summary.
- Extents:
  - `TextSpan`: code-point start and end (the unit the user uses), plus the byte start and end derived from them. Both are stored, and decode checks the bytes against the code points of the step or resource text.
  - `SpatialExtent`: native width and height, x, y, w, h, frame. The region must lie inside the source.
  - `TemporalExtent`: clock, start_ns ≤ stop_ns, precision (recorded_point, estimated_capture_interval or sample_enclosing_ns), optional hardware latency.
  - `SampleExtent`: 0 ≤ start_frame < stop_frame, sample_rate > 0. Its enclosing ns interval is as in :68-71.
- Address: `<memory address>/selector:<digest>`. The digest covers the memory address, step index, step digest, kind, path and extents (user :102-108). No bbox is invented where the source recorded none (:156).
- Selectors are built cold when the memory is appended (the equivalent of build). They are durable metadata that depends on the memory, like cue bindings: they carry no authority, they are no memory of their own, and they cast no vote.
- The record kind and its index entries still need a decision (**Q2**). They must fit the journal classification and the cue-binding kind 4.

## 5. Cues for media
- Tags, captions and transcripts produce text keys. They are bound, as normalized whole phrases, to the memory's address or to a selector's address (cue binding, commit a106ddc).
- The original tag text stays in its tag_proposal selector.
- Nothing tokenizes the media bytes themselves.

## 6. Familiarity without text
- The user's visual familiarity input (DINOv2 cosine, visual_deja_vu :164-226) is kept as an input that Déjà vu may receive.
- No model inference and no gallery scan go on the <1 ms Déjà vu→Recall path.
- Where the vectors live and what they cost is decided with the four-stage split on SWEGCA terms (**Q3**).

## 7. VRS
Edges carry no modality column (vrs_connectivity_regions :170-176). Strength stays per synapse group.

## 8. Open questions
- **Q1 (settled for now, Codex 19:25).** No promise that C++ canonical step bytes equal the user's sorted-key JSON bytes. C++ canonical addresses and the user's past `memory-step-artifact:` addresses are named differently and never mixed. Compare against the user's JSON input and encoding rules separately before any such promise.
- **Q2 (direction agreed).** Selectors are stored as durable metadata that depends on the memory (the user's immutable build → prepare). Kind, index entries and schema are fixed after a separate design review.
- **Q3 (deferred).** Where familiarity vectors are kept is decided only after the input contract of the <1 ms path is set.
- **Q4 (closed, Codex 19:34).** In the user's existing implementation the episode_id is itself a content address: `experience:` + SHA-256 of the Main-sealed item's canonical payload (mosaic_experience_organization.py@3bddcb7:186-195, "Derive persistent identity from main-sealed lineage, never from a model"). Repeats are grouped by alias rows that keep every original and its position (mosaic_experience_canonicalization.py@3bddcb7:1-5, :327-335). Supersession points at a record id (conversation_memory.py@3bddcb7:37-44, mosaic_memory_promotion.py@3bddcb7:328). So: our memory address, sealed by Main from the new C++ canonical content, takes the episode_id's place. The producer's episode_id stays as a provenance field. Lineage and supersedes name exact record addresses. A repeat alias never deletes the original and never adds an evidence vote. No byte equality with the user's Python JSON addresses is claimed.

## 9. Compatibility
The C++ store starts new memory. It is not a reader of the old store (user 2026-09-23: 「c++ 버전은 기존 호환이 아니라 어차피 경험 새로 만들건데」). The user's existing records are a specification source, not data this store must decode.

## 10. Envelope v4 layout (v3.3; Claude msg 245, Codex 20:50)
Agreed before code. The C++ envelope v3 (experience.cpp) gets one optional **typed section**. Nothing else in v3 changes.
- **Why one section.** The v3 worst case is 14,769,475 bytes with one more blob at its largest encoding (parted 2,097,198 bytes; addresses 75 bytes): fixed 89 + namespace 4,100 + lineage 1,024·79 + resources 1,024·4,100 + 5·2,097,198. That is 2,007,741 bytes under `max_payload_bytes` (16,777,216). Two more blobs do not fit.
- **Storage.** The section is one blob through the existing `BlobInput`/`BlobPlan` path: inline up to 2 MiB, parts beyond. It is streamed part by part. It is not a `CanonicalPayload`. That type is the state's owned opaque metadata (cognitive_state.hpp:41-49) and would lose the part streaming.
- **Optional episode.** The C++ memories so far follow the original ExperienceArtifact line and have no steps (transcript rows, file observations). Requiring steps would make up phase, judgment and outcome for them. So a memory may carry an episode or not. Both lines are the user's.
- **When the episode is present, the user's rules apply unchanged** (mosaic_memory_activation.py@3bddcb7:63-106):
  - `episode_id`, `revision` and `verification_state` are non-blank texts. The episode's own `revision` is kept **separately** from `Observation.source_revision` (Codex 20:50: the first is the episode's revision, the second the producer's).
  - `source_addresses`: at least one, unique, in the given order.
  - `steps`: at least one. Each step has:
    - `phase` and `judgment`: non-blank.
    - `outcome`: one of the six OUTCOMES (:19-21).
    - `relations`: texts, order and repeats kept, no check (as the source).
    - `evidence_refs`: at least one, each non-blank, order and repeats kept.
    - `observation`: bytes kept as given.
  - "Non-blank" uses the Python `str.strip` set of 29 code points (Codex 9be048a). Texts are stored as given, not stripped, as the source does (`_text`'s result is not assigned).
- **Resources:** zero or more, in the producer's order, with the fields of §2.
  - The `r` index is the sorted union of `Observation.resources` and the section's resource ids. Each list is stored as given, so where an id came from stays visible. The producer never has to keep the two in sync.
  - Resource bytes, when received, are a blob descriptor inside the section. The bytes themselves are existing content-addressed part records.
- **Decode and checks.** An inline section is checked fully at decode. A parted one is checked by `verify_parts` and while its visitor walks it; evidence admission and Re-evidence already run `verify_parts` first.
- **No new record kind, no new index letter.** Step and selector addresses are views.
- **Open:** the observation's JSON-compatibility check. The source round-trips it through JSON (:81-84). C++ has no native JSON validator yet. Until one exists, the bytes are kept unchecked and this gap is recorded.
