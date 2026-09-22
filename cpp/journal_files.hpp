#pragma once

#include "journal_frame.hpp"

#include <cstdint>
#include <cstddef>
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

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void write_atomic_file(const std::filesystem::path& path,
                       std::span<const std::byte> bytes);

// The caller holds the single-owner lock before allowing tail repair.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
[[nodiscard]] JournalScan visit_journal_files(
    const std::filesystem::path& generation_directory,
    bool repair_head_tail,
    const std::function<void(JournalRow&&)>& visit);

// Iterate in journal order, validating sequence until the visitor stops.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:179-221
void visit_journal_rows_until(
    const std::filesystem::path& generation_directory,
    const std::function<bool(JournalRow&&)>& visit);

// The caller holds the single-owner lock and has scanned the current head.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
[[nodiscard]] std::vector<std::int64_t> append_journal_rows(
    const std::filesystem::path& generation_directory,
    JournalScan& current,
    std::span<const PendingJournalRow> rows);

}  // namespace swegca::vrs
