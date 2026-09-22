#pragma once

#include "native_graph_numeric_record.hpp"
#include "native_graph_page_map.hpp"
#include "owner_lock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace swegca::vrs {

enum class GraphPageKind { node, edge };

struct NativeGraphNodePage {
    std::uint32_t page_id = 0;
    std::uint32_t valid_records = 0;
    std::array<GraphNumericNodeRecord,
               NativeGraphPageMap::records_per_page> records{};
};

struct NativeGraphEdgePage {
    std::uint32_t page_id = 0;
    std::uint32_t valid_records = 0;
    std::array<GraphNumericEdgeRecord,
               NativeGraphPageMap::records_per_page> records{};
};

// Immutable numerical pages appended to one journal-generation file. Old
// readers keep their page-map root and therefore never follow a later edit
// to the same logical address. The caller syncs pages before publishing the
// map update and matching Main generation.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
class NativeGraphPageFile {
public:
    NativeGraphPageFile(std::filesystem::path path,
                        std::string journal_generation,
                        GraphPageKind kind,
                        OwnerLock* writer_lock = nullptr);

    // Each result is a physical offset for NativeGraphPageMap::with_updates.
    // These writes are unpublished until sync and the Main publication gate.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:138-169
    [[nodiscard]] std::uint64_t append(const NativeGraphNodePage& page);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:138-169
    [[nodiscard]] std::uint64_t append(const NativeGraphEdgePage& page);

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    void sync() const;

    // These reads validate the generation, page ID, record count, page CRC,
    // and each live record before any value reaches the VRS calculation.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] NativeGraphNodePage read_node(
        std::uint64_t offset, std::uint32_t expected_page_id) const;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] NativeGraphEdgePage read_edge(
        std::uint64_t offset, std::uint32_t expected_page_id) const;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }

private:
    [[nodiscard]] std::uint64_t append_encoded(
        std::uint32_t page_id, std::uint32_t count,
        const std::byte* payload, std::size_t payload_bytes);
    void read_encoded(std::uint64_t offset, std::uint32_t expected_page_id,
                      std::byte* payload, std::size_t payload_bytes,
                      std::uint32_t& count) const;
    void require_writer() const;
    [[nodiscard]] std::uint64_t page_bytes() const;

    std::filesystem::path path_;
    std::string journal_generation_;
    GraphPageKind kind_;
    OwnerLock* writer_lock_;
    mutable std::mutex writer_mutex_;
};

}  // namespace swegca::vrs
