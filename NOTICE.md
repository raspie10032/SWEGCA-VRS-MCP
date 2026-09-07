# Source and license boundaries

- First-party MCP integration, persistence adapter, graph adapter and tests: MIT,
  copyright 2026 Dongjun Park. See LICENSE.
- `src/swegca_vrs_mcp/core/mosaic_*.py` and their original ported tests: MIT,
  derived from the public SWEGCA-VRS-PoC revision in UPSTREAM.json.
- `src/swegca_vrs_mcp/architecture/*.py` and `tests/architecture/*.py`: MPL-2.0,
  derived from public SWEGCA-Architecture. Preserve their file-level notices and
  LICENSES/MPL-2.0.txt; exact lineage appears in ARCHITECTURE_UPSTREAM.json.
- `core/vrs_refinement.py`: MIT first-party numerical extraction; only two generic
  numerical functions and their two constants were carried over. VRS_UPSTREAM.json
  records origin and function-level equivalence. No private runtime, history,
  experience, media or credentials are imported.

The combined source distribution is not MIT-only. Third-party dependencies keep
their respective licenses. The new graph construction and stopping adapter are
not claimed byte-equivalent to an entire prior runtime or production system.
