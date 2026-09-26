# VRS storage accounting foundation

StorageBudget is VRS resource bookkeeping, outside the SWEGCA evidence kernels.
It shares an atomic logical-byte allowance among independent block writers.
Its limit is configurable, including 500,000,000,000 bytes or a larger profile.
There is no fixed storage ceiling in the architecture.

ExperienceBlock accepts this owner on creation or writer reopen. Creating a
block reserves its header before creating the file. Appending reserves the
whole framed extent before the first write. Exhaustion raises StorageLimit,
not the block-full length_error used to rotate a physical block. Exhaustion
leaves the block writable and its existing content intact.

A failed exclusive open releases its unused header reservation. Once writing
starts the reservation is retained even if only part of the write succeeds:
partial originals remain on disk, and capacity is never accidentally reused.
This can conservatively overcount failed writes until cold reconciliation.
Closing or moving a block handle does not release persistent byte accounting.
Readers do not charge stored data again; their buffers use MemoryBudget.

The owner must outlive every writer and initialize existing_bytes from the
owned files before reopening. Reopen does not charge those bytes twice. The
current class does not scan directories, deduplicate hard links, or allow a
live counter reset. Multiple independent budgets do not collectively enforce
a shared device limit.

## Current boundary

This commit connects the budget to physical ExperienceBlock writes. The full
Runtime/SessionStore/Main/catalog chain has not yet been wired to one mandatory
owner. Omitted budget pointers retain the existing low-level unbudgeted behavior.
This is not yet a claimed 500GB system-wide limit. Remaining integration must
include metadata files, startup inventory with inode deduplication, crash tails,
and the separate SSD bandwidth policy. Filesystem allocation units, metadata,
and unrelated processes are not measured by these logical-byte counters.

Tests cover shared block limits, exact capacity, failed create rollback, no
file on denied create, no record on denied append, move/reopen accounting,
explicit larger limits, concurrent reservations, integer overflow boundaries,
and a real injected pwrite failure after a prefix reached disk.
