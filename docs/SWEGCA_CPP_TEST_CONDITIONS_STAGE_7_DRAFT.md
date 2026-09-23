# SWEGCA C++ Stage 7 test conditions: memory and transaction

Status: static-review draft for the architecture inventory §10 step 9. These
are conditions to review before writing tests. The architecture is incomplete;
no build, syntax compilation, product test or benchmark is authorized by this
document. Conditions marked `route pending` describe required outcomes, not
claims that an implementation already provides them.

Sources: `SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md` §2.9-11,
§3F-G, §5-7, §9-10; `ARCHITECTURE_SPEC.md@5901a5a` §4.8-4.9 (I07, I10);
`mosaic_memory_promotion.py@3bddcb7:37-71,165-344`,
`mosaic_world_memory_transaction.py@3bddcb7:49-183,212-360`, and
`mosaic_versioned_memory.py@3bddcb7:143-204`.

| ID | Setup | Required result | Rule |
|---|---|---|---|
| M1 | Construct a candidate with each required scalar blank under Python `str.strip`, no provenance reference, or a blank alias; then use valid fields with duplicate aliases | Invalid input is refused; valid aliases keep first-seen order and refs keep order and repeats | Candidate source :37-71 |
| M2 | Candidate contains the exact original-experience addresses and write-receipt link, then omit each one in turn; add unrelated refs | Exact required set is included; each omission fails; extras do not substitute for either typed kind | Linked transaction :234-239 |
| M3 | Pass the pure promotion rule an incomplete-provenance, rejected, accepted-and-counterfactual-verified, semantic-regime-change, and remaining case | Respectively quarantine, retract, promote or refresh semantic, quarantine, and record episode; semantic read is allowed only for the accepted verified case | Promotion source :193-246 |
| M4 | Give the rule a tier or judgment status/reason pair that the native accumulator cannot issue | No decision or authority is issued, and the output cannot be used as a stale decision | Inventory Stage 7; I05, I10 |
| M5 (`route pending`) | Present an accepted visible decision that is forged, stale, for another claim revision, or not bound to the candidate's admitted original experiences | No promotion capability or journal mutation | I03, I05, I10; inventory §3C, §3F |
| M6 (`route pending`) | Present a genuine accepted decision and all three causal/source/context minima, then violate each minimum alone | Only the all-passing case is eligible; each failure makes no semantic mutation | Linked transaction :226-233 |
| M7 (`route pending`) | Match a receipt's visible ID but use another Main publication, wrong state hash, wrong evidence set, or an unselected journal root | The route refuses the candidate before preparing a transaction | I07; linked transaction :224-239; inventory §3F-G |
| M8 (`route pending`) | Replay an original experience, a derived experience and a write receipt through their typed references | Every original remains exactly addressable; derived material and a receipt link never impersonate original evidence or grant authority | Inventory §2.3-4, §2.10-11 |
| M9 (`route pending`) | Promote a verified revision that supersedes an earlier one, then query historical validity | New and old versions remain addressable with their validity intervals and revision links; semantic reads choose only an eligible current version | Inventory §3F; versioned source :143-204 |
| M10 (`route pending`) | Commit a memory mutation that differs from the prepared transaction's doc ID or update ID | Refuse completion; retain a recoverable, inspectable stage and do not publish a mismatched Main head | Linked transaction :241-272; I07 |
| M11 | Attempt each legal stage edge and every skipped, reversed or unknown edge, including recovery from each incomplete stage | Legal edges advance by exact expected-stage comparison; unsupported edges fail without changing the output stage; recovery never starts from completed or rolled back | Linked transaction :75-130, :142-167, :265-272, :295-312 |
| M12 (`route pending`) | Interrupt before and after each native journal, manifest, marker rename and directory durability boundary during preparation, memory publication, Main publication, completion and compensation | Restart selects one committed Main root; it replays or compensates an incomplete transaction without treating detached bytes as committed | I01, I07; inventory §9-10 |
| M13 (`route pending`) | Roll back when the current after-state hash matches, then after an unrelated write changes it; retract a linked current head while preserving unrelated later cognition | Exact rollback succeeds only at the matching state; stale rollback fails; allowed retraction preserves unrelated successors and historical records | Linked transaction :275-360; I07 |
| M14 (`route pending`) | Forge or reuse a visible receipt, promotion decision, or spent semantic-promotion capability | No mutation; only Main's one-use semantic-promotion authority can cross the writer boundary | I10; inventory §4, §6 failure 8 |

Open route decisions remain in the architecture inventory, including Main's
publication boundary, selected receipt and journal root, rollback retention,
and native version format. These conditions must be revised against the final
route before step 10; they do not justify adding compatibility storage or
copying the old SQLite transaction design.
