# Source and license boundaries

The repository and the distribution contain the first-party `swegca_vrs2` package
only (since 2.2, 2026-09-21; the historical `swegca_vrs_mcp` package — v0.3
stateful core, Hermes adapter and delivery outbox, agent service, v2.0 native
bridge — and its tests, tools, integrations and manifests were removed from the
source tree and remain in git history). No experiences, weights or private
runtime state are publication inputs.

- First-party MCP integration, store, loopback daemon, harness, graph adapters and
  tests: MIT, copyright 2026 Dongjun Park. See LICENSE.
- `src/swegca_vrs2/engine/mosaic_vrs_*.py`, `mosaic_hot_evidence_pages.py`,
  `mosaic_immutable_numeric.py`, `mosaic_memory_activation.py`,
  `mosaic_memory_promotion.py`: MIT first-party port of the native VRS2 numerical
  component; NATIVE_VRS2_PORT.json records exact source and port hashes. No private
  runtime, history, experience, media or credentials are imported.
- `src/swegca_vrs2/engine/mosaic_evidence_accumulator.py`: vendored unchanged (core)
  from the public SWEGCA-Architecture repository, commit 5901a5a, Mozilla Public
  License 2.0 — its file-level notice and LICENSES/MPL-2.0.txt are preserved. The
  proposal-gating helpers that need torch were omitted; the decisions are the
  architecture's decisions.
- `src/swegca_vrs2/vrs_refine.py`: the two generic numerical rules of the v0.2
  refinement kernel (shuffled passes, x1.01 / x0.995, clamp) generalized to the
  experience graph; first-party MIT.

The combined source distribution is not MIT-only (the vendored accumulator is
MPL-2.0). Third-party dependencies keep their respective licenses. The graph
construction, regions, portals and the session producer layer are not claimed
byte-equivalent to any prior runtime or production system.
