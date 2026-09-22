# VRS-MCP source inventory for the full SWEGCA rebuild

This is a static inventory of the old product checkout at `c06092a`. It is
not a test result, runtime proof, or permission to keep an old implementation.
Every one of the 65 Python product files is accounted for below. The new
implementation must replace the role, not merely preserve the filename.

| Current role | Product files | Count |
| --- | --- | ---: |
| Package boundary | `__init__.py` | 1 |
| Host ingress and session lifecycle | `codex_hooks.py`, `conversation_finalize.py`, `conversation_hooks.py`, `conversation_merge.py`, `conversation_watch.py`, `session_capture.py` | 6 |
| MCP transport and exposed tools | `layered.py`, `loopback.py`, `native_context.py`, `native_memory.py`, `native_transport.py`, `server.py` | 6 |
| Main owner, original storage, derived read paths and shards | `compact_index.py`, `csr_cache.py`, `cue_shards.py`, `exact_replay.py`, `linked_shards.py`, `native_journal.py`, `native_lock.py`, `projected_recall.py`, `read_lease.py`, `read_projection.py`, `resident.py`, `sharded.py`, `store.py` | 13 |
| VRS numerical state and refinement | `fast_regions.py`, `flat_vrs.py`, `vrs_evidence.py`, `vrs_refine.py` | 4 |
| Source provenance | `producer_identity.py`, `provenance.py` | 2 |
| Engine package boundary | `engine/__init__.py` | 1 |
| SWEGCA memory activation and evidence | `engine/mosaic_evidence_accumulator.py`, `engine/mosaic_memory_activation.py`, `engine/mosaic_memory_promotion.py`, `engine/mosaic_proposition_directory.py`, `engine/mosaic_semantic_family_directory.py` | 5 |
| Physical resident representation | `engine/mosaic_compressed_memory.py`, `engine/mosaic_hot_evidence_pages.py`, `engine/mosaic_immutable_numeric.py`, `engine/mosaic_lossless_blocks.py`, `engine/mosaic_lossless_float_tuple.py`, `engine/mosaic_packed_memberships.py`, `engine/mosaic_resident_directory_archive.py`, `engine/mosaic_resident_framed_header.py`, `engine/mosaic_resident_header_stream.py`, `engine/mosaic_resident_leaf_archive.py`, `engine/mosaic_resident_observation_tree.py`, `engine/mosaic_snapshot_digest.py` | 12 |
| VRS event, region, membership and portal architecture | `engine/mosaic_vrs_address_index.py`, `engine/mosaic_vrs_coactivation.py`, `engine/mosaic_vrs_coactivation_navigation.py`, `engine/mosaic_vrs_connectivity_regions.py`, `engine/mosaic_vrs_dependency_index.py`, `engine/mosaic_vrs_event_delta.py`, `engine/mosaic_vrs_event_kernel.py`, `engine/mosaic_vrs_event_signal.py`, `engine/mosaic_vrs_local_navigation.py`, `engine/mosaic_vrs_membership_cache.py`, `engine/mosaic_vrs_portal_activation.py`, `engine/mosaic_vrs_portal_lifecycle.py`, `engine/mosaic_vrs_region_arrays.py`, `engine/mosaic_vrs_region_publication.py`, `engine/mosaic_vrs_state_update.py` | 15 |
| **Total** | | **65** |

`pyproject.toml` declares five console script names, of which two point to
`server:main`. The four distinct Python entry modules are `server`, `layered`,
`conversation_hooks`, and `codex_hooks`. Following their first-party static
imports reaches 57/65 files. The other eight are the root `__init__` plus
seven engine modules: `mosaic_packed_memberships`, `mosaic_vrs_coactivation`,
`mosaic_vrs_coactivation_navigation`, `mosaic_vrs_local_navigation`,
`mosaic_vrs_membership_cache`, `mosaic_vrs_portal_activation`, and
`mosaic_vrs_portal_lifecycle`. This is a source reachability result only;
dynamic imports and direct external imports are separate checks. The main
point is that those author architecture paths cannot be claimed as active
just because their files exist.

Three static import sites have no source target: two in
`engine/mosaic_compressed_memory.py` for
`mosaic_paper_vrs_generation_rebind`, and one in
`engine/mosaic_vrs_membership_cache.py` for
`mosaic_vrs_nodeset_membership_cache`. The first is a generation-bound
compression dependency; the second is the packed node membership strategy.
Neither may be silently dropped or replaced with a different mechanism in
the full rebuild.

The broader SWEGCA architecture in the public `SWEGCA-Architecture` source
also defines main-owned cognitive state, producer proposals, evidence
accumulation, arbitration, bounded writes, and receipt-bound promotion. The
VRS-MCP rebuild must explicitly map those authority boundaries to its
memory-facing API. It must not imply that an MCP read or transcript admission
is a cognitive-state write or semantic promotion.

Source lineage for comparison: `SWEGCA-Architecture` revision
`5901a5aa2dcbd0ac7ad12ac6dd745699f72288a8` and the old VRS-MCP repair
checkout revision `c06092af7f6d050fc41a950be637b8e4cea584bc`.

## Product-adjacent surfaces that must switch with the runtime

The earlier `docs/VRS2_2_WHOLE_SOURCE_AUDIT_20260922.md` describes a 47-file
product tree and 21 standalone test files. The current tracked tree has 65
product Python files and 23 standalone test files. Its historical scope is
therefore not a complete inventory for this rebuild.

| Surface | Current tracked files or declaration | Rebuild obligation |
| --- | --- | --- |
| Distribution and five command names | `pyproject.toml`, `MANIFEST.in`, `uv.lock`; `swegca-vrs-mcp`, `swegca-vrs2-mcp`, `swegca-vrs2-codex`, `swegca-vrs2-hook`, `swegca-vrs2-codex-hooks` | Replace Python package execution and all command routes with the new source-built C++ product; no prebuilt wheel route. The first two names currently share `server:main`. |
| Host configuration and user setup | `README.md`, `docs/WINDOWS.md`, `examples/claude-vrs2.json`, `examples/claude-vrs2-windows.json` | Update commands and state paths to the rebuilt runtime. Both example JSON files currently name Python console scripts. |
| Runtime verification and smoke probes | `tools/verify_standalone.py`, `tools/smoke_mcp.py`, `tools/smoke_codex_mcp.py`, `tools/audit_vrs22_session_native_capture.py` | Verify new source, actual transport, session ingress and exact original lineage rather than accepting the old package. |
| Benchmarks and performance evidence | `local/bench/*.py`, `local/bench/compaction/score.py`, `tools/benchmark_vrs22_exact_occupancy.py`, `tools/benchmark_vrs22_natural_replay.py`, `tools/profile_vrs22_natural_first.py` | Rebind inputs and timing boundaries to the new executable without treating old results as new-runtime evidence. |
| Model evaluation | `tools/run_vrs22_compaction_stress_v019.py`, `tools/vrs22_eval_live_supervisor_v019.py`, `tools/summarize_vrs22_compaction_stress_v019.py`, `evals/vrs22_context/test_v019_grader.py` and fixture/hidden tests | Do not execute before the rebuild is complete. The runner currently hardcodes a Python runtime, live state path, and a receipt whose filename identifies an old wheel; these are stale for C++ acceptance. |
| Native source extraction and guards | `tools/extract_vrs2_runtime.py`, `tools/vrs22_shell_guard_v019.c`, `NATIVE_VRS2_PORT.json`, `ARCHITECTURE_UPSTREAM.json`, `UPSTREAM.json`, `VRS_UPSTREAM.json` | Check lineage and guard semantics against the new complete implementation, without copying an old binary or treating the old manifest as completion proof. |

There are no tracked `.github` workflow files in this checkout. This is a
tracked-tree observation, not a claim about remote repository settings. The
read-only inventory above does not execute any evaluator or change any service.
