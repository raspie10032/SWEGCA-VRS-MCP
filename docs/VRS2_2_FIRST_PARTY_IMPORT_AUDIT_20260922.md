# First-party import closure audit (2026-09-22)

This is a source audit of the public repair checkout, not a live Codex MCP
installation or a performance result. Every Python file under
`src/swegca_vrs2` was parsed for relative imports. A target had to be a source
module or package in this checkout; a compiled cache did not count.

The public port omitted `mosaic_proposition_directory`, although
`MemoryActivationIndex.__post_init__` imports it. The directory was restored
from the author source at revision
`3bddcb7adc8c07e21a57d8c921d312aed83270e5`; its original SHA-256 is
`5e0db492f2309757a4fd66b4124cec6791fe7c67aa173bcbc220dd19fdb1d2c7`.
Only the directory and routing for public MCP index types were ported. Its
source and port hashes are recorded in `NATIVE_VRS2_PORT.json`.

Three first-party modules remain unresolved:

| Missing module | Public call site | Effect |
| --- | --- | --- |
| `mosaic_paper_vrs_generation_rebind` | `mosaic_compressed_memory.inherited_compression_policy`, `compress_hot_memory_index` | Generation-bound compression branches raise on import. |
| `mosaic_resident_directory_archive` | `mosaic_compressed_memory.resident_python_bytes` | The resident-size diagnostic raises on import. |
| `mosaic_vrs_nodeset_membership_cache` | `mosaic_vrs_membership_cache.configure_membership_cache(strategy='packed_nodes')` | Packed node membership configuration raises on import. |

The author-source relative-import closure of these three modules reaches 63
unported modules. That count is a dependency search result, not proof that all
63 are required in the public product. It is unsafe to claim a complete port
or copy the closure without tracing each public call path and its source
contract. `tools/verify_standalone.py` now fails on the four unresolved import
sites instead of reporting a false PASS.

The default session-first Recall route, the optional author engine/portal
activators, and the resident-size and packed-membership paths still need
separate end-to-end validation. None of these import checks proves the user's
input-to-first-Recall latency bound or the four-stage live hook route.
