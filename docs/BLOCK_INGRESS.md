# Native block ingress (2026-09-28)

`whole-file-ingress[-gpu]` now uses `BlockIngress`; it no longer invokes
`Runtime::retain_input`, ends/merges ten synthetic sessions, or persists input
before judging it. Existing corpus services remain stopped.

## Execution path

1. Bounded file queue -> installed codecs, original and decoded bytes in RAM.
2. Deduplicate the complete input/observation identity; partition original
   experiences by the configured count, using the SWEGCA partition boundary.
3. Ready batches go to free CPU/CUDA endpoints. Endpoints execute the existing
   evidence and association core functions; CPU and GPU share the scalar source.
4. A memory-only dispatcher collects results per block, flushing at the batch
   threshold, timeout or final drain. Independent block collectors own writes.
   The dispatcher never writes payloads or journals.
5. Each collector writes original/decoded/provenance records with one payload
   flush per batch, then publishes original addresses and ternary connection
   changes together in one checksummed durable block-journal record.

There are ten CPU endpoints and two CUDA endpoints by default in the GPU
executable. This is not one logical core per OS thread: an endpoint accepts a
batch of independent core operations. CUDA buffers are reused. Other blocks can
write while a block is flushing. Bounded queues provide backpressure.

## Run

Build: `make build/whole-file-ingress-gpu`.

The native executable accepts JSON lines containing `path`, `source`, `media`,
and `ticket`, and optional `relations`: an array of `{peer, support, refute}`.
`peer` is a 64-character experience identity; support/refute are already observed
relation evidence supplied WITH the input. Both present or neither present
preserves the existing core's abstention. The transport does not declare a
relation true and does not infer semantic observations from bytes.

The CLI's unspecified whole-input claim retains unknown evidence. Its `status`
is separate from `relation_counts` (accept/reject/abstain) in the receipt.
The native API also accepts an EvidenceTally from the observation producer.
A byte-level codec result is not reported as semantic image/text understanding.

`python3 tools/run_block_ingress.py ROOT INPUT...` feeds original paths
straight to the executable, with affinity 3-7,11-15. It performs no staging,
archive conversion or codec preprocessing. `--gpus 0` allows CPU-only execution;
`--block-originals N` controls the original-experience count limit. Recognized existing roots reopen with the same layout settings; unrecognized roots
are rejected, not overwritten or migrated. Receipts include `experience_id` for
addressing cross-block relations.

## Failure/recovery boundary

Within a collector batch, all original bindings and connection changes publish
in one journal record. A crash before that can leave unreferenced payload records,
which are retained. Atomicity is not claimed across blocks.

An explicit successful `finish()` (normal CLI EOF) drains all queued work and
seals the live blocks in parallel. The seal is a checksummed journal event using
SWEGCA's active-to-ended session transition. Repeated finish/seal is idempotent;
subsequent appends to sealed blocks are rejected. Destruction, processing failure
or a killed process does not manufacture a successful session-end event.

Reopening reconstructs experience placements, original addresses, connection
counts/delivery numbers and bidirectional portal indexes from block journals.
Closed and live blocks are both included by `original()` and `connected()`;
connection eligibility still comes from the ternary-count core predicate.
Duplicate input identities stay duplicates across restarts. New input uses an
unsealed block with free original slots, or a new block ID. Closed blocks are
never reopened for appending. Physical payload writing resumes in a NEW segment;
existing partial tails are preserved. No main-root merge or full copy occurs.

Recovery validates referenced payload framing/address metadata; payload content
checksums are verified by the selected `ExperienceBlock::read` call. It does not
scan every original payload body during startup. Missing committed payloads or
invalid frames fail recovery instead of silently dropping experiences. Record
addresses, rather than process-local record sequence counters, are stable IDs.

A process death during multi-block finish can leave some blocks sealed and some
live. Recovery preserves those per-block outcomes; sealing is not claimed to be
an atomic transaction across the entire session. No background compaction runs.

Codec errors remain attached to the experience. Unsupported data is preserved.
The in-process byte-vector budget does not include codec subprocess RSS/memfd
copies; this change does not establish a total RSS guarantee. Graph metadata and
per-block connection page limits still bound this implementation; it does not
claim unlimited-corpus performance or automatic cross-block semantic discovery.

Tests: `block-session-tests`, `block-ingress-tests`, `block-store-tests`, `work-pipeline-tests`. They
check actual codec input, restored original bytes, three-state relation counts,
exact-input deduplication, independent collector overlap, and CPU/CUDA parity.

`block-session-tests` checks successful/idempotent sealing, append rejection,
resume of an interrupted live block, unchanged closed-block bytes, preserved
partial tails, restart deduplication, live/closed cross-block portal restoration,
failed-finish non-sealing and rejection of missing committed payloads.
