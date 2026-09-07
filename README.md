# SWEGCA + VRS MCP

**v0.2: persistent experience storage, recall and VRS tools for existing agents.**
Your agent is the MCP client; this server owns its external memory store. It does
not host, call or replace an LLM, intercept conversations or control the agent.

Unlike the historical v0.1 read-only preview, core mode can record new experience,
update its VRS graph, survive restart, recall stored content, re-evaluate evidence
and request gated, bounded verification-state commits. This is a tested software
integration, not a claim of autonomous cognition or empirical cognitive growth.

## Install and connect to Claude Code

Requires Python 3.11+ and [uv](https://docs.astral.sh/uv/getting-started/installation/).
Linux is verified; Windows/macOS runtime integration is not yet verified.

```sh
git clone https://github.com/raspie10032/SWEGCA-VRS-MCP.git
cd SWEGCA-VRS-MCP
uv sync --locked --extra core
```

For an existing clone, run `git pull --ff-only` first. The core extra includes
PyTorch, NumPy and filelock. The uv lock uses CPU-only PyTorch on Linux/Windows;
no model weights or CUDA packages are needed. Plain pip does not use uv's index
configuration and may select a larger PyTorch distribution.

In the project where you use Claude Code, register **absolute paths** to the
installed command and a **private local state directory outside this Git clone**:

```sh
claude mcp add --transport stdio --scope local swegca-vrs -- \
  /absolute/path/SWEGCA-VRS-MCP/.venv/bin/swegca-vrs-mcp \
  --state-dir /absolute/path/private-swegca-memory --enable-writes
claude mcp get swegca-vrs
```

If the name is already registered to v0.1, inspect that entry and deliberately
replace just that entry with the command above; do not delete unrelated servers.
Inside Claude Code check `/mcp` and reconnect if necessary. Approval and execution
permissions belong to the host/user, not this server. Registration syntax follows
[Claude Code's official MCP guide](https://code.claude.com/docs/en/mcp).
**Actual SDK stdio processes are tested; a live Claude Code session is not yet
verified.** On Windows use the absolute `.venv\\Scripts\\swegca-vrs-mcp.exe` path.

Ask the agent:

> Use swegca-vrs. Read system_status. Record one clearly labeled test experience
> using record_experiences with the current revision and a unique request_id.
> Use an actual integer Unix nanosecond timestamp. Then call converge_vrs and
> recall the record by its words. Do not treat stored agent notes as verified
> truth. Report actual tool results, not just installation success.

The writable server exposes **nine tools**. Omitting `--enable-writes` opens an
existing store read-only with five tools; it cannot create a new store.

## Tools

| Tool | Function |
|---|---|
| `system_status` | Owner identity, revision, outcome counts, memory/VRS snapshots |
| `record_experiences` | Atomically store observation/outcome records and update the hot index |
| `get_episode` | Read an exact event and provenance |
| `recall` | Return related episodes and content by lexical words/cues |
| `converge_vrs` | Run CPU VRS rounds and persist graph strengths, scores and convergence receipt |
| `judge` | Déjà vu → Recall → Replay → Re-evidence for a hypothesis/current event |
| `get_world` | Read bounded verification state and current claim authority |
| `commit_judgment` | Request evidence, arbitration and bounded-write gates |
| `rollback_world` | Undo only the latest World commit without deleting experience |

Resource: `swegca://capabilities`. No shell, arbitrary file read, network actuator,
model-update, distribution or P3 tool is exposed.

## Experience format and agent workflow

`record_experiences` takes `observations`, `request_id` and `expected_revision`.
Each observation needs:

- `event_id`: immutable source-event identity; identical retries do not add rows.
- `hypothesis_id`: the proposition the observation bears on.
- `producer_id`, `context_id`: provenance, not a claim of independent truth.
- `axis`: observational, counterfactual, intervention or cross_context.
- `outcome`: success, failure, negative, uncertain, conflict or pending.
- `observation`: a JSON object holding the actual content.
- `evidence_refs`: nonempty source references (not automatically fetched).
- `observed_at_ns`: integer Unix nanoseconds, not seconds or milliseconds.
- Optional `cues`, `expires_at_ns`, `supersedes` and producer `signature`.

Read the revision from `system_status` or the last successful mutation receipt.
Use a unique request ID for each mutation. After a lost response, retry the
**same ID and payload**; committed operations are recovered without duplication.
Stale revisions fail closed. One server process exclusively owns a store at a
time; different agents can use the same persistent directory sequentially.
Multiple simultaneous stdio processes cannot share it; a shared multi-client
service is not implemented.

Agent integration is explicit: record observed outcomes, converge when useful,
recall before related work, and submit later contradictory outcomes as well.
MCP registration alone does not guarantee a model will call these tools on each
turn. Repeated examples or new row IDs are not evidence of distinct experiences;
producers remain responsible for honest source identity and lineage.

## Verification versus ordinary memory

**No producer key is needed for ordinary storage and recall.** Unsigned,
unsuccessful and unresolved records remain accessible and participate in the
graph. They are not automatically elevated to verified belief.

For authority-bearing evidence an operator may supply
`--producer-keys /private/path/producer-keys.json`. The owner-only JSON file
contains `producers`, mapping each producer ID to `key_hex` (32–64 bytes encoded
as lowercase hex), `source_family` and `allowed_axes`. A producer signs the
canonical observation excluding `signature` with HMAC-SHA256; the Python helper
is `swegca_vrs_mcp.observations.sign_observation`. Never put keys in prompts,
MCP arguments, public repositories or model-visible files. There is no MCP
signing tool. Store policy is bound at creation; changing it requires a future
explicit migration, not an implicit change of trust on reopen.

Authentication proves origin under the configured trust policy, **not physical
truth or source independence**. Source/context/axis sufficiency, current evidence,
VRS state, contradiction handling, arbitration and bounded World gates still
apply. A single agent assertion is not enough to pass those gates.

## Implementation boundaries

- All six outcomes remain hot-addressable; pagination only bounds responses.
- Recall uses normalized lexical tokens/cues, not embeddings or an LLM semantic
  parser. Precise `hypothesis_id` values identify propositions for `judge`.
- SQLite transactions persist raw events, VRS generation, state and successful
  operation receipts. Hot recall/judgment does not do disk/JSON/hash work.
- VRS uses the existing numerical refinement kernel with a **new star-graph
  adapter** connecting observations to hypotheses. This is not a port of a
  complete multimodal experience graph. It does not invent cross-proposition
  semantic connections.
- Refinement uses reinforcement 1.01 and weakening 0.995. All outcomes are active;
  there is no pre-convergence pruning. Maximum rounds are bounded; insufficient
  convergence remains pending and cannot authorize a World commit.
- VRS arithmetic currently traverses the whole graph on an explicit convergence
  call. Local-only numerical equivalence is not established. It is not run on
  every recall; incremental large-graph convergence is future work.
- A `>= 1.0` connection alone grants no World/action authority. Expiration
  invalidates cached authority at read time; explicit convergence refreshes it.
- World writes affect the published kernel's **bounded verification slot**,
  not arbitrary semantic entities or an agent's private internal state. The
  32-slot/8-wide state adapter is a bounded integration representation, not a
  claim that all experience is encoded in those tensors.
- Supersession is authenticated, same-producer and same-proposition; old records
  remain stored and active. New evidence invalidates affected committed claims.
- Resource and concurrency limits are tested only on small synthetic stores.
  This is not a production-hardening or cognitive-growth claim.
- A local user able to edit the store and keys is outside this integrity model.
  Do not expose private stored content to an untrusted MCP host.

## Legacy diagnostic mode

`swegca-vrs-mcp --dataset /absolute/path/examples/synthetic.json` preserves the
v0.1 four-tool read-only mode, including `evaluate_vrs` and its five comparison
arms. Its supplied strengths and caller assessments are conditional inputs.
That mode does **not** ingest new experience or expose the stateful core tools.
Historical verification: [v0.1 report](docs/VALIDATION.md).

## Verify

```sh
uv sync --locked --extra core --extra test
.venv/bin/python -m pytest -q
.venv/bin/python tools/verify_port.py
.venv/bin/python tools/smoke_core_mcp.py --server /absolute/path/to/swegca-vrs-mcp
```

The smoke check creates and removes only its own temporary synthetic store.
It tests actual MCP storage, VRS, recall and recall after a new server process.
See [the core ledger](docs/CORE_NOTE.md) and [core validation](docs/CORE_VALIDATION.md).

## License and provenance

First-party integration is **MIT**. Imported public SWEGCA-Architecture files and
their tests retain **MPL-2.0**; the combined distribution is not MIT-only.
See [NOTICE](NOTICE.md), LICENSE and LICENSES/MPL-2.0.txt. The three upstream
manifests record hashes and transformations. No private entity, private Git
history, personal experience, models, media or credentials are bundled.

## Codex 작업 실수 및 교정

The earlier read-only release did not fulfill the requested stateful integration.
Its limitations and the subsequent repairs remain in the chronological ledgers;
protocol success is not substituted for real-client or cognitive-growth success.
