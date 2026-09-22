#pragma once

#include "journal_frame.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace swegca::vrs {

struct JournalScan {
    std::uint64_t row_count = 0;
    std::int64_t last_sequence = 0;
    std::string last_pair;
};

// The caller holds the single-owner lock before allowing tail repair.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
[[nodiscard]] JournalScan visit_journal_files(
    const std::filesystem::path& generation_directory,
    bool repair_head_tail,
    const std::function<void(JournalRow&&)>& visit);

}  // namespace swegca::vrs
