#include "native_published_hot_index.hpp"

#include <array>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
void require_posting_publication(const NativeCueDirectory& directory,
                                 std::string_view generation,
                                 std::string_view pair_snapshot_id,
                                 std::int64_t row_limit) {
    const auto publication = directory.publication();
    if (!directory.published_reader() || !publication ||
        directory.journal_generation() != generation ||
        publication->journal_generation != generation ||
        publication->published_rows != row_limit ||
        publication->pair_snapshot_id != pair_snapshot_id)
        throw std::runtime_error("native_hot_index_publication_changed");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:122-175
// SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
NativePublishedHotIndex::NativePublishedHotIndex(
    HotIndexSeed memory, std::string pair_snapshot_id,
    std::int64_t published_row_limit,
    std::shared_ptr<const ExactJournalDirectory> originals,
    std::shared_ptr<const HotIndexProjectionLog> headers,
    std::shared_ptr<const NativeCueDirectory> cues,
    std::shared_ptr<const NativeCueDirectory> propositions,
    std::shared_ptr<const NativeCueDirectory> successors,
    std::shared_ptr<const NativeCueDirectory> sources,
    std::vector<std::shared_ptr<const RecordedSemanticFamilyRead>>
        semantic_families)
    // SWEGCA: src/swegca_vrs2/store.py@7536139:122-175
    : memory_(std::move(memory)),
      pair_snapshot_id_(std::move(pair_snapshot_id)),
      published_row_limit_(published_row_limit),
      originals_(std::move(originals)), headers_(std::move(headers)),
      cues_(std::move(cues)), propositions_(std::move(propositions)),
      successors_(std::move(successors)), sources_(std::move(sources)),
      semantic_families_(std::move(semantic_families)) {
    if (!originals_ || !headers_ || !cues_ || !propositions_ || !successors_ ||
        !sources_ ||
        memory_.snapshot_id.empty() || pair_snapshot_id_.empty() ||
        published_row_limit_ < 0)
        throw std::runtime_error("native_hot_index_generation_invalid");
    constexpr std::array<std::string_view, 6> outcomes{
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    if (memory_.outcome_counts.size() != outcomes.size())
        throw std::runtime_error("native_hot_index_outcomes_invalid");
    for (const auto outcome : outcomes)
        if (!memory_.outcome_counts.contains(std::string(outcome)))
            throw std::runtime_error("native_hot_index_outcomes_invalid");
    const auto original_publication = originals_->publication();
    if (!originals_->published_reader() || !original_publication ||
        original_publication->published_rows != published_row_limit_ ||
        original_publication->pair_snapshot_id != pair_snapshot_id_ ||
        original_publication->journal_generation !=
            originals_->journal_generation() ||
        headers_->journal_generation() != originals_->journal_generation())
        throw std::runtime_error("native_hot_index_publication_changed");
    const auto& generation = originals_->journal_generation();
    require_posting_publication(*cues_, generation, pair_snapshot_id_,
                                published_row_limit_);
    require_posting_publication(*propositions_, generation, pair_snapshot_id_,
                                published_row_limit_);
    require_posting_publication(*successors_, generation, pair_snapshot_id_,
                                published_row_limit_);
    require_posting_publication(*sources_, generation, pair_snapshot_id_,
                                published_row_limit_);
    const std::array<const NativeCueDirectory*, 4> directories{
        cues_.get(), propositions_.get(), successors_.get(), sources_.get()};
    for (std::size_t left = 0; left < directories.size(); ++left)
        for (std::size_t right = left + 1; right < directories.size(); ++right)
            if (std::filesystem::equivalent(directories[left]->directory(),
                                             directories[right]->directory()))
                throw std::runtime_error("native_hot_index_directory_alias");
    for (const auto& family : semantic_families_)
        if (!family)
            throw std::runtime_error("native_semantic_family_source_missing");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1299-1303
bool NativePublishedHotIndex::has_live_source(std::string_view source) const {
    return sources_->any_episode_id_for_cue(
        source, published_row_limit_, [&](std::string_view identifier) {
            return !successor_of(identifier).has_value();
        });
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:146-153
bool NativePublishedHotIndex::contains_episode(
    std::string_view identifier) const {
    return originals_->find(identifier, published_row_limit_).has_value();
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:129-135
// SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
HotIndexEpisodeHeader NativePublishedHotIndex::episode_header(
    std::string_view identifier) const {
    const auto address = originals_->find_header(identifier,
                                                  published_row_limit_);
    if (!address)
        throw std::out_of_range("'" + std::string(identifier) + "'");
    return headers_->read_at(*address, identifier, published_row_limit_).header;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:166-170
std::vector<std::string> NativePublishedHotIndex::proposition_ids(
    std::string_view proposition) const {
    return propositions_->episode_ids_for_cue(proposition, published_row_limit_);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:149-175
std::optional<std::string> NativePublishedHotIndex::successor_of(
    std::string_view identifier) const {
    auto children = successors_->episode_ids_for_cue(identifier,
                                                       published_row_limit_);
    if (children.size() > 1)
        throw std::runtime_error("native_hot_index_successor_conflict");
    if (children.empty()) return std::nullopt;
    return std::move(children.front());
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:172-174
std::string NativePublishedHotIndex::snapshot_id() const {
    return memory_.snapshot_id;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:174-174
std::uint64_t NativePublishedHotIndex::outcome_count(
    std::string_view outcome) const {
    return memory_.outcome_counts.at(std::string(outcome));
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:129-135
std::vector<std::string> NativePublishedHotIndex::episode_ids_for_cue(
    std::string_view cue) const {
    return cues_->episode_ids_for_cue(cue, published_row_limit_);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_semantic_family_directory.py@7536139:8-19
std::vector<std::string> NativePublishedHotIndex::semantic_family_parents(
    std::string_view identifier) const {
    std::set<std::string> parents;
    for (const auto& family : semantic_families_) {
        if (family->contains_parent(identifier))
            parents.emplace(identifier);
        for (auto& parent : family->parents_for_child(identifier))
            parents.insert(std::move(parent));
    }
    return {parents.begin(), parents.end()};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_semantic_family_directory.py@7536139:8-27
std::vector<std::string> NativePublishedHotIndex::semantic_family_members(
    std::string_view parent) const {
    std::vector<std::string> members;
    for (const auto& family : semantic_families_)
        for (auto& member : family->members_for_parent(parent))
            members.push_back(std::move(member));
    return members;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:254-259
std::uint64_t NativePublishedHotIndex::posting_count(std::string_view cue) const {
    return cues_->posting_count(cue, published_row_limit_);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:257-259
std::uint64_t NativePublishedHotIndex::exact_union_count(
    std::span<const std::string> cues) const {
    return cues_->exact_union_count(cues, published_row_limit_);
}

}  // namespace swegca::vrs
