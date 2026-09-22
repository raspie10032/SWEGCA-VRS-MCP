#pragma once

#include "journal_frame.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

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

// The caller holds the single-owner lock and has scanned the current head.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
[[nodiscard]] std::vector<std::int64_t> append_journal_rows(
    const std::filesystem::path& generation_directory,
    JournalScan& current,
    std::span<const PendingJournalRow> rows);

}  // namespace swegca::vrs
