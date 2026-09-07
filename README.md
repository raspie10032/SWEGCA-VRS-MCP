# SWEGCA + VRS MCP

Independent, model-agnostic **read-only MCP server for the public SWEGCA/VRS
reference components**. MIT licensed. No private entity, resident service,
model, GPU, screen/audio collector, private experience or account is required.

This is a bounded component port, **not a complete autonomous cognitive system**.
It runs the actual published memory activation, VRS promotion-projection and
source-provenance decision code. It does not manufacture a replacement VRS
algorithm. Full-graph convergence, learned-strength generation, online
assimilation, durable World commits and actuators are not implemented here.

## Install and run

Python 3.11+; the development environment is Python 3.12.14. Use an isolated
environment; dependencies are the official MCP SDK 2.x and its dependencies.

```sh
python -m venv .venv
.venv/bin/python -m pip install -e '.[test]'
.venv/bin/swegca-vrs-mcp --dataset examples/synthetic.json
```

The last command speaks MCP on stdin/stdout; a terminal will wait for protocol
input. Use an MCP host or SDK client, not ordinary chat text. Logs go to stderr.
On Windows use `.venv\Scripts\python.exe` and `.venv\Scripts\swegca-vrs-mcp.exe`.
No network listener or background service is installed. Disconnecting the MCP
host stops its stdio child; no capture/experience jobs are started.

Example host configuration (replace both absolute paths):

```json
{
  "mcpServers": {
    "swegca-vrs": {
      "command": "/absolute/path/SWEGCA-VRS-MCP/.venv/bin/swegca-vrs-mcp",
      "args": ["--dataset", "/absolute/path/SWEGCA-VRS-MCP/examples/synthetic.json"]
    }
  }
}
```

This is a portable example, not an automatic edit to any host's global settings.
Host-specific configuration formats can differ. Current and legacy MCP clients
are exercised separately in the SDK subprocess tests.

## Tools and resource

| Interface | Operation | Boundary |
|---|---|---|
| `system_status` | Dataset identity, outcome counts, capabilities | No cognition or growth claim |
| `get_episode` | Exact episode and provenance | No file-path or arbitrary URL access |
| `recall` | Déjà vu → Recall with paginated candidates | Pagination limits returned rows, not memory access |
| `evaluate_vrs` | Déjà vu → Recall → Replay → Re-evidence and five diagnostic arms | Caller assessments are untrusted conditional proposals |
| `swegca://capabilities` | Read-only capability resource | No action/write authority |

The five original diagnostic arms are current VRS, frozen VRS, no VRS,
base-only promotion and repair-only promotion. Every arm recalls the same
related records, including failure, negative, uncertain, conflicting and pending
outcomes. Role restrictions affect only diagnostic promotion, never ordinary
memory access. The no-VRS arm is a causal diagnostic, not the normal architecture.
Strength `>= 1.0` allows the published conditional re-evidence path; it grants no
World, action, write, model update, distribution or P3 authority.

## Example call

With the **synthetic** example dataset:

```json
{
  "query": "demo",
  "current_cues": ["demo"],
  "assessments": [{
    "episode_id": "vrs-edge-group:7",
    "proposition": "demo-outcome",
    "verdict": "support",
    "rationale": "Constructed conditional evidence for a software test",
    "current_evidence_refs": ["fixture:current"]
  }]
}
```

Call `evaluate_vrs` with those arguments. Expected branch decisions are
`success, abstain, abstain, abstain, success`. These are labels from a constructed
source-outcome assay, **not proof of learning, real-world success or an isolated
empirical VRS effect**. Missing assessments produce abstention. Contradictory
current evidence and opposing source outcomes remain visible and cause
abstention on the affected decision. User/model-supplied evidence references
are not fetched or authenticated by this server.

## Datasets, provenance and authority

The operator chooses one local JSON dataset at launch. It is validated and
loaded once into a detached immutable hot index. No model-selected file read,
dataset reload, shell, write, ingestion, URL fetching or strength-edit tool exists.
Use `examples/synthetic.json` as the schema example; Pydantic rejects unknown
top-level fields, duplicate IDs, bad strength references and nonfinite strengths.
Unknown/missing source bindings fail or abstain rather than inventing evidence.

`dataset_id` hashes the complete normalized dataset, including both projections;
`projection_content_sha256` binds the actual strength map. A supplied VRS report
identifier is not independently authenticated. Compare **dataset_id**, not just
a caller-declared report ID, when checking identity across clients/restarts.
All records in the chosen dataset remain addressable. This does not claim access
to unprovided data elsewhere on the operator's machine. A larger/new generation
can be supplied at a later launch; this MVP has no online accumulation path.

Read-only means the **server has no consequential write tools**. An external MCP
host can still act on tool output under its own permissions, so it must not treat
these diagnostics as authorization. Tool annotations are hints, not an access
control mechanism. Anyone given this local stdio server can read its loaded
dataset. Do not connect private data to an untrusted/remote-model host.

The hot component decision path does no disk/network/JSON/hash work. MCP input
validation and output serialization are a separate transport boundary and incur
ordinary overhead. Evaluation receipts are limited to 2 MiB at that boundary;
oversize replies return an error, never silently truncated cognitive evidence.
Each request allows up to 256 cues and 4096 conditional assessments; these are
transport payload bounds, not caps on stored memory or recalled candidates.
Recall supports output pagination. Startup loads and hashes the supplied dataset;
there is no per-request full-corpus reload. This is not a large-corpus performance
or production-hardening claim.

## Verify

```sh
.venv/bin/python -m pytest -q
.venv/bin/python tools/verify_port.py
```

`tools/smoke_mcp.py --server /absolute/path/to/swegca-vrs-mcp --dataset
/absolute/path/to/examples/synthetic.json` exercises an installed server over
stdio and closes it afterwards. Run with the environment's Python; optionally
add `--mode legacy` to check the older initialization flow.

Tests cover the public components, detached state, all outcome types, provenance,
promotion/revocation, conflicts, source-decision gating, hot-path I/O guards,
malformed requests and real MCP stdio subprocess calls. One original test tied
to a private resident service is deliberately **not ported**; it is not counted as
passing. New standalone MCP integration tests replace only transport coverage,
not evidence of the excluded private system.

## Source and license

Derived from the MIT public
[SWEGCA-VRS-PoC](https://github.com/raspie10032/SWEGCA-VRS-PoC), pinned at
`9d0fcc9eb58c2fec1aea5d6034b9b0a886208bb3`. `UPSTREAM.json` records source and
ported hashes, namespace/schema-label transformations and the excluded test.
The numerical/decision algorithms were not rewritten. No private Git history,
models or media are imported. This project does not copy or relicense the
separately published MPL-2.0 architecture package.

MCP transport uses the [official Python SDK](https://py.sdk.modelcontextprotocol.io/).
The MIT license covers first-party code here, not third-party dependency licenses.
This package is prepared locally for public use; no remote repository or registry
release is implied by these files.

## Codex 작업 실수 및 교정

See [the chronological development ledger](docs/IMPLEMENTATION_NOTE.md) for the
initial scope correction, implementation failures, repairs and verification limits.
