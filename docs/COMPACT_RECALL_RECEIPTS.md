# Compact natural-input Recall receipts

`InputRecall` owns remembered connection contexts. Exact-cue results pin original
items through shared segment ownership; continuation results pin whole sequences
of sealed experience segments.
Consecutive candidates from the same owner, connection identity, recorded head
and current-observation boundary share one full `RecallMatch` snapshot. Every
exact-cue candidate retains its sealed experience, original index and context index.
Candidate order and count are unchanged. Distinct source sessions never share
a context merely because their connection identities match.

`matches()` now returns an indexed, iterable view instead of a contiguous span.
Indexing and iteration assemble an `InputMatch` by value in O(1), using only
receipt-owned data. They perform no storage reads and never consult a mutable
connection or a replaced Main entry. The receipt must outlive its views; obtain
a new view after moving the receipt. Original selection and core lineage checks
in Replay are unchanged. Receive can append its new event after Recall without
changing the receipt's prior head, address or Re-evidence boundary.

All context and address allocations use the VRS MemoryBudget. A failed address
append removes its newly inserted context; a failed overall Recall destroys the
private result. No partially built result is published. This is storage layout
compaction, not evidence selection or a new judgment rule.

Exact-cue receipts still store one sealed-item reference per candidate. Exact-cue
enumeration still traverses all candidates. MCP now serializes bounded
pages (see MCP_STDIO.md), while the receipt itself is not constant-space.
Giant-graph latency remains unproven.

Tests cover iteration and indexed equality, range rejection, move ownership,
separate sources with the same identity and different strengths, exact selected
Replay, and receive-after-Recall/Re-evidence boundary preservation. Persistent
Main tests exercise old receipts across subsequent Main publication as before.

Measured before adding segment snapshots in continuation contexts: a 16-candidate one-context receipt uses 1712
tracked bytes, versus 4224 bytes for 16 expanded InputMatch values (excluding
container object storage in both figures). Session runtime: 429 checks; runtime
lifecycle: 101; persistent Main: 174; native stdio subprocess protocol: 363.


## Continuation Recall without expanding original addresses

A continuation selects the existing connection history. It now retains one
read-only `ExperienceSequence::Snapshot` and remembered head per candidate
connection, plus that range's exclusive end. It does not copy an original
address for every observation. Indexed access binary-searches the connection
range and addresses its sealed segment in O(1). MCP pages consequently assemble
only the requested matches; no live connection state is consulted for them.

Snapshots share ownership of segments, including a partially filled tail. The
writer can append new slots but cannot modify previously sealed values. The
snapshot count excludes those later slots. This differs from two independent
writers, for which Main's existing prefix sharing still copies the partial tail.
A snapshot's directory uses the requesting VRS MemoryBudget; sealed storage
remains charged to its original budget. Both budgets must outlive the snapshot.
All directories and shared ownership use native C++/PMR with no external package.

Destroying/replacing the source sequence cannot invalidate pinned values. A
snapshot can retain old segment memory until its receipt is released. Replay
still checks current source lineage, and stale Main receipts still cannot
Replay after a new Main publication. Retaining metadata does not confer current
Main authority. Allocation failure leaves the source and published graph intact.

Continuation receipt metadata scales with segment count plus connection count,
not one full address per observation. It is not constant-space, and exact-cue
candidate references still scale with match count. No core verdict,
shuffle traversal, source admission, persistent format or memory-stage ordering
changes in this implementation.


Native regression measurements after this change: 16-candidate continuation
receipt uses 264 tracked bytes, versus 4224 bytes for expanded InputMatch values.
The exact-cue receipt uses 1768 bytes (previously 1712), because its context now
also accommodates an optional snapshot. Shared sealed data was already stored;
these figures measure new receipt allocations, not retained-data/RSS totals.
Connection checks: 14972; session runtime: 497; persistent Main: 196; MCP stdio
subprocess: 918. Failure injection covers snapshot-directory and context setup;
old receipts remain readable as metadata after Main entry replacement while
stale Replay is rejected.

ASan+UBSan instrumentation could not link on this host: the toolchain points to
missing `/usr/lib64/libasan.so.8.0.0`. No ASan execution or address-sanitizer
coverage is claimed; no system package change was made.
The UBSan trap build of persistent Main passed all 196 checks.


## Exact-cue Recall pins selected items

Exact-cue candidates now hold aliasing `shared_ptr<const ExperienceEvidence>`
references to the selected sealed items. The pointer uses the existing segment
ownership block; `pin` allocates no memory and copies neither an original
location nor the connection's segment directory. Index checks precede pointer
creation. Receipt access assembles the original location from the pinned sealed
item, without touching the replaced owner or reading original bytes from disk.

A sparse match pins only its containing segment (capacity at most 256), not the
whole connection. Already sealed slots are immutable while later tail slots may
be appended. The segment's original MemoryBudget must outlive its last pin.
Holding a receipt across Main replacement can retain a segment until that receipt
is released. This is an explicit retained-memory tradeoff; it does not guarantee
lower total RSS for every sparse workload. Candidate ordering, authority checks,
source lineage, shuffle results and persistent formats remain unchanged.

Tests pin the tail of a 1030-value owner with allocation disabled, append another
value, destroy the owner and read the same sealed item. Accounting shows only
one tail segment remains, then returns exactly to baseline after the final pin
is dropped. Additional exact-receipt allocation failure checks preserve memory
accounting, and old exact receipt addresses survive Main entry replacement while
stale Replay remains rejected.

Native measurement: the 16-candidate exact-cue receipt now allocates 744 tracked
bytes, down from 1768 before selected-item pinning (4224 for fully expanded
InputMatch values). Continuation remains 264 bytes in that fixture. This counts
new receipt allocations, excluding already stored or subsequently retained
segments. Connection: 14981 checks; session runtime: 497; persistent Main: 219
in both native and UBSan trap builds; native MCP subprocess: 918 checks.
