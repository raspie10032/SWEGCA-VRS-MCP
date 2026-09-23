#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace swegca::vrs {

// The author snapshot digest uses SHA-256 over exact canonical UTF-8 bytes.
// This small source implementation keeps the hash boundary portable.
class Sha256 {
public:
    // SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:76-80
    Sha256();
    // SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:76-80
    void update(std::span<const std::byte> bytes);
    // SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:76-80
    void update(std::string_view bytes);
    // SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:76-80
    [[nodiscard]] std::array<std::byte, 32> finish();

private:
    std::array<std::uint32_t, 8> state_;
    std::array<std::byte, 64> pending_{};
    std::size_t pending_count_ = 0;
    std::uint64_t byte_count_ = 0;
    bool finished_ = false;
    // SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:76-80
    void compress(const std::array<std::byte, 64>& block);
};

// SWEGCA: src/swegca_vrs2/store.py@7536139:57-58
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

}  // namespace swegca::vrs
