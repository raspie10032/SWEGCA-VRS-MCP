# Independent public MCP port — development ledger

## Scope and preflight (2026-09-07)

The user explicitly requested an independent public SWEGCA+VRS MCP package,
not a connection to the private cognitive entity. This is a new local package;
no remote creation/publication, user-wide MCP registration, private runtime,
model, media, snapshot, credentials, service restart, or experience collection
is included. No new long-term goal is registered.

Public source: raspie10032/SWEGCA-VRS-PoC, commit
9d0fcc9eb58c2fec1aea5d6034b9b0a886208bb3, MIT. Only the four standalone
memory/causal/provenance modules and their component tests are candidates for
porting. Namespace and schema identifiers will be renamed with recorded lineage;
no private runtime implementation is imported. The older architecture package
has MPL-2.0 components and is not silently relicensed or included.

1. Main ownership: yes. A standalone server owns its loaded immutable memory/VRS
   generation; MCP clients receive detached diagnostics and cannot mutate it.
   No existing entity's identity or state is touched.
2. Specialist boundary: yes. No model or 135.5M/E2B substitution is shipped.
   Client re-evidence assessments are explicitly untrusted conditional inputs,
   never authority or independently verified observations.
3. Experience access: yes. All records in the operator-supplied dataset remain
   hot-addressable. Runtime cues select candidates; no per-record allowlist.
4. Freeze: yes. An immutable generation is used per diagnostic. Loading a new
   operator dataset on the next server launch is supported; this read-only MVP
   does not impose a lifetime cap or pretend to implement online assimilation.
5. New experience: yes. No growth claim is allowed from fixture rows, repeated
   requests, protocol tests or imported projections. Future growth would require
   new outcome-bearing observations and later changed cognition.
6. Evaluation: yes. Here tests are software diagnostics only; any future system
   growth claim requires longitudinal outcomes, retention and refutation, not
   an LLM score or a protocol PASS.
7. Replaceability: yes. State and source lineage live outside any external model
   context. No specialist model dependency or cross-client conversation owner.
8. Authority: yes. No World/action/model-update/distribution/P3 or persistent
   write tool. Held-out evidence is not bundled or consulted. User-supplied
   verdicts/strengths have no capability to bypass a write gate.

AGENTS preflight: PASS (scope exclusion and bounded portable-component port).

## Implementation/evaluation boundary

Use the official MCP Python SDK, stdio only, no listening socket. Validate
startup datasets and tool payloads; no arbitrary file/network/shell tool.
Expose status, episode lookup, four-stage re-evidence and five-arm VRS diagnostic
with provenance-based source decisions. Preserve all six outcome categories.
Only the public promotion-projection/decision algorithm is available: full graph
convergence, learned-strength generation, autonomous ingestion and persistent
World commits are NOT implemented by this phase. A fixture is labeled synthetic.
Verify source lineage, unit regressions, real SDK client/server stdio calls,
malformed inputs, absent evidence, conflicts, no-authority and clean shutdown.

## Codex 작업 실수 및 교정

- Initial commentary assumed a private-main adapter. User corrected the scope
  before any edits. Stopped that path, reread the contract and created this
  independent plan; no private source/state was copied or service started.
- A combined inspection exceeded the tool output budget. Reread the entire
  contract separately before implementation; no truncated read used as a PASS.
- `uv pip index versions mcp` is not supported by the installed uv. The read-only
  command failed without mutation; dependency availability will be resolved by
  the actual isolated environment install instead.

## System judgment

Evaluation now completed for this bounded component port (results below).
No empirical judgment or growth result is claimed.

## Implementation observations (before final verification)

- Official SDK documentation and actual isolated installation agree on v2;
  installed MCP 2.1.1, Python 3.12.14, pytest 9.1.1. No torch/GPU/model needed.
- Four public algorithms were ported by namespace/schema-label transformation.
  Four component test files were ported with one explicitly omitted private
  integration test. The 62 retained public component tests passed.
- First wrapper tests: 5 failed, 13 passed. Codex passed the provenance object
  as a callable instead of its `.decide` method. Corrected the adapter callback;
  all 18 runtime tests then passed. No source algorithm or threshold changed.
- First MCP suite: 3 failed, 86 passed. Codex used an old camelCase Python
  annotation attribute; SDK v2 exposes snake_case. Corrected server/test fields.
  Next checks exposed that bare `dict` return annotations did not produce
  structuredContent. Changed the adapter signatures to `dict[str, Any]`;
  retained assertions and all 9 MCP tests then passed, including real stdio
  auto/legacy clients. Combined result at that checkpoint: 89 passed in 2.08s.
- Initial build succeeded but inspection showed the sdist omitted examples,
  lineage and verifier. Added explicit MANIFEST.in rather than claiming a
  reproducible source archive from build success alone. A combined patch had
  an incorrect README context and was rejected without partial edits; checked
  exact state and reapplied the corrected patch.
- Added request/response bounds and tests; these do not restrict the loaded
  dataset or the actual candidate set. Final results follow after rerun.

All above implementation mistakes belong to Codex, not to a model or the
published numerical/decision algorithm. No private service was restarted,
private data imported, remote repository created or host configuration changed.

## Codex judgment

Development ledger, not a completed release report. Public source boundaries and
the user's exclusion of the private entity take precedence over earlier plans.

## Final verification checkpoint — 2026-09-07 12:06 KST

- Full suite: **91 passed in 1.98s** (62 public component tests, 18 new runtime
  tests, 11 MCP/protocol/boundary tests). No skipped tests in this suite. The
  separately documented private integration test was not ported or counted.
- `tools/verify_port.py`: PASS for all 8 ported source/test files.
- `uv build --quiet`: wheel and source archive built. Source archive inventory
  confirmed the example dataset, UPSTREAM manifest, ledger and verifier.
- Installed the wheel into a second isolated environment outside the checkout;
  module resolution confirmed site-packages, not editable source. Actual stdio
  smoke calls passed for both `auto` and `legacy` protocol modes. Both produced
  the expected 5-arm synthetic decisions and all-false authority flags.
- MCP tests cover tool discovery, structured results, resource reads, invalid
  requests, empty evidence, unchanged state and context-managed child shutdown.
  No persistent MCP service or global host registration remains.
- Dataset identity in both installed-wheel runs:
  `ae9b004f2a269b4ead73852072b669da06224e7bb1c91f68ab973b4ebafea489`.
- Current code SHA-256:
  - server.py: `123ffacec1182c0acba9885bf6330f542247c9027bf43cec83944ce2a1878dca`
  - runtime.py: `bbb1fb15f2a052b29a9f891a4c7d6246cfa2717ad304ec8569f49bcc00856e39`
  - synthetic.json: `1390b305344d488ddc0ee3280049a4bb1d582e260677325e4197b0842a113cc5`
- New local environment occupied about 47 MiB; no GPU/model allocation, large
  download or experience acquisition. Existing user-stopped services stayed off.
- Packaging smoke environment fell back from hardlinks to copies across
  filesystems; installation succeeded. This is a packaging/environment warning,
  separate from the Codex implementation mistakes recorded above.

## Remaining boundaries

Final staged whitespace check rejected two extra EOF blank lines in packaging
scaffolding before the first commit. Removed those lines and reran the check;
no numerical code or test expectations changed.

This is an independently runnable read-only **reference-component MCP port**,
not a completed port of the entire SWEGCA cognitive kernel or VRS convergence
system. No persistence writer, World gate/commit endpoint, evidence acquisition,
strength training or graph-convergence implementation is shipped. Future work
must preserve numerical lineage and separately design/authenticate those write
boundaries instead of granting authority to caller-supplied assessments.
No real-client GUI configuration, remote repository, registry publication,
empirical learning result, large-dataset performance claim or Windows runtime
test is included. Source release preparation is local only.
