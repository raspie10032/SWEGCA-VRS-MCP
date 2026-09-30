#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>

namespace swegca::world {

struct ProcessMemory final {
    double rss_mib{};
    double peak_rss_mib{};
    [[nodiscard]] JsonValue::Object to_dict() const;
};

[[nodiscard]] ProcessMemory retrieval_process_memory();

[[nodiscard]] JsonValue::Object profile_static_bm25(
    const std::filesystem::path& topics_path,
    const std::filesystem::path& corpus_path,
    std::size_t warmups = 3,
    std::size_t repeats = 50,
    double max_memory_mib = 2048.0,
    std::optional<std::filesystem::path> dynamic_report = std::nullopt);

inline constexpr std::string_view mosaic_static_bm25_profile_source_sha256 =
    "7b8b4720a3f7c4bfd9d537a8951556db2a1a7c4b4cdc8c3ac6a40e0ac2b97d83";

}  // namespace swegca::world
