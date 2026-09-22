# SWEGCA numerical source boundary for the rebuild

This document separates author SWEGCA rules from behavior introduced by the
old VRS 2.2 product. It is a source comparison, not a C++ implementation,
parity result, or performance claim. The earlier draft that treated VRS 2.2
constants as author requirements is preserved outside this worktree in the
agent bridge's rejected draft directory.

## Author rules to reproduce

| Rule | Author source | Required comparison |
| --- | --- | --- |
| Observation `Main.ingest` validates and appends to HotIndex, then Graph | VRS-MCP product `store.py` at `7536139`, `HotIndex.append` and `Main.ingest`; Claude rules (b) | Request ID, fingerprint, cue postings, revision and pair chain, including exact error behavior. |
| `Graph.append` creates record and cue nodes, record direct signal 0.1, signed bidirectional edges of base strength 0.5 | VRS-MCP product `store.py` at `7536139`; Claude rules (d) §1 | Node and edge order, float32 bits, result snapshot. A record's signal means existence, not truth. |
| Event settlement reads directed dependencies and keeps pending work when the round budget runs out | `mosaic_vrs_event_signal.py` and `mosaic_vrs_event_delta.py` at `tinylm` `3bddcb7`; Claude rules (d) §§4–5 | Exact pending set, edge binding, score update and float32 order. Pending is never relabeled convergence. |
| Connectivity regions use weighted modularity and overlapping membership coefficients from positive neighbor mass, normalized with `fsum`; shared original is an original with multiple memberships | `mosaic_vrs_connectivity_regions.py` at `tinylm` `3bddcb7`; Claude rules (d) §6 | Region labels, coefficient sum, generation binding and shared-original identity. No added membership floor. |
| Region preactivation, coactivation witnesses, decayed portal planning and one local navigation page precede Recall | `mosaic_vrs_portal_activation.py`, `mosaic_vrs_coactivation_navigation.py`, `mosaic_vrs_portal_lifecycle.py`, `mosaic_vrs_local_navigation.py` at `tinylm` `3bddcb7`; Claude rules (e) | One eligible portal and one page, deferred regions, witness lineage, source generation identity. No transitive BFS claim. |
| SWEGCA evidence accumulator groups source, context and evidence axes before any judgment | `SWEGCA-Architecture` `5901a5a` `mosaic_evidence_accumulator.py`, and `tinylm` `3bddcb7` variant; Claude rules (f) §§1–6 | Provenance, conflict, abstention, distinct source and context, no retrieval-rank authority. Variant choice requires exact source review. |
| `refine_vrs` shuffles edges, updates state in ordered batches, reinforces or decays strengths, clamps by base, and computes stability | `tinylm` `9ec73bd` `tools/organize_rozephine_mixed_experience_connections_hybrid.py:793–906`; Claude rules (f) §7 | Explicit PRNG and shuffle version, batch order, float32 state and strength, stability. Seed, cycles, passes and batch size are parameters, not fixed author constants. |

## Old product behavior that is not an author rule

The old `vrs_evidence.py` sufficiency weighted direct formula, `vrs_refine.py`
fine-region recursion and connector phase, thresholds `FINE_MIN_NODES=400`,
`MEMBER_FLOOR=0.05`, `SHARED_FLOOR=0.2`, portal key summary limit 8, default
seed 1729 and 16 cycles, and its edge candidate scoring come from the old
VRS 2.2 vehicle. They are not licensed by the author source as mandatory
SWEGCA behavior. Their old presence must not be used to omit author
coactivation, memberships or portal lifecycle.

The user still requires physical splitting, linked original experience,
parallel work on the 16-thread CPU, preservation of the entire SWEGCA logic,
and a 4 GB memory limit. The implementation must meet those requirements
using author rules or obtain an explicit decision for any new rule. A split
storage representation cannot change source addresses or bypass `Graph.append`
when session VRS experience enters a main generation.

Existing VRS 2.2 pair certificates and numerical arrays are historical
evidence. A new source implementation must account for their lineage and
explain numerical differences; copied originals alone do not prove numeric
parity. No new build, test, benchmark or resident service has been run for
this source comparison.
