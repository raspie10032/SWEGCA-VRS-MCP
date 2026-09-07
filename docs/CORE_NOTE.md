# Stateful core integration ledger — 2026-09-07

## Scope repair and preflight

User approved adding the missing core, not another read-only wrapper. Implement
new observation accumulation, active mixed-outcome VRS refinement/convergence,
four-stage memory use, SWEGCA accumulator/arbitration/bounded World commits,
atomic durable storage, restart recovery and rollback in this independent package.
No existing entity's data, identity, services, models, captures or settings enter.
Continuation: the user now requests the missing implementation in the already
public MCP project. Publish only verified standalone changes to that project's
main; no global MCP registration, private entity data or service activation.

1. Main ownership: yes. One server instance owns one CognitiveState and one
   full-current memory/graph generation. Exclusive store lock prevents two owners.
2. Specialists: yes. No 135.5M/E2B/model substitution or embedded LLM. Clients
   submit observation proposals; authentication and all gates are server-owned.
3. Access: yes. All admitted records, including unsuccessful/unresolved records,
   remain hot-addressable. Response pagination is not a cognition allowlist.
4. Freeze: yes. Each operation holds one immutable generation; new observations
   create later generations. No fixed experience count or permanent projection.
5. Growth: yes. Repeated IDs/content cannot be counted as new evidence. Tests
   are synthetic diagnostics, never a claim of longitudinal cognitive growth.
6. Evaluation: yes. Test trajectories cover accumulation, changed later decisions,
   contradiction, retention and recovery; software passes are not real-world growth.
7. Replaceability: yes. Durable state and provenance are stored outside any model
   context. Workers/proposals cannot acquire the main's mutable tensor references.
8. Authority: yes. Store mutation requires an operator-enabled writable launch.
   Raw input is not verified belief. Only authenticated producer observations can
   enter authority-bearing evidence gates; unknown/bad signatures remain pending.
   World updates additionally require VRS convergence, current re-evidence,
   source/context/axis sufficiency, non-conflicting arbitration and bounded writes.
   No shell, network actuator, model-update, distribution or P3 capability.
   Held-out evidence is neither included nor consulted.

AGENTS preflight: PASS.

## Current-turn continuation preflight — 2026-09-07

Full repository contract reread this turn. The proposed implementation is the
eight clauses above, not the earlier read-only publication: (1) server/store sole
state owner and exclusive lease; (2) no embedded specialist or 135.5M/E2B swap,
all client content remains observation/proposal; (3) all stored outcomes remain
addressable without evaluator allowlists; (4) immutable generations per request,
new writes advance them; (5) synthetic/repeated checks never claim growth;
(6) persistence/refutation/retention trajectories are software diagnostics only;
(7) external clients/models replaceable without losing store identity/lineage;
(8) operator-enabled writes plus separate verified-World gates, no held-out data,
external actions, model updates or P3. Each answer is yes by the corresponding
implementation boundary and test obligation, not a claim of tests already passed.
AGENTS preflight: PASS.

A stateful external tool is not automatic interception of every agent turn.
Clients explicitly submit experience and request recall/VRS. Unsigned experience
is stored and recalled without claiming independently verified truth. Producer
authentication is optional for memory use, required for authority-bearing gates.

## Sources and resource boundary

- Public SWEGCA-Architecture commit 5901a5aa2dcbd0ac7ad12ac6dd745699f72288a8:
  retain MPL-2.0 notices on derived files; namespace/entity-label changes only.
- Existing VRS numerical function `refine_vrs` and its direct-score helper:
  isolate only generic numerical code, not its acquisition/runtime imports.
  Reinforcement 1.01 and weakening 0.995 are retained, not tuned to new tests.
  Provenance and extracted-function hash will be recorded separately.
- New MCP/store integration remains first-party MIT. Do not claim all bundled
  sources are MIT or strip inherited MPL notices.
- CPU-only torch build in the project environment; no CUDA/model downloads.
  Initial capacity: about 943 GiB free disk, 67 GiB available RAM. Small synthetic
  tests and an isolated dependency install fit a 2 GiB working reservation.
- Global VRS arithmetic is retained because equivalence to local-only convergence
  is not established. Do not reload/read/hash the whole source corpus per query.

## Implementation plan

Reuse the published CognitiveState, evidence accumulator, proposal arbiter and
bounded verification-write/rollback primitives. A new transactional store binds
observation records, graph generations, commit receipts and state under one SQLite
transaction. The cold path rebuilds hot indexes once; queries use RAM only.
Operator-configured producer keys authenticate observations; signatures bind all
fields. Authentication is not proof of physical truth. Authority therefore remains
conditional on explicitly trusted evidence producers and published evidence gates.
MCP clients never supply booleans that bypass those gates. Preserve pending data
when graph computation fails; do not promote an unconverged graph.

## System judgment

Final bounded software evaluation is complete; see CORE_VALIDATION.md. Historical
checkpoints below retain their original pre-evaluation scope. No growth claim.

## Continuation checkpoints (software diagnostics, not phase completion)

- First ten new store tests passed: raw six-outcome persistence, lexical recall,
  exact restart snapshot identity, authenticated commit/rollback, CAS/idempotency,
  transaction fault recovery, exclusive ownership, corrupt head rejection,
  hot-judgment I/O guards, unconverged rejection and new-conflict invalidation.
- Three actual MCP subprocess tests then passed in the same run as 73 retained
  public architecture tests: 86 passed in 8.59 seconds. A new process recalled
  stored content; unsigned evidence did not pass authority; signed synthetic
  multi-source observations exercised actual bounded World commit/rollback.
- Interim full suite: 177 passed in 10.54 seconds; all 34 ported/extracted files
  matched source manifests. Further expiry/supersession/retention tests and
  installed-package verification follow; this checkpoint is not final completion.
- Resource refresh: 942 GiB disk free, 68 GiB RAM available. Existing CPU-only
  dependencies reused. uv core extra keeps CUDA/model downloads out of this run.
- Read path: prepared main-owned evidence decisions avoid per-judgment JSON/hash
  work and all-corpus scans; expiry immediately blocks stale authority. Ingestion
  updates the relevant hot bundles; explicit global VRS rebuilds those bundles.
- Persistence: graph generations are stored separately and referred to by hash;
  an experience append does not reserialize the entire unchanged graph.
- Public dependency boundary: first-party integration MIT, 25 public architecture
  source/test files MPL-2.0. No private state or runtime imports.

## Codex judgment

The previous read-only port remains as diagnostic mode. Core mode must be tested
through actual MCP and durable restart, not merely by import/health success.

Final checkpoint: 181 passed in 8.01 seconds, 34 ported/extracted files verified,
clean locked installation and installed-wheel persistence smoke passed in both
auto and legacy modes. Actual import path resolved to site-packages. No live
Claude Code session was available; client attachment remains explicitly unverified.
The installed smoke deletes only its own newly created synthetic temporary store.

## Codex 작업 실수 및 교정

The previous phase narrowed the user's full-system request to public diagnostics.
This phase repairs that omission explicitly without relabeling the earlier result.
During source discovery two guessed filenames did not exist; read-only searches
failed without mutation and actual source names were then resolved with rg.

During this continuation, a combined read again exceeded output limits; the full
contract was reread separately before preflight. Review exposed prior draft
defects: snapshot identity changed on restart, judgments rebuilt/hashed evidence
and scanned all records, odd-length hex keys passed validation, writable SQLite
setup ran in read-only mode, and graph bodies were reserialized on every append.
These are Codex integration mistakes, not model or numerical algorithm failures.
Repairs preserve admission-wave snapshot lineage, cache related evidence on
mutation, validate even-length keys, use a read-only SQLite URI, and persist
separate immutable graph generations. Tests retain failure boundaries.

Function-level source comparison also failed: the earlier generic extraction
omitted the original @torch.inference_mode() decorator. Restored it before
publication; AST equality including decorators now passes for both functions.
No original numerical threshold was tuned. All earlier ten/86-test checkpoints
preceded this exact-source decorator repair and are retained as interim evidence,
not rewritten as the final source-equivalence result.
