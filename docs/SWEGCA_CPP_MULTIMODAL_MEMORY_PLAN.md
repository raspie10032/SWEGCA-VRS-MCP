# SWEGCA C++ multimodal memory — design v3 (for cross-review, no code yet)

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

## 1. What one memory is
One memory is one user episode, appended once under one digest-bound address:
- **episode identity:** episode_id (the producer's id, kept as a field), revision, verification_state, source_addresses (unique, at least one).
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
- `content_digest`: SHA-256 of the source bytes. Always required: the user's records name `content_sha256` / `storage_sha256` even when the bytes are not kept.
- `bytes`: optional `BlobInput`, present only when the original bytes were actually received. Main keeps them as the original (semantic_encoding :6-7). Large ones use the existing parts, so video fits. When present, SHA-256(bytes) must equal content_digest. When absent, the record says so and names its provenance. Bytes are never made up, and a derived record never claims bytes can be recovered from it (wd14 :21-24, video_native :115 `raw_media_retained_as_experience = False`).
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
- The user's step bytes are JSON with sorted keys (episode_atoms :21-26). Whether the C++ canonical form must reproduce those bytes exactly is **open question Q1**. It matters if the user's step artifact addresses have to match.
- A step's derived address is `memory-step-artifact:<index>:sha256:<digest of its canonical bytes>` (:59-60). It is a view, not a record.
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
- **Q1.** Must the C++ canonical step bytes equal the user's sorted-key JSON bytes, so that step artifact addresses match the user's records?
- **Q2.** Record kind and index entries for selectors. Are they stored at append time (a durable generation, as the user's immutable build/prepare) or recomputed cold at Replay? Draft: stored.
- **Q3.** Where familiarity vectors are kept and what the non-text Déjà vu input looks like.
- **Q4.** The episode_id text and our digest address are both kept. Which one do lineage and supersession name? Draft: our address. episode_id is kept as the user's identity field.
