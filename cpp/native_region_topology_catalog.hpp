#pragma once

#include "native_region_topology_file.hpp"
#include "owner_lock.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace swegca::vrs {

struct NativeRegionTopologyCatalogRecord {
    std::uint32_t component = 0;
    std::string vrs_snapshot_id;
    std::string topology_id;
    std::string file_name;
};

// Append-only physical catalog addressed directly by a component root's
// binding record. Entries name immutable topology files; a manifest still
// chooses which binding-page root is visible to one Main generation.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
class NativeRegionTopologyCatalog {
public:
    NativeRegionTopologyCatalog(std::filesystem::path path,
                                std::filesystem::path topology_directory,
                                std::string journal_generation,
                                OwnerLock* writer_lock = nullptr);

    // The topology file must already be durable and cold-valid. The returned
    // nonzero offset can be stored only in the matching component root record.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
    [[nodiscard]] std::uint64_t append(
        const NativeRegionTopologyFile& topology);

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    void sync() const;

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
    [[nodiscard]] NativeRegionTopologyCatalogRecord read_record(
        std::uint64_t offset) const;

    // Opens the exact immutable file named by the checksummed catalog record.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
    [[nodiscard]] std::shared_ptr<const NativeRegionTopologyFile> open_topology(
        std::uint64_t offset, std::uint32_t expected_component) const;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::filesystem::path& topology_directory() const {
        return topology_directory_;
    }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }

private:
    void require_writer() const;

    std::filesystem::path path_;
    std::filesystem::path topology_directory_;
    std::string journal_generation_;
    OwnerLock* writer_lock_;
    mutable std::mutex mutex_;
};

}  // namespace swegca::vrs
