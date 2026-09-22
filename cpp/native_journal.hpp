#pragma once

#include "journal_files.hpp"
#include "owner_lock.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

// A journal has no independent experience authority. Its writable owner must
// hold the same OS lock as the main generation that applies its rows.
class NativeJournal {
public:
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
    NativeJournal(std::filesystem::path directory, bool create,
                  bool writable, OwnerLock* owner_lock = nullptr);

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:183-221
    void refresh_head();
    [[nodiscard]] std::optional<std::pair<std::int64_t, std::string>> head() const;
    [[nodiscard]] std::uint64_t row_count() const;
    void visit_rows(std::int64_t after, std::optional<std::int64_t> upto,
                    const std::function<void(JournalRow&&)>& visit) const;
    [[nodiscard]] std::optional<std::string> pair(std::int64_t sequence) const;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-247
    [[nodiscard]] std::vector<std::int64_t> append(std::span<const PendingJournalRow> rows);

    // The producer emits complete five-field rows in sequence order. It can
    // stream the old journal into the new one without holding all rows in RAM.
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:295-325
    void rewrite(const JournalRowProducer& produce_rows);

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::string& identity() const { return identity_; }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
    [[nodiscard]] const std::string& generation() const { return generation_; }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-95
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:123-127
    [[nodiscard]] const std::filesystem::path& generation_path() const { return path_; }

private:
    void require_write_owner() const;

    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::string identity_;
    std::string generation_;
    bool writable_ = false;
    OwnerLock* owner_lock_ = nullptr;
    JournalScan scan_;
};

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:70-81
[[nodiscard]] bool is_native_store(const std::filesystem::path& directory);

}  // namespace swegca::vrs
