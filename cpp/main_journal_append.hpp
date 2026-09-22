#pragma once

#include "main_observation_batch.hpp"
#include "native_journal.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace swegca::vrs {

struct MainJournalAppendResult {
    std::optional<JournalAppendResult> appended;
    bool recovered_existing_frame = false;
};

// The Main owner supplies its published pair and the journal head observed
// while building the detached batch. This records only original rows. It does
// not publish a memory, Graph, directory, or read generation.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1452
[[nodiscard]] MainJournalAppendResult append_main_observation_journal_rows(
    NativeJournal& journal,
    const MainObservationBatchPlan& plan,
    std::string_view published_pair_id,
    const std::optional<std::pair<std::int64_t, std::string>>& expected_head);

}  // namespace swegca::vrs
