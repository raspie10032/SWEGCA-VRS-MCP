#include "main_journal_append.hpp"

#include "memory_vrs_pair.hpp"
#include "native_journal_entry.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1397-1452
void validate_candidate(const MainObservationBatchPlan& plan,
                        std::string_view published_pair_id) {
    if (plan.parent_pair_id != published_pair_id ||
        plan.pair_snapshot_id != full_current_pair_snapshot_id(
            plan.memory_snapshot_id, plan.graph_snapshot_id))
        throw std::runtime_error("main_batch_parent_pair_changed");
    if (plan.journal_rows.empty() && plan.pair_snapshot_id != published_pair_id)
        throw std::runtime_error("main_batch_journal_row_invalid");
    std::set<std::string> request_ids;
    for (const auto& row : plan.journal_rows) {
        const auto entry = parse_native_journal_entry(
            row.request_id, row.body, row.fingerprint);
        if (row.pair_id != plan.pair_snapshot_id ||
            entry.kind != NativeJournalEntryKind::observation ||
            entry.value.canonical() != row.body ||
            !request_ids.insert(row.request_id).second)
            throw std::runtime_error("main_batch_journal_row_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
bool same_frame(const JournalFrameAddress& left,
                const JournalFrameAddress& right) {
    return left.generation == right.generation &&
           left.file_name == right.file_name &&
           left.byte_offset == right.byte_offset &&
           left.first_sequence == right.first_sequence &&
           left.last_sequence == right.last_sequence;
}

// A durable append can succeed and then report an error while rotating its
// head. On owner restart, compare the exact tail before retrying the batch.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1429-1452
JournalAppendResult recover_exact_tail(
    const NativeJournal& journal, std::int64_t old_sequence,
    const MainObservationBatchPlan& plan) {
    std::optional<JournalFrameAddress> frame;
    std::vector<std::int64_t> sequences;
    sequences.reserve(plan.journal_rows.size());
    journal.visit_addressed_rows(
        [&](JournalRow&& actual, const JournalFrameAddress& address) {
            if (actual.sequence <= old_sequence) return;
            const auto offset = actual.sequence - old_sequence - 1;
            if (offset < 0 || static_cast<std::uint64_t>(offset) >=
                                  plan.journal_rows.size())
                throw std::runtime_error("main_batch_journal_head_changed");
            const auto& expected = plan.journal_rows[
                static_cast<std::size_t>(offset)];
            if (actual.request_id != expected.request_id ||
                actual.body != expected.body ||
                actual.fingerprint != expected.fingerprint ||
                actual.pair_id != expected.pair_id ||
                (frame && !same_frame(*frame, address)))
                throw std::runtime_error("main_batch_journal_head_changed");
            if (!frame) frame = address;
            sequences.push_back(actual.sequence);
        });
    if (!frame || sequences.size() != plan.journal_rows.size() ||
        frame->first_sequence != old_sequence + 1 ||
        frame->last_sequence != sequences.back())
        throw std::runtime_error("main_batch_journal_head_changed");
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        if (sequences[index] != old_sequence +
                                    static_cast<std::int64_t>(index) + 1)
            throw std::runtime_error("main_batch_journal_head_changed");
    }
    return JournalAppendResult{std::move(sequences), std::move(*frame)};
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1452
MainJournalAppendResult append_main_observation_journal_rows(
    NativeJournal& journal, const MainObservationBatchPlan& plan,
    std::string_view published_pair_id,
    const std::optional<std::pair<std::int64_t, std::string>>& expected_head) {
    validate_candidate(plan, published_pair_id);
    const auto current_head = journal.head();
    if (current_head == expected_head) {
        if (plan.journal_rows.empty()) return {};
        return MainJournalAppendResult{
            journal.append_addressed(plan.journal_rows), false};
    }
    if (plan.journal_rows.empty())
        throw std::runtime_error("main_batch_journal_head_changed");
    const auto old_sequence = expected_head ? expected_head->first : 0;
    if (old_sequence < 0 ||
        plan.journal_rows.size() > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - old_sequence) ||
        !current_head ||
        current_head->first != old_sequence +
                                   static_cast<std::int64_t>(plan.journal_rows.size()) ||
        current_head->second != plan.pair_snapshot_id)
        throw std::runtime_error("main_batch_journal_head_changed");
    return MainJournalAppendResult{
        recover_exact_tail(journal, old_sequence, plan), true};
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1452
void require_committed_main_observation_frame(
    const NativeJournal& journal, const JournalAppendResult& committed,
    const MainObservationBatchPlan& plan,
    std::string_view published_parent_pair) {
    if (plan.journal_rows.empty() ||
        plan.parent_pair_id != published_parent_pair ||
        committed.sequences.size() != plan.journal_rows.size() ||
        committed.frame.generation != journal.generation() ||
        committed.frame.first_sequence != committed.sequences.front() ||
        committed.frame.last_sequence != committed.sequences.back() ||
        journal.row_count() <
            static_cast<std::uint64_t>(committed.frame.last_sequence) ||
        full_current_pair_snapshot_id(plan.memory_snapshot_id,
                                      plan.graph_snapshot_id) !=
            plan.pair_snapshot_id)
        throw std::runtime_error("main_batch_committed_frame_changed");
    std::size_t at = 0;
    journal.visit_frame_rows(committed.frame, [&](JournalRow&& actual) {
        if (at >= plan.journal_rows.size() ||
            actual.sequence != committed.sequences[at] ||
            actual.request_id != plan.journal_rows[at].request_id ||
            actual.body != plan.journal_rows[at].body ||
            actual.fingerprint != plan.journal_rows[at].fingerprint ||
            actual.pair_id != plan.journal_rows[at].pair_id ||
            actual.pair_id != plan.pair_snapshot_id ||
            parse_native_journal_entry(actual.request_id, actual.body,
                                       actual.fingerprint).kind !=
                NativeJournalEntryKind::observation)
            throw std::runtime_error("main_batch_committed_frame_changed");
        ++at;
    });
    if (at != plan.journal_rows.size())
        throw std::runtime_error("main_batch_committed_frame_changed");
}

}  // namespace swegca::vrs
