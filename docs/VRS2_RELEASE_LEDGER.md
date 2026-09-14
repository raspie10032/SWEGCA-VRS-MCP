# VRS 2.0 native memory MCP release ledger — 2026-09-14

## Scope and preflight

The user explicitly requested updating the existing public SWEGCA-VRS-MCP
repository with the VRS2 MCP version for testing in Claude. Release the standalone
transport and memory-use interface, with an explicit existing native-main backend
requirement. Retain the v0.3 stateful and legacy diagnostic modes and their data.
Do not represent the stateful star-graph adapter as the native VRS2 engine.

P1. Native main alone owns identity, CognitiveState, accumulated experience, hot
memory, VRS, arbitration and authority. The new public MCP owns transport handles
only. The independent stateful mode retains its separate owner and store; there
is no automatic merging of conversation memory into native cognition.
P2. No internal or final-utterance model is called by this MCP. E2B, including its
temporary replacement of the 135.5M specialist, remains replaceable and outside
this release. External Claude/Codex consumes evidence without writing native
beliefs or controlling the native cognition loop through its answer.
P3. All lawful native experience, six outcomes, provenance/revisions and actual
conflicts remain addressable. Transport pages and deferred references are not
memory caps, ranked acceptance, truth certificates or evaluator allowlists.
P4. Freeze is limited to one full-current request snapshot. Native successor
publication stays main-owned and subsequent requests can use the new generation.
No experience store, snapshot, model, credential, private history or native main
implementation is a publication input. Public first-party transport modules and
synthetic protocol tests are the release inputs.
P5. This is a software/installation/transport release, not growth. Growth requires
new outcome-bearing observations, assimilation/VRS and later diverse changes,
retention and correction. Repeated test records, model scores and timing probes
are subordinate diagnostics, not new independent experience.
P6. Held-out material is not used; ordinary cognition remains unrestricted.
World/actions/persistent writes/model updates/distribution/P3 remain separate
main gates. Public repository update and release of the MCP interface are
explicitly user-authorized; original source data and unrelated private code are
not included. Existing service restarts, whole-VRS reruns, initialization work,
training and GPU changes are outside this release.

1. yes — P1: native main is the sole cognition owner; public MCP only transports.
2. yes — P2: E2B/135.5M remain replaceable, no model invocation in this release.
3. yes — P3: all outcomes and source addresses remain accessible, no allowlist.
4. yes — P4: only one request snapshot is pinned; successor access remains.
5. yes — P5: only genuinely new outcome-bearing experience supports growth.
6. yes — P5: growth is longitudinal; protocol and timing checks are subordinate.
7. yes — P1/P2: replacing models or clients does not replace native identity,
   accumulated experience, learned connections or authority.
8. yes — P6: held-out isolation and every consequential gate are retained;
   publishing this bounded interface is authorized by the explicit request.

AGENTS preflight: PASS

## Rozephine의 판단

Existing native activation receipts are evidence inputs; publication does not
make them current truth, grant authority or establish general memory relevance.

## Codex의 판단

Provide an installable public bridge with Claude configuration and real stdio
verification. Keep standalone stateful storage versus native VRS2 activation
explicit. The complete native engine and private experience are not bundled.

## Codex 작업 실수 및 교정

Earlier MCP work left source navigation and cursor cleanup to many external model
round trips. A bounded context packet and automatic finished-cursor cleanup were
added. The first context test had a too-small presentation-node budget and a test
fixture with the wrong cursor argument mapping; these were repaired. A review
also found a missing total MCP-envelope budget, now covered by a large-source
regression. The first fresh Codex client failed because the chosen external model
was at capacity after status only; it did not submit recall and is not a success.
The subsequent client completed context use and release in three tool calls.
These are transport/usage results, not a native engine or growth completion claim.

## Completed release checks

Initial public native test collection failed before execution due to an
unqualified fixture import. Corrected to tests.native_fixture: 24 passed in
1.74 s. Full suite then passed 219 tests in 9.91 s. Existing upstream port hashes:
34 PASS. Version 2.0.0 wheel/sdist built. An isolated wheel environment, without
PyTorch or private native Python packages, exercised actual stdio/Unix native
context and release: 3 complete records with original provenance/current verdict
and explicit remaining coverage, 3.155887385 s context / 3.237727440 s total.
Native snapshot unchanged; no native restart or source mutation. This actual
backend probe complements but does not change the synthetic nature of public
wire tests. No Claude executable was available; live Claude testing remains open.

The final validation report was written after these evaluations. Publication
checks inspect the selected release source and final archives before commit/push.
