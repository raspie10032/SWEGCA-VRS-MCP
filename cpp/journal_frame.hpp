#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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

struct PendingJournalRow {
    std::string request_id;
    std::string body;
    std::string fingerprint;
    std::string pair_id;
};

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
[[nodiscard]] std::vector<std::byte> encode_journal_frame(std::span<const JournalRow> rows);

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-234
[[nodiscard]] std::vector<std::byte> encode_journal_frame(
    std::int64_t first_sequence, std::span<const PendingJournalRow> rows);

// Validate the complete frame before delivering rows. Rows are then streamed
// from a second inflation pass, so a batch does not require one large JSON tree.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-177
void visit_journal_frame(std::span<const std::byte> frame,
                         const std::function<void(JournalRow&&)>& visit);

// A complete frame is validated first. The second inflation pass can stop
// after the requested row, as the author's rows(after, upto) generator does.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:206-221
[[nodiscard]] bool visit_journal_frame_until(
    std::span<const std::byte> frame,
    const std::function<bool(JournalRow&&)>& visit);

}  // namespace swegca::vrs
