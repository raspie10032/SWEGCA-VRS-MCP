# Bounded block storage

`BlockStore` persists post-verification VRS results. It does not classify raw inputs or replace SWEGCA verdicts. `OctahedralBlocks::Persist` connects each block collector to `BlockStore::append`. The synapse library includes this implementation.

- `max_originals`: original-experience count limit per logical block.
- `max_connections`: independent sparse connection-page entry limit.
- `segment_bytes`: physical file limit; full files roll into the next numbered segment without rewriting old files.
- Each block owns its mutex and writer. Different blocks can append concurrently; one block serializes its batches.
- A batch contains stable endpoint IDs, original record addresses, previous/current integer accept/reject/abstain counts, verdicts and delivery numbers. The existing SWEGCA count primitive checks each count transition. Storage never substitutes a new semantic verdict.
- Checksummed durable records are published in memory only after append succeeds. Reopening reconstructs counts and bindings. Complete corruption is rejected. A partial final record is preserved and subsequent writes use a new segment.
- Writer ownership is exclusive per store. Accounting includes the layout and segment files. Append uses a cached tail offset instead of scanning previous records.
- Atomicity is per block batch, not a transaction across blocks. Original payloads remain at their authenticated addresses; this store does not duplicate them.

## Verified boundary

`make build/block-store-tests && ./build/block-store-tests` checks concurrent block writers, segment limits/rollover, reopening, address binding, stale update rejection, batch atomic validation, torn-tail preservation, corruption rejection, exclusive ownership, and topology/core/collector persistence. A separate collector rendezvous assertion fails if its two block jobs are serialized.

The native whole-file input executable now uses this store through BlockIngress (see BLOCK_INGRESS.md). No full-corpus job was launched. Session closure/sealing and reconstruction of ingress placements, addresses, counts and portal indexes are now implemented by BlockIngress. Selective idle compaction and base-knowledge link compression remain separate work. No throughput claim follows from these functional tests.
