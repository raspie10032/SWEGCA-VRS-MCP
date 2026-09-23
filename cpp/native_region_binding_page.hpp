#pragma once

#include "native_graph_page_map.hpp"
#include "owner_lock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace swegca::vrs {

// One physical node-to-region-directory address. Pending Graph nodes have an
// explicit absent record; they are not assigned to an old or guessed region.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
struct RegionBindingRecord {
    std::uint32_t component = 0;
    std::uint32_t local = 0;
    bool present = false;
};

inline constexpr std::size_t region_binding_record_bytes = 16;

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-575
[[nodiscard]] std::array<std::byte, region_binding_record_bytes>
encode_region_binding_record(RegionBindingRecord value);

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-575
[[nodiscard]] RegionBindingRecord decode_region_binding_record(
    const std::array<std::byte, region_binding_record_bytes>& bytes);

struct NativeRegionBindingPage {
    std::uint32_t page_id = 0;
    std::uint32_t valid_records = 0;
    std::array<RegionBindingRecord,
               NativeGraphPageMap::records_per_page> records{};
};

// Append-only immutable pages for the Graph's node -> component/local
// directory. A separately published page-map root chooses one page version
// for each logical page ID, so pinned readers do not follow later writes.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
class NativeRegionBindingPageFile {
public:
    NativeRegionBindingPageFile(std::filesystem::path path,
                                std::string journal_generation,
                                OwnerLock* writer_lock = nullptr);

    // Returns the physical offset to install into a successor page map.
    // The page remains unpublished until the caller syncs this file and
    // commits the region manifest under the Main owner.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:542-575
    [[nodiscard]] std::uint64_t append(
        const NativeRegionBindingPage& page);

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    void sync() const;

    // Validates file generation, logical page identity, checksums, padding,
    // and every live record before returning values.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:297-310
    [[nodiscard]] NativeRegionBindingPage read(
        std::uint64_t offset, std::uint32_t expected_page_id) const;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }

private:
    void require_writer() const;

    std::filesystem::path path_;
    std::string journal_generation_;
    OwnerLock* writer_lock_;
    mutable std::mutex mutex_;
};

}  // namespace swegca::vrs
