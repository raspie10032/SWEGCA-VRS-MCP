#pragma once

#include "world/resident_directory_archive.hpp"
#include "world/vrs_generation_rebind.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view sealed_generation_restore_source_sha256 =
    "1f41ba32f75fb177e27351830e31e5a69c127d53540010f5536ab0518590fc06";

struct ResidentLeafSeal final {
    std::string sha256;
    std::string snapshot_id;
    std::size_t bytes{};
};

[[nodiscard]] std::vector<std::shared_ptr<const HotMemoryIndex>>
restore_resident_ordinary_leaves(
    const std::vector<std::span<const std::byte>>& archives,
    const std::vector<ResidentLeafSeal>& seals,
    std::size_t maximum_archive_bytes);

[[nodiscard]] FullCurrentMemoryVrsSnapshot restore_sealed_final_generation(
    const FullCurrentMemoryVrsSnapshot& base,
    const std::vector<std::vector<MemoryEpisode>>& waves,
    std::shared_ptr<const VrsHotMemorySource> replacement,
    std::string vrs_report_sha256,
    std::vector<std::string> replaced_vrs_source_snapshot_ids,
    std::string_view expected_memory_snapshot_id,
    std::string_view expected_pair_snapshot_id);

[[nodiscard]] FullCurrentMemoryVrsSnapshot restore_resident_final_generation(
    const std::vector<std::span<const std::byte>>& archives,
    const std::vector<ResidentLeafSeal>& seals,
    std::size_t maximum_archive_bytes,
    std::shared_ptr<const VrsHotMemorySource> replacement,
    std::string vrs_report_sha256,
    std::vector<std::string> replaced_vrs_source_snapshot_ids,
    std::string_view expected_memory_snapshot_id,
    std::string_view expected_pair_snapshot_id);

}  // namespace swegca::world
