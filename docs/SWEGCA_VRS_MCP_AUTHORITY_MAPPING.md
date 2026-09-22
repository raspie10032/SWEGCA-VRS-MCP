# SWEGCA authority boundary for the VRS-MCP rebuild

This maps the public `SWEGCA-Architecture` revision
`5901a5aa2dcbd0ac7ad12ac6dd745699f72288a8` to the memory-facing
VRS-MCP at `c06092a`. It is a source audit, not a claim that the old MCP
enforces the whole architecture. The reviewed execution order is recorded in
`SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md`; this map makes no new read-order decision.

| SWEGCA rule | Authoritative source | VRS-MCP boundary and current evidence |
| --- | --- | --- |
| One persistent Cognitive State owner | `paper/swegca/ARCHITECTURE_SPEC.md` §§2, 4.1 | The VRS-MCP must serve main-owned experience/VRS generations and must not create a second Cognitive State or let an MCP client own belief/action authority. Existing `store.Main` owns one store generation, but session, shard, and main composition must be checked as one logical owner during replacement. |
| Observation, experience, evidence, belief are distinct | Architecture spec §§1, 3.1, 4.2; `TERMINOLOGY.md` “Epistemic objects” | An admitted user or tool record is an addressable observation/experience, not verified evidence or belief. `session_capture.py:316-435` sends host-visible records through session VRS with source and uncertainty metadata. A transcript cursor is not a recall source. |
| Experience access is status-unfiltered | Architecture spec §§3.1, 4.2 | Failed, pending, superseded and contradictory originals stay addressable. `store.Main.recall` currently keeps historical candidate records and chooses a current one for Replay (`store.py:1717-1762`); no selection rank may grant authority. Masked kinds and region-scope exclusion in `store.py:1481-1603` must be evaluated against complete-address access. |
| Evidence is claim-relative and provenance-bound | Architecture spec §§3.1, 4.4; `TERMINOLOGY.md` “evidence” | `vrs_evidence.py:217-278` builds explicit proposition hypotheses from resolved, non-superseded, addressable records. It excludes failed producer verification from evidence and records source/context/axis information. Its accumulated decision is not automatically a persistent Cognitive State write. |
| Insufficient or conflicting evidence abstains | Architecture spec §§4.4, 4.6, 4.9 | `engine/mosaic_memory_activation.py:463-559` represents current verdict, conflict, insufficiency and abstention. The rebuilt Re-evidence must compare opened originals against the current pinned VRS and preserve unresolved opposing evidence; a retrieval match alone is not support. |
| Producers are authority-limited | Architecture spec §§2, 4.3 | Hooks, MCP callers, workers and language specialists may contribute observations or receive zero-authority receipts. They cannot directly change persistent belief, semantic memory, action, training, distribution or P3 authority. `store.py:71` declares those authority flags false; a new receipt must make that structural, not merely a JSON convention. |
| Persistent mutation needs an accepted, bound decision and receipt | Architecture spec §§4.5–4.8 | The public architecture itself reports an evidence-address/decision-to-delta binding gap on its audited writer. The VRS-MCP must not copy a low-level commit shortcut or claim that experience admission or VRS strength is a World write. The memory MCP may expose evidence and provenance, but any later Cognitive State write requires its own guarded decision, target binding and receipt. |
| Rollback, retraction, recovery differ | Architecture spec §4.9; `TERMINOLOGY.md` “Decision and mutation operations” | Journal recovery of an interrupted VRS frame (`native_journal.py:137-178`) is storage recovery. A superseding record is not automatically a Cognitive State rollback or retraction. Do not use one event label as proof of another authority operation. |

## Promotion naming conflict to resolve in code

`engine/mosaic_memory_promotion.py:8-83` sets a field named
`semantic_evidence_allowed` to true solely when VRS connection strength is at
least `1.0`; its receipt denies action and persistent-write authority. The
public SWEGCA terminology says an experience becomes evidence only relative
to a claim and an admission policy, and verified semantic-memory promotion
requires a linked accepted decision and World-write receipt (architecture
spec §§3.1, 4.8). Therefore this field cannot be exposed or interpreted as
verified semantic-memory or World authority in the new product. The source
audit does not establish that the old product uses this field to perform such
a write; it identifies a naming/contract hazard that needs an explicit
boundary in the rebuild.

The C++ `VRSExperiencePromotionDecision` now preserves the source threshold,
four result actions and zero-authority flags as a detached decision value.
Its `semantic_evidence_allowed` field remains the source's connection-strength
status only; it is not a World-write receipt or claim-relative evidence
admission. The Re-evidence state-update planner and event signal binding are
still separate unfinished paths, so this value alone is not an active product
promotion route.

`src/swegca/mosaic_unrestricted_experience.py` in the public architecture
imports SQLite for one registered general-purpose artifact reader. The user
forbids SQLite in VRS-MCP, so copying that implementation would violate the
runtime requirement. The architecture's status-unfiltered access and
zero-authority receipt rules still apply to native VRS originals.

This mapping does not authorize adding a new internal LLM call or a second
memory system. No test, service, state migration or data mutation was run.
