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

// A derived address into one validated native generation. It identifies a
// frame, not a second copy of an experience or a semantic retrieval result.
struct JournalFrameAddress {
    std::string generation;
    std::string file_name;
    std::uint64_t byte_offset;
    std::int64_t first_sequence;
    std::int64_t last_sequence;
};

using JournalRowEmitter = std::function<void(JournalRow&&)>;
// A producer invokes its emitter synchronously and never retains it.
using JournalRowProducer = std::function<void(const JournalRowEmitter&)>;

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

// Rebuild derived frame addresses from the original journal. Each callback
// follows validation of its frame and all preceding sequence values. The
// caller publishes its derived index only after this entire walk succeeds.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
void visit_journal_frame_addresses(
    const std::filesystem::path& generation_directory,
    const std::function<void(const JournalFrameAddress&)>& visit);

// Stream verified originals and their exact frame addresses together. A
// caller may construct a derived ID directory without reopening every frame.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
void visit_journal_addressed_rows(
    const std::filesystem::path& generation_directory,
    const std::function<void(JournalRow&&, const JournalFrameAddress&)>& visit);

// Open only the addressed frame and return exactly one original row. Its
// generation, frame sequence span, checksum and original sequence are checked.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-177
[[nodiscard]] JournalRow read_journal_row_at(
    const std::filesystem::path& generation_directory,
    const JournalFrameAddress& address, std::int64_t sequence);

// The caller holds the single-owner lock and has scanned the current head.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
[[nodiscard]] std::vector<std::int64_t> append_journal_rows(
    const std::filesystem::path& generation_directory,
    JournalScan& current,
    std::span<const PendingJournalRow> rows);

// Write a complete replacement generation in bounded 512-row frames.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:295-312
void write_generation_head(
    const std::filesystem::path& generation_directory,
    const JournalRowProducer& produce_rows);

}  // namespace swegca::vrs
