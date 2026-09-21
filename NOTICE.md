# Source and license boundaries

The v2.2 runtime and package under `src/swegca_vrs2` are first-party MIT code.
`NATIVE_VRS2_PORT.json` records the exact first-party source lineage for the
ported engine definitions. The new native journal, main composition, session
capture, read directories, projections, and MCP integration are not claimed to
be byte-equivalent to an entire private Rozephine application. No private
experience, model weights, credentials, or runtime state are release inputs.

The source snapshots under `reference/upstream_vrs/architecture` are MPL-2.0
and originate from public SWEGCA-Architecture. Their exact paths and hashes are
recorded in `ARCHITECTURE_UPSTREAM.json`; preserve their file notices and
`LICENSES/MPL-2.0.txt`. They are lineage references only and are excluded from
the Python package and release archives.

The source snapshots under `reference/upstream_vrs/core` are MIT and derive
from the public SWEGCA-VRS-PoC lineage recorded by the upstream manifests. They
are also excluded from runtime and archives.

The combined source repository therefore carries MIT and MPL-2.0 material.
Third-party dependencies retain their own licenses.
