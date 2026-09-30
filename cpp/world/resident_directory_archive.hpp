#pragma once

#include "world/compressed_memory.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view resident_directory_archive_source_sha256 =
    "b35d38ea3e04784e3871c3d7318b9f63461fe26c27787db5c9627b94ed42459b";
inline constexpr std::string_view native_resident_directory_magic = "RZCPPD01";

class ResidentDirectoryMemoryIndex final : public HotMemoryIndex {
public:
    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] bool contains_episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;

    const CompressionPolicy policy;
    const CompressedMemoryStats compression_stats;

private:
    friend std::shared_ptr<const ResidentDirectoryMemoryIndex> load_directory_leaf(
        std::span<const std::byte>, std::string_view, std::string_view, std::size_t);
    ResidentDirectoryMemoryIndex(std::shared_ptr<const std::vector<std::byte>> archive,
        std::string snapshot_id, CompressionPolicy policy,
        CompressedMemoryStats stats,
        std::map<std::string, std::size_t, std::less<>> episode_offsets,
        std::map<std::string, std::vector<std::string>, std::less<>> postings,
        std::map<std::string, std::size_t, std::less<>> outcome_counts);

    std::shared_ptr<const std::vector<std::byte>> archive_;
    std::string snapshot_id_;
    std::map<std::string, std::size_t, std::less<>> episode_offsets_;
    std::map<std::string, std::vector<std::string>, std::less<>> postings_;
    std::map<std::string, std::size_t, std::less<>> outcome_counts_;
    mutable std::mutex cache_mutex_;
    mutable std::map<std::string, std::shared_ptr<const MemoryEpisode>, std::less<>> cache_;
};

[[nodiscard]] std::vector<std::byte> dump_directory_leaf(
    const CompressedMemoryActivationIndex& source);
[[nodiscard]] std::shared_ptr<const ResidentDirectoryMemoryIndex> load_directory_leaf(
    std::span<const std::byte> archive,
    std::string_view expected_sha256,
    std::string_view expected_snapshot_id,
    std::size_t maximum_bytes);
[[nodiscard]] std::string resident_archive_sha256(std::span<const std::byte> archive);

}  // namespace swegca::world
