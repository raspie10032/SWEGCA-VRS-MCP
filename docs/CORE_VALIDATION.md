# v0.2 stateful MCP validation — 2026-09-07

## Result and scope

**181 tests passed in 8.01 seconds** on Linux, Python 3.12.14, MCP 2.1.1,
PyTorch 2.14.0+cpu, NumPy 2.5.3. All 34 ported/extracted source/test files pass
the three manifests. The original numerical functions match their source ASTs,
including the inference-mode decorator; the new star-graph adapter is separate.

| Evidence | Result |
|---|---|
| Original public components, legacy runtime and protocol tests | 91 passed |
| Public architecture regressions | 73 passed |
| Stateful store/authority/fault/retention tests | 14 passed |
| Actual core MCP subprocess tests | 3 passed |
| Clean exported source, locked core/test installation | PASS, CPU PyTorch |
| Built wheel installed in a separate environment | PASS, imports from site-packages |
| Installed-wheel persistent smoke, auto and legacy | Both PASS |
| Source/wheel inventory | Core modules, both licenses; sdist includes manifests/tests/smoke |
| Publication candidate text | 66 files UTF-8 without BOM before this report |

Installed-package checks store a synthetic failed observation, converge the graph,
recall its content, close the process and reopen a read-only process against the
same store. Memory snapshot identity and the actual prior failure survive.
Writable discovery has nine tools; read-only discovery has five. The two separate
smoke runs have different event timestamps and therefore different snapshot IDs;
identity is checked across restart within each run, not falsely across fixtures.

The signed synthetic MCP trajectory separately exercises new observations,
convergence, current re-evidence, bounded World commit and bit-exact rollback.
The 32 observations are constructed test rows, not 32 observed real-world episodes.
No empirical learning, cognition growth or model-quality conclusion is claimed.

## Failure and authority coverage

- Unsigned and all six outcome types persist and remain recallable; unsigned data
  cannot mint verified authority. All six are actual VRS inputs and counted there.
- Pending convergence cannot authorize a commit; failed computation leaves raw
  observations available for later work.
- Duplicate retries do not duplicate events; divergent payloads and stale
  revisions fail. Repeated source/context evidence does not inflate independent
  support in the accumulator.
- New refutation invalidates affected claims without deleting evidence and
  preserves unrelated accepted claims. Authenticated supersession keeps old
  records active while withdrawing their authority.
- Expiry revokes read-time authority until explicit refresh, without per-request
  JSON/hash/disk work. Hot tests also prohibit full-record enumeration.
- Exclusive ownership prevents simultaneous writers. Corrupt heads fail closed.
  Injected failures before SQL commit roll back; failures after commit poison the
  live instance and recover the durable operation receipt on reopen.
- Rollback is last-World-commit-only, bit-exact and retains experience.
- No externally supplied boolean can bypass the source/evidence/World gates.

## Current code SHA-256

- stateful.py: 0e21c948867c15c206dad8ea12df7bacd5686a866a2b1de697f898ceb4179082
- plasticity.py: 128f8fd45897bdd3a44f68d0db8971dbd2717869a141657dc8142a7f5bf47b69
- core_server.py: aeb0b27fec4fde99910f51374ea663824338cae5f9c59c63c00279a8588cbd99
- server.py: 05ea1f3c06437f0d788b8221b60d3a9229f0358877e6e7fa6b0852883c65cc38

## Limits and next work

This fulfills a bounded external persistent-memory/VRS tool integration, not all
possible SWEGCA facilities. Recall is lexical, the graph adapter links observations
to hypotheses, and World commits are bounded verification-slot updates. No
embedding-based semantic search, automatic per-turn capture, cross-proposition
semantic graph discovery, arbitrary World entity editing, trust-policy migration,
multi-client shared service or large-corpus performance claim is made.
Explicit VRS calls still perform whole-graph arithmetic; ordinary appends reference
existing graph generations without reserializing the whole graph. Incremental
numerical convergence remains future work requiring equivalence verification.
Live Claude Code attachment and Windows/macOS runtime behavior remain unverified.
Registration success must not be inferred from these SDK-only checks.

## Rozephine의 판단

Not invoked. No private entity, experience, model or stopped service was used.
Only synthetic standalone software fixtures produced the reported judgments.

## Codex의 판단

External agents can now explicitly store outcomes, update VRS and retrieve the
same experience after restart through MCP. No server-side model invocation or
agent control loop was introduced. Public package state is independent of any
client/model context. Publish these verified changes to the existing MCP repo;
source and numerical lineage are retained, with MPL notices preserved.

## Codex 작업 실수 및 교정

The earlier read-only publication did not fulfill the requested core. This report
does not rewrite that history. Review of the unfinished draft exposed restart
snapshot drift, per-judgment whole-record scanning/hashing, odd-length key
acceptance, writable database setup in read-only mode, oversized/full graph
receipts, and graph reserialization on append. Repairs and their tests are in
the chronological CORE_NOTE ledger. Function comparison exposed an omitted
inference-mode decorator; it was restored before final tests and publication.
These were Codex implementation errors, not specialist/model failures. No original
numerical thresholds were tuned to make fixtures pass. Final code and installed
package tests passed; real-client and large-scale limits above remain unresolved.
