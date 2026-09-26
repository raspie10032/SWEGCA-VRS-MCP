# Compact natural-input Recall receipts

`InputRecall` owns pinned original addresses and remembered connection contexts.
Consecutive candidates from the same owner, connection identity, recorded head
and current-observation boundary share one full `RecallMatch` snapshot. Every
candidate still owns its original location, original index and context index.
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

The receipt still stores one pinned address per candidate. Cue enumeration and
MCP response serialization still traverse all candidates. This change does not
claim constant-space Recall, paginated transport, or giant-graph latency proof.

Tests cover iteration and indexed equality, range rejection, move ownership,
separate sources with the same identity and different strengths, exact selected
Replay, and receive-after-Recall/Re-evidence boundary preservation. Persistent
Main tests exercise old receipts across subsequent Main publication as before.

Observed on the native build: a 16-candidate one-context receipt uses 1712
tracked bytes, versus 4224 bytes for 16 expanded InputMatch values (excluding
container object storage in both figures). Session runtime: 429 checks; runtime
lifecycle: 101; persistent Main: 174; native stdio subprocess protocol: 363.
