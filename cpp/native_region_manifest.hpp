#pragma once

#include "native_journal.hpp"
#include "native_region_binding_view.hpp"
#include "native_region_topology_catalog.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace swegca::vrs {

// One durable, derived region publication. It carries physical binding-page
// addresses for one exact Main pair and no independent evidence or authority.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
struct NativeRegionManifestCursor {
    NativeRegionBindingState bindings;
    std::string memory_snapshot_id;
    std::string pair_snapshot_id;
    std::int64_t published_source_rows = 0;
    std::uint64_t manifest_rows = 0;
};

// Publish already-synced binding pages and catalog entries under the same
// Main owner as the canonical source journal. A Graph change resets the page
// map and therefore requires one address for every logical page. Only a
// same-Graph derived replacement may copy unchanged page addresses.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
[[nodiscard]] NativeRegionManifestCursor append_region_manifest_row(
    NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    const NativeRegionManifestCursor& current,
    std::string graph_snapshot_id,
    std::uint64_t node_count,
    std::string memory_snapshot_id,
    std::string pair_snapshot_id,
    std::int64_t published_source_rows,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> page_updates,
    const NativeRegionBindingPageFile& binding_file,
    const NativeRegionTopologyCatalog& catalog);

// Replay the checksummed manifest chain and cold-validate every binding page
// and component-root topology named by its final active row. Historical
// derived topology files grant no authority and need not remain active.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
[[nodiscard]] NativeRegionManifestCursor recover_region_manifest(
    const NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    const NativeRegionBindingPageFile& binding_file,
    const NativeRegionTopologyCatalog& catalog);

}  // namespace swegca::vrs
