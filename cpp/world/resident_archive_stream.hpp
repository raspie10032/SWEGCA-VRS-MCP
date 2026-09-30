#pragma once

#include "world/resident_directory_archive.hpp"

#include <cstddef>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view resident_archive_stream_source_sha256 =
    "e276c3153f583f143fd68c0292d38c778b65598e66c33aa13cb51c8a71fbd7f3";

struct ResidentArchiveWriteReceipt final {
    std::size_t bytes{};
    std::string sha256;
};

[[nodiscard]] ResidentArchiveWriteReceipt write_resident_directory(
    const CompressedMemoryActivationIndex& source,
    std::ostream& stream, std::size_t maximum_bytes);
[[nodiscard]] std::shared_ptr<const ResidentDirectoryMemoryIndex> load_resident_stream(
    std::istream& stream, std::size_t expected_bytes,
    std::string_view expected_sha256,
    std::string_view expected_snapshot_id,
    std::size_t maximum_bytes);

}  // namespace swegca::world
