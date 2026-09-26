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

## Runtime integration

Runtime owns one mandatory shared StorageBudget. Session controls, original
blocks, connection/catalog records, catalog pointer staging files, end markers,
Main controls and Main merge journals receive that same owner. Publishing hard
links does not reserve the payload again. The stdio configuration requires
`storageBytes`; the example uses 500,000,000,000 decimal bytes and callers may
configure a larger value. The C++ RuntimeConfig default is also 500GB.

Before opening writers, Runtime holds an exclusive advisory lock on the root
directory, inventories regular-file logical sizes, and deduplicates device/inode
pairs. It includes partial and unrelated regular files under this dedicated
root, rejects symlinks/special files, detects sum overflow, and charges the
inventory's inode set to MemoryBudget. The directory walk is a cold startup
operation and does not run in input/Recall. Cooperating Runtime owners cannot
race inventory against each other's writes. External processes and standalone
low-level stores must not mutate the owned root.

Reopen creates a fresh budget from actual retained extents. A smaller limit than
the existing inventory refuses startup before any block writes. Opening at the
exact existing size permits reads/recovery but denies new storage growth.
Low-level standalone store APIs can still omit a budget for isolated use; the
Runtime path always supplies one.

## Remaining physical resource boundary

This enforces logical byte reservations for the Runtime-owned tree. Filesystem
allocation units/metadata, unrelated processes, SSD bandwidth and total process
RSS are separate requirements. Failed-write reservations and publication-sync failures remain conservatively
charged until reopen: live usage may overestimate retained bytes. Successfully
replaced catalog pointers are reclaimed only after rename and directory sync,
when fstat on the held old descriptor reports zero links and that descriptor
has been closed. An old pointer retained by a hard link stays charged. Failed
rename, failed sync or failed inode inspection never returns those bytes.
Reopen reconciles any conservative failed-operation reservations. No claim of physical-device 500GB or 5Gbps compliance
is made by these logical counters alone.

Tests cover shared block limits, exact capacity, failed create rollback, no
file on denied create, no record on denied append, move/reopen accounting,
explicit larger limits, concurrent reservations, integer overflow boundaries,
and a real injected pwrite failure after a prefix reached disk.

Catalog quota regressions cover repeated successful replacement with exact
inventory equality, old-pointer hard links, rename failure with both files
retained, and directory-sync failure after rename (old bytes remain charged).
