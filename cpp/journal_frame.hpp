#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace swegca::vrs {

struct JournalRow {
    std::int64_t sequence;
    std::string request_id;
    std::string body;
    std::string fingerprint;
    std::string pair_id;
};

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
[[nodiscard]] std::vector<std::byte> encode_journal_frame(std::span<const JournalRow> rows);

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-177
[[nodiscard]] std::vector<JournalRow> decode_journal_frame(std::span<const std::byte> frame);

}  // namespace swegca::vrs
