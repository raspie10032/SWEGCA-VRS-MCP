# VRS 2.2 repair invariants — 2026-09-21

This file freezes the user's requirements before the repair. These statements
are acceptance constraints. An implementation may not weaken them by changing
the meaning of a term, dropping an architecture layer, or excluding a slow
stage from the named boundary without an explicit user correction.

## Exact requirements

- "모든 경험은 swegca-vrs 시스템을 거쳐 경험으로 들어가야함."
- "불러오는것도 로그가 아니라 경험을 통해서 정확한 위치를 불러오는 것임."
- An active session writes to a small session-local temporary VRS.
- Active reads use the session VRS first and long-term main after a complete
  session miss. Main experience remains available.
- Main assimilation happens after the session ends. Silence during a session is
  not an end boundary.
- Original experience and temporary experience both remain usable, with exact
  addresses and provenance.
- Déjà vu -> Recall -> Replay -> Re-evidence remains the memory path.
- "너무 커지면 오래 걸리니까 분할하고 연결선 넣자."
- "vrs의 메모리 사용 제한은 4GB로 한다."
- "SSD속도는 5Gbps로 한정한다."
- "SSD의 최대 용량은 500GB로 한정한다."
- "vrs메인은 어떤 크기가 되어도 1ms 미만의 속도로 replay 단계까지 조회 되어야 한다."
- "10억 파라미터 경험도 초단위로 끝나야 함."
- The existing VRS architecture and main logic may not be deleted or bypassed
  to meet these limits.
- Existing current VRS experience is used for the repaired system because there
  is not enough time to accumulate a replacement experience set.
- User statements are not to be interpreted by guesswork. Unknowns stay
  unknown until an exact experience, source, code path, or user correction
  resolves them.

## Measurement boundaries that remain unresolved

The user has not redefined "parameter" as a record, edge, cue, token, or byte.
The repair must identify and report the VRS architecture's actual parameter
unit before claiming the one-billion target. It must not silently substitute a
different unit.

The `<1 ms through Replay` boundary includes every main-owned stage from lookup
entry through Replay completion. Any later source-page transport must be named
separately and cannot be excluded from that boundary by assumption.

The repaired read capsule keeps the exact derived cue vector checksummed beside
the original Replay body and decodes that vector when Re-evidence consumes it.
This follows the named four-stage boundary; no cue or original field is dropped.

The only trainable-style parameter count found in this repository is the dormant
recurrent cognitive-core default: 708,902,912 block parameters + 655,360 state
embeddings + 2,048 final-norm parameters = 709,560,320. No live code currently
maps VRS experiences, cues, edges or index slots to that count. The one-billion
target remains unresolved until that architectural link and unit are explicit.

## Source lineage required by the repair

The repair starts from `origin/vrs-regions` at
`ba4fea06722c78a37b219c2d33e57b3ff9e0ff5a`, which contains batch generations,
fine connectivity regions, connector/portal navigation, shared-experience keys,
and the multi-bundle resident. Session capture and layering are ported onto this
lineage. The simplified standalone candidate is evidence for the session-hook
defect and is not the architectural base.

## Implemented session ownership path

SessionStart or the first user prompt starts one locked transcript tailer. It
captures newly appended complete host-visible records into
`session-vrs/<host>/<session-hash>` within a one-second poll; lifecycle events
also scan the same cursor as durable delivery boundaries. The cursor contains
only byte/accounting positions and recent exact VRS addresses. It contains no
dialogue text and is never a recall source. VRS tool calls receive the exact host
session ID; reads complete against session VRS first and open main only after a
zero-candidate completed recall. `memory_status` establishes a bounded recall
lease before returning its session pair snapshot. Until the matching read is
released, transcript admission leaves the byte cursor in place so memory tool
records cannot invalidate that pinned snapshot. Release, read failure, lease
expiry, server close, and SessionEnd all unblock admission without excluding or
discarding a host-visible record. The resident also defers publication of idle
consolidation for the complete logical VRS while the lease is live, including
all hot shards that contribute to its pair snapshot.

`SessionEnd` returns within the host's three-second command-hook ceiling after
spawning a detached finalizer. The finalizer publishes end intent, acquires the
tailer's lifetime lock, captures a stable final transcript tail, publishes the
end marker, validates the session primary and all automatic
child VRS shards, and atomically attaches them to main. Main reads the original
experience stores through its exact directory and complete VRS projections.
There is no export/re-ingest merge, transcript outbox, proposal journal or
log-based recall.

Each writable shard persists exact envelopes and pair generations in the native
checksummed VRS frame journal (`vrs-store.json`, `journal/*.vrsj`,
`checkpoint.vrsc`). SQLite, a compatibility reader and a transcript database are
absent from the runtime package. A partial final frame is discarded at restart;
checksum damage fails closed. Checkpoints remain derived caches and the complete
native journal remains the source of truth.
