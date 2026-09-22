#include "main_memory_projection_append.hpp"

#include "hot_index_projection.hpp"
#include "memory_episode.hpp"
#include "memory_vrs_pair.hpp"
#include "native_journal_entry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

struct ValidatedObservation {
    std::string identifier;
    HotIndexProjectionRow projection;
};

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1416-1452
std::vector<ValidatedObservation> validate_committed_batch(
    const NativeJournal& journal, const JournalAppendResult& committed,
    const MainObservationBatchPlan& plan, std::string_view published_parent_pair) {
    if (plan.parent_pair_id != published_parent_pair ||
        plan.pair_snapshot_id != full_current_pair_snapshot_id(
            plan.memory_snapshot_id, plan.graph_snapshot_id) ||
        plan.journal_rows.empty() ||
        committed.sequences.size() != plan.journal_rows.size() ||
        committed.frame.generation != journal.generation() ||
        committed.frame.first_sequence != committed.sequences.front() ||
        committed.frame.last_sequence != committed.sequences.back())
        throw std::runtime_error("main_projection_batch_changed");
    std::vector<ValidatedObservation> validated;
    validated.reserve(plan.journal_rows.size());
    std::size_t at = 0;
    journal.visit_frame_rows(committed.frame, [&](JournalRow&& actual) {
        if (at >= plan.journal_rows.size())
            throw std::runtime_error("main_projection_batch_changed");
        const auto sequence = committed.sequences[at];
        if (sequence < 1 || sequence != committed.frame.first_sequence +
            static_cast<std::int64_t>(at) ||
            sequence > static_cast<std::int64_t>(journal.row_count()))
            throw std::runtime_error("main_projection_batch_changed");
        const auto& expected = plan.journal_rows[at];
        if (actual.sequence != sequence ||
            actual.request_id != expected.request_id ||
            actual.body != expected.body ||
            actual.fingerprint != expected.fingerprint ||
            actual.pair_id != expected.pair_id ||
            actual.pair_id != plan.pair_snapshot_id)
            throw std::runtime_error("main_projection_batch_changed");
        const auto entry = parse_native_journal_entry(
            actual.request_id, actual.body, actual.fingerprint);
        if (entry.kind != NativeJournalEntryKind::observation)
            throw std::runtime_error("main_projection_batch_changed");
        auto episode = episode_from_observation(entry.value);
        auto identifier = episode.episode_id;
        validated.push_back(ValidatedObservation{
            identifier,
            HotIndexProjectionRow{sequence, plan.pair_snapshot_id,
                hot_index_header_from_episode(episode),
                postings_cues_from_observation(entry.value)}});
        ++at;
    });
    if (at != plan.journal_rows.size())
        throw std::runtime_error("main_projection_batch_changed");
    return validated;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:149-175
void require_predecessor(const HotIndexProjectionRow& projection,
                         const ExactJournalDirectory& originals,
                         const HotIndexProjectionLog& headers,
                         const NativeCueDirectory& successors,
                         std::int64_t sequence) {
    if (!projection.header.supersedes) return;
    const auto& previous = *projection.header.supersedes;
    const auto old = originals.find(previous, sequence - 1);
    const auto old_header = originals.find_header(previous, sequence - 1);
    if (!old || !old_header ||
        old_header->journal_sequence != old->sequence ||
        successors.posting_count(previous, sequence - 1) != 0)
        throw std::runtime_error("invalid_source_revision_successor");
    const auto prior = headers.read_at(*old_header, previous, sequence - 1);
    if (prior.header.source_addresses != projection.header.source_addresses ||
        prior.header.revision == projection.header.revision)
        throw std::runtime_error("invalid_source_revision_successor");
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
void append_original_postings(const HotIndexProjectionRow& projection,
                              NativeCueDirectory& cues,
                              NativeCueDirectory& propositions,
                              NativeCueDirectory& successors,
                              NativeCueDirectory& sources) {
    const auto sequence = projection.journal_sequence;
    const auto& identifier = projection.header.episode_id;
    cues.put(projection.posting_cues, identifier, sequence);
    if (projection.header.proposition_id) {
        const std::array<std::string, 1> one{*projection.header.proposition_id};
        propositions.put(one, identifier, sequence);
    }
    if (projection.header.supersedes) {
        const std::array<std::string, 1> one{*projection.header.supersedes};
        successors.put(one, identifier, sequence);
    }
    sources.put(projection.header.source_addresses, identifier, sequence);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1416-1452
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
MainMemoryProjectionAppendCount append_main_memory_projections(
    const NativeJournal& journal, const JournalAppendResult& committed,
    const MainObservationBatchPlan& plan, std::string_view published_parent_pair,
    ExactJournalDirectory& originals, HotIndexProjectionLog& headers,
    NativeCueDirectory& cues, NativeCueDirectory& propositions,
    NativeCueDirectory& successors, NativeCueDirectory& sources,
    NativeOperationDirectory& operations) {
    const auto& generation = journal.generation();
    if (originals.journal_generation() != generation ||
        headers.journal_generation() != generation ||
        cues.journal_generation() != generation ||
        propositions.journal_generation() != generation ||
        successors.journal_generation() != generation ||
        sources.journal_generation() != generation ||
        operations.journal_generation() != generation)
        throw std::runtime_error("main_projection_generation_changed");
    const auto validated = validate_committed_batch(
        journal, committed, plan, published_parent_pair);
    MainMemoryProjectionAppendCount count{};
    std::size_t added_at = 0;
    for (std::size_t at = 0; at < plan.journal_rows.size(); ++at) {
        const auto sequence = committed.sequences[at];
        const auto& row = plan.journal_rows[at];
        const auto& identifier = validated[at].identifier;
        const auto prior = originals.find(identifier, sequence - 1);
        if (prior) {
            ++count.duplicate_originals;
        } else {
            if (added_at >= plan.memory_additions.size() ||
                plan.memory_additions[added_at].identifier != identifier ||
                !plan.memory_additions[added_at].added())
                throw std::runtime_error("main_projection_plan_changed");
            const auto expected = project_hot_index_addition(
                plan.memory_additions[added_at], sequence,
                plan.pair_snapshot_id);
            if (encode_hot_index_projection(expected) !=
                encode_hot_index_projection(validated[at].projection))
                throw std::runtime_error("main_projection_plan_changed");
            require_predecessor(expected, originals, headers, successors,
                                sequence);
            const auto already = originals.find(identifier, sequence);
            if (already) {
                if (already->sequence != sequence)
                    throw std::runtime_error("main_projection_address_changed");
                const auto existing_header = originals.find_header(
                    identifier, sequence);
                if (!existing_header ||
                    encode_hot_index_projection(headers.read_at(
                        *existing_header, identifier, sequence)) !=
                    encode_hot_index_projection(expected))
                    throw std::runtime_error("main_projection_header_changed");
            } else {
                const auto header_address = headers.append(expected);
                originals.put(identifier,
                    OriginalJournalAddress{committed.frame, sequence},
                    header_address);
            }
            append_original_postings(expected, cues, propositions,
                                     successors, sources);
            ++added_at;
            ++count.new_originals;
        }
        operations.put(row.request_id,
            MainOperation{row.fingerprint, identifier, plan.pair_snapshot_id},
            sequence);
        ++count.journaled_observations;
    }
    if (added_at != plan.memory_additions.size())
        throw std::runtime_error("main_projection_plan_changed");
    return count;
}

}  // namespace swegca::vrs
