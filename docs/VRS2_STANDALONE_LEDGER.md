# Windows standalone VRS2 correction — 2026-09-14

Status: implementation and validation pending. This is not a completion report.
Base: public main 2eeee70cc9faea25d05ec4c9745be4f6f2195a98 (v2.0.0).
The user requires VRS2 MCP on one Windows machine, independently of Linux main,
and without Hermes in the distributed artifact. This supersedes the bridge-only
release scope; it is a repair of omitted product requirements, not an optional
feature request. Preserve prior releases, local stores and private dirty work.

## Corrected implementation contract

W1. Include a local main owner and the actual required VRS2 memory runtime in
the standalone distribution. Main exclusively owns identity, persistent state,
all accumulated experience, hot indices, VRS lifecycle, arbitration and authority.
The MCP boundary transports requests and receipts. Do not substitute the old
stateful star graph, a protocol fixture, or a Linux bridge for the VRS2 engine.
Port required first-party components with source lineage and equivalence checks;
do not publish unrelated private code, experiences, model weights or credentials.

W2. The runtime invokes no internal LLM and this memory task invokes no final
utterance LLM. E2B, including replacement of the 135.5M specialist, remains a
replaceable specialist outside this task. Models and MCP clients own no durable
identity, memories, learned connections or authority. Client-provided memories
are external observations, not accepted beliefs or model-directed cognition.

W3. Preserve all lawful accumulated experience addresses, provenance, revisions,
uncertainty and six outcomes. Use hot related-address lookup; no arbitrary top-k,
evaluator allowlist or full scan on each query. Preserve the four activation
stages, actual same-proposition conflicts and replayable selection receipts.
Returned pages bound transport only. Retain complete source access across pages.

W4. Pin each judgment to one immutable full-current memory/VRS snapshot.
Validate ingress and publish successor generations atomically, retaining earlier
source revisions. New accepted observations become accessible on the next query.
Persist the local owner/store and restore it on restart. Use incremental updates
and structural sharing where supported; record any necessary full recomputation.

W5. Installation, persistence, equivalence, latency and regression checks are
software diagnostics, not growth evidence. Growth requires new outcome-bearing
observation, assimilation/VRS and later source-diverse change, retention and
correction measured longitudinally. Repeated and synthetic rows are not new
independent experiences; model-style metrics remain subordinate diagnostics.

W6. Held-out material stays outside development; ordinary memory access is not
restricted. Observation ingress grants only the explicitly configured local
recording permission. Current VRS evidence promotion/revocation does not grant
World, action, persistent-belief, model-update, distribution or P3 authority;
those remain separately gated by main. Reject attempts to assert such grants.
Existing live main/GPU services, full live VRS reruns, initialization optimization,
speech and training remain untouched. Use CPU and a fresh test store.

## Eight-invariant audit for this correction

1. yes — W1 makes local main the sole identity/state/experience/VRS/authority
   owner, not the MCP adapter or an external client.
2. yes — W2 explicitly confines E2B/135.5M to replaceable specialist roles and
   permits no model execution in this memory runtime task.
3. yes — W3 retains every accumulated address/outcome and forbids allowlists;
   pagination does not define cognitive access.
4. yes — W4 freezes only one judgment snapshot and publishes later generations
   with durable source revisions, without freezing experience accumulation.
5. yes — W5 requires genuinely new outcome-bearing experience for any growth
   claim and labels synthetic/repeated verification as diagnostics.
6. yes — W5 defines growth by the longitudinal system trajectory, not model
   metrics or test counts.
7. yes — W1/W2/W4 store identity, experience, connections and authority in main;
   model/client replacement does not remove them.
8. yes — W6 preserves held-out isolation and separate consequential gates while
   W3 preserves ordinary cognitive addressability.

AGENTS preflight: PASS

## Required release evidence

| User requirement | Evidence required before completion | Current status |
| --- | --- | --- |
| Windows on one machine | Actual Windows fresh artifact installation and MCP stdio lifecycle, without Linux/WSL/remote backend | Missing |
| Required VRS2 engine included | Dependency-closed first-party runtime, source lineage and meaningful native equivalence checks | Missing |
| New memory works | MCP observation ingress, atomic successor and recall with original text/provenance | Missing |
| Persistence works | Process exit/restart, same store identity and restored memory/VRS retrieval | Missing |
| Hermes absent | Inspect both wheel and source archive; imports, entrypoints, dependencies and integrations excluded | Missing |
| Memory use structure retained | Four-stage receipt, actual conflicts, all outcomes, same snapshot, complete paged access | Bridge only; standalone missing |
| No hidden cognition LLM | Runtime dependency and normal/error-path blocking tests | Standalone missing |
| Ownership and permissions | Duplicate owner, failed transaction, stale generation, forged grants and restart checks | Standalone missing |

No number of passing tests substitutes for a missing row above. Actual Claude
application testing must be reported separately from MCP SDK/stdio verification.

## Rozephine의 판단

No new cognitive run in this correction preflight. Prior experiences and judgments
remain unchanged; memory availability does not establish current factual truth.

## Codex의 판단

v2.0.0 proves a bridge against an existing Linux main, not a standalone Windows
memory system. The product's required backend, persistence and installation
boundary must be verified from the distributed artifact in a fresh environment.

## Codex 작업 실수 및 교정

During v2.0.0 extraction I narrowed the release to transport and treated Linux
connection tests as sufficient delivery evidence. I checked that Hermes was not
called by the entrypoint but left Hermes in package discovery and source archives.
The user's separation and Windows reports exposed the missing backend and wrong
artifact boundary. Impact: the user received an artifact unable to meet standalone
Windows use, requiring another correction. The old release and its limited test
evidence remain preserved; it is not retroactively a standalone success. The
corrective gate is the requirement-to-evidence table above. No code fix or Windows
success has yet been claimed. This turn also combined too much document output,
causing truncation and extra chunked reads; no source/runtime mutation resulted.

## Resumed implementation audit

User explicitly repeated implementation and repository delivery after Codex stopped
at documentation. Existing W1-W6 are the proposed implementation contract.
1. yes W1: local main owns persistent identity/state/memory/VRS/authority.
2. yes W2: E2B/135.5M replaceable; no internal or final model invocation.
3. yes W3: unrestricted accumulated addresses and all six outcomes.
4. yes W4: judgment snapshot only; atomic ingress successor and restart retention.
5. yes W5: growth requires genuinely new outcome-bearing experience.
6. yes W5: longitudinal growth; current software tests are diagnostics only.
7. yes W1/W2/W4: models/clients own no durable memory or identity.
8. yes W6: held-out and consequential gates retained, ordinary access unchanged.
AGENTS preflight: PASS

### Codex 작업 실수 및 교정
Codex twice ended its turn after explanation/status instead of implementing the
already authorized correction. No external blocker justified stopping. The user
had to repeat the request. Continue through code, tests and repository delivery;
do not replace implementation with this audit or imply the old release is fixed.

## Implementation and Linux validation in progress

Native first-party source port: twelve modules/selected definitions with exact
source hashes in NATIVE_VRS2_PORT.json. New local main owns SQLite observation
journal, persistent immutable maps, native event signal/re-evidence strengths and
affected-component overlapping topology. UTF-8 original receipts are transported
in process; no Unix socket or remote main. Models/Hermes are not dependencies.
New standalone tests: first 13 passed/0.21 s; actual SDK stdio/restart extension
18 passed/1.16 s; ingress strength/correction checks 20 passed/1.15 s. These are
overlapping runs, not 51 independent tests. Windows execution still pending.
Resources: local disk free 817436819456 bytes at preflight, CPU only, no E: writes,
models/GPU/live-main restarts/full live VRS runs zero. New synthetic test stores
are not real cognitive growth. Commands and XML/logs in /var/tmp/vrs2-* locally.

### Codex 작업 실수 및 교정
The first extraction script assumed the wrong symbol name extend_event_inputs;
the actual native function is prepare_event_delta. Corrected before integration.
Initial direct page integration passed mutable root mappings; native pager
rejected them. Bound immutable root views and matched cursor-release signatures.
First archive verification rejected legacy Hermes docs/tools retained by source
manifest inputs. Preserve that failed build in /var/tmp/vrs2-standalone-dist;
explicitly prune old docs/tools/tests/integrations then include only standalone
release inputs. No bad archive has been published. Earlier stopped turns and
bridge-only scope error remain unresolved until actual verified delivery.

Archive correction verified: wheel and sdist now have zero Hermes/agent-service/
legacy-package files; required native engine/main/store present. Fresh wheel-only
environment ran all 20 standalone checks (1.76 s), including actual SDK stdio
with auto and legacy modes and restart persistence. A subsequent immutable-view
sharing correction is covered by the final source suite and upcoming Windows CI.
Legacy unchanged source regression: 216 passed, 3 failed due SQLite disk I/O
errors under /tmp. No legacy source/test changes. Retained original XML/log;
rechecked only the three failures with a new /var/tmp base: 3 passed/0.78 s.
The original transient I/O cause is not proven; do not erase or relabel that run.
Next publication is a clearly unfinished validation branch to run actual Windows
CI. This is not phase closure or a release tag. Final main/tag follow verified
Windows results and the required completed report.

## First actual Windows CI failure and correction

GitHub run 34804004050, candidate 7536139: Ubuntu installed-artifact job passed.
Both Windows 3.11/3.12 jobs built archives then failed the exact native source
hash check (mosaic_vrs_address_index). Windows checkout converted LF to CRLF;
Codex had omitted repository eol rules. Added .gitattributes to retain LF text
bytes instead of weakening hash verification. No Windows runtime pass claimed
from this failed run. Also added direct original episode-address retrieval
through the same four-stage API so lexical query matching is not a prerequisite
for accessing a known stored address. New address-specific regression included.
### Codex 작업 실수 및 교정
Missing Windows checkout line-ending contract was an implementation/packaging
omission by Codex. CI caught it before release. Prior invalid Windows run and
logs are retained; rerun is necessary because the relevant checkout was changed.
