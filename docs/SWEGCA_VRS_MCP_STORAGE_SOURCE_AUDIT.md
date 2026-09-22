# Existing VRS storage and resource source audit

This is a read-only inventory of the public VRS-MCP source at `c06092a`.
It defines existing-experience preservation requirements for the full SWEGCA
rebuild. It is not a storage-format implementation, a migration instruction,
or a runtime acceptance result. The reviewed new flow is in
`SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md`.

## Canonical native experience

| Existing structure | Source evidence | What the rebuild must account for |
| --- | --- | --- |
| Store identity and active journal generation | `src/swegca_vrs2/native_journal.py:26-36,91-128` | `vrs-store.json` carries `swegca-vrs2-native-store-v1`, an identity, and a generation. Existing current experience is in these native stores, not in a transcript read cache. |
| Complete native journal | `native_journal.py:130-249` | Every append is one frame: `VRS2JNL1` file magic, little-endian 64-bit compressed-payload length, zlib-compressed canonical JSON with `swegca-vrs2-frame-v1` rows, and SHA-256 of that payload. Each row has five fields: sequence, request ID, envelope, fingerprint, pair ID. Sequence continuity and the pair head are checked. A partial final head frame can be truncated on writable recovery; checksum damage fails closed. |
| Segments and checkpoint | `native_journal.py:249-291`; `store.py:945-994,1099-1137` | The head rotates into immutable `segment-*.vrsj` files around 32 MiB. `checkpoint.vrsc` is a derived, identity-bound, SHA-256-checked snapshot. Its inner graph/index blob is zlib-compressed Python pickle protocol 4. Its absence or unreadability cannot mean the source experience is absent. Re-derivation can change pair certificates while retaining original envelopes, so addresses, source lineage, and revision must be checked independently. |
| Ended-session ownership | `linked_shards.py:21-145`; `session_capture.py:461-497` | `linked-shards.json` atomically registers complete ended-session native stores by path, pair ID, record count, and cue count. Attachment is in place; the session originals are not exported and re-ingested. Registry loading validates ownership, location under `session-vrs`, and duplicate IDs/paths. |
| Exact original and source route | `exact_replay.py:37-50,417-449,662-785` | Derived exact capsules contain a Replay header, original observation, and derived cue vector with CRC32 and original SHA-256 checks. The exact/source directory is a route to the original; losing or rebuilding it must not change the original's address, body, revision, outcome, or source. |
| Cue and current-VRS read projections | `cue_shards.py:25-37`; `read_projection.py:97-187,201-350` | Cue postings route to exact addresses. A pair-bound projection holds current strengths, numerical state, stability, pending status, usage, region, overlapping memberships, cue strengths, portals with shared original IDs, and evidence decisions. These are derived reads of a complete main generation, not a replacement source of truth. |

The current journal's `rewrite` operation in `native_journal.py:295-325`
publishes a new generation and removes the old directory. The only product
caller found is `store.Main.rebuild_from_journal` at `store.py:993-1075`, which
recomputes certificates and writes every journal row into that new generation.
The ordinary `Main.compact` at `store.py:1193-1207` checkpoints and rotates;
its code does not delete experience rows. A new implementation must not infer
from the word “compact” that old originals may be discarded.

`store.py:1389-1463` validates a whole `ingest_many` batch, appends its fresh
rows in one journal frame, and gives all rows in that batch the same pair ID.
It rejects request-ID reuse with changed content and treats an identical
retry as idempotent. Replaying existing experience must preserve these batch
and source-lineage facts, not treat each row as an unrelated observation.

The old linked-shard row above describes ownership in the previous product.
In the rebuild, the link transfers ownership only after SessionEnd. Main
numerical integration also replays those already admitted journal rows through
the author's SWEGCA `Main.ingest` and `Graph.append` path. This is native VRS
experience replay, not transcript re-ingestion. The session journals remain.

The checkpoint's Python pickle is a derived cache, not a requirement to keep
Python or a compatibility reader in the final C++ runtime. The complete
native journal provides the original envelopes needed to rebuild. This audit
does not yet prove that a C++ numerical re-derivation will reproduce the old
pair certificates; `SWEGCA_VRS_MCP_NUMERICAL_SOURCE_CONTRACT.md` records the
float32, RNG and ordering dependencies that must be checked.

## Shard and resource boundaries in the old source

`resident.py:26-32` sets 8,192 records per automatic shard,
`MAX_RSS_BYTES = 4 * 1024**3`, a 500,000,000,000-byte allocated-storage
ceiling, and a one-second storage-scan cache. `resident.py:570-642` keeps a
revision on the shard that owns its earlier episode or source and opens new
complete VRS shards for new sources. Record count is a physical splitting
boundary, not an authority or experience-quality rule.

`resident.py:345-410` counts allocated filesystem blocks and scans again near
the storage ceiling. It estimates RSS from `/proc/self/statm`, accepting an
unknown RSS value. These are pre-admission checks on the current process;
their source alone does not prove a hard post-batch storage ceiling, aggregate
VRS memory usage across processes, or enforcement on a system without `/proc`.
The C++ runtime must be checked against the user's resource limits with
authoritative measurements. The old 4 GiB constant must not silently redefine
the user's stated 4 GB requirement.

The stored address directory's maximum-depth structural result in
`docs/SIZING.md` is a routing-depth observation, not a billion-parameter VRS
measurement or proof that the full user-input-to-first-Recall path meets 1 ms
at arbitrary main size. No source, service, benchmark, or VRS state was
modified by this audit.
