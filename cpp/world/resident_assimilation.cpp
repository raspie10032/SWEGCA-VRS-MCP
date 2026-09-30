#include "world/resident_assimilation.hpp"

#include "world/vrs_generation_rebind.hpp"
#include "world/vrs_memory_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string normalized_digest(std::string value, const char* label) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (!digest_id(value))
        throw std::invalid_argument(std::string(label) + " must be a SHA-256 digest");
    return value;
}

void require_nonempty(const std::vector<std::string>& values, const char* label) {
    if (values.empty() || std::ranges::any_of(values, [](const auto& value) {
            return value.empty();
        })) throw std::invalid_argument(std::string(label) + " must not be empty");
}

void require_unique(const std::vector<std::string>& values, const char* label) {
    std::set<std::string, std::less<>> unique(values.begin(), values.end());
    if (unique.size() != values.size())
        throw std::invalid_argument(std::string(label) + " must be unique");
}

std::set<const HotMemoryIndex*> ordinary_leaf_identities(
    const std::shared_ptr<const HotMemoryIndex>& index) {
    std::set<const HotMemoryIndex*> result;
    for (const auto& leaf : vrs_leaf_sources(index))
        if (!std::dynamic_pointer_cast<const VrsHotMemorySource>(leaf))
            result.insert(leaf.get());
    return result;
}

bool subset(const std::set<const HotMemoryIndex*>& expected,
            const std::set<const HotMemoryIndex*>& actual) {
    return std::ranges::all_of(expected, [&](const auto* value) {
        return actual.contains(value);
    });
}

}  // namespace

SealedOutcomeWave::SealedOutcomeWave(
    std::vector<MemoryEpisode> episodes_value, std::string sha256_value,
    const std::size_t bytes_value, std::vector<std::string> source_ids_value,
    std::vector<std::string> source_revisions_value,
    std::vector<std::string> source_families_value,
    std::function<void()> require_unchanged_value)
    : episodes(std::move(episodes_value)),
      sha256(normalized_digest(std::move(sha256_value), "sealed outcome wave")),
      bytes(bytes_value), source_ids(std::move(source_ids_value)),
      source_revisions(std::move(source_revisions_value)),
      source_families(std::move(source_families_value)),
      require_unchanged(std::move(require_unchanged_value)) {
    if (episodes.empty() || !bytes || !require_unchanged)
        throw std::invalid_argument("sealed outcome wave is incomplete");
    if (source_ids.size() != episodes.size() ||
        source_revisions.size() != episodes.size() ||
        source_families.size() != episodes.size())
        throw std::invalid_argument("outcome wave lineage count differs");
    require_nonempty(source_ids, "source IDs");
    require_nonempty(source_revisions, "source revisions");
    require_nonempty(source_families, "source families");
    require_unique(source_ids, "source IDs");
    require_unique(source_revisions, "source revisions");
    require_unchanged();
}

IncrementalResidentAssimilationController::IncrementalResidentAssimilationController(
    FullCurrentMemoryVrsSnapshot initial, const std::size_t maximum_rows_per_wave,
    ResidentGenerationPreparer prepare_generation,
    std::shared_ptr<DurableAssimilationMarker> durable_marker,
    std::shared_ptr<const void> initial_runtime, const std::size_t cold_bootstrap_count)
    : owner_(std::move(initial)), maximum_rows_(maximum_rows_per_wave),
      prepare_(std::move(prepare_generation)), durable_marker_(std::move(durable_marker)),
      runtime_(std::move(initial_runtime)) {
    const auto resident = owner_.snapshot();
    if (resident.memory->lookup_requires_io())
        throw std::invalid_argument("resident initial memory is not hot");
    if (cold_bootstrap_count != 1)
        throw std::invalid_argument("resident must have exactly one cold bootstrap");
    if (!maximum_rows_ || !prepare_ || !durable_marker_)
        throw std::invalid_argument("resident assimilation configuration is incomplete");
}

FullCurrentMemoryVrsSnapshot IncrementalResidentAssimilationController::snapshot() const {
    std::lock_guard lock(mutex_);
    return owner_.snapshot();
}

std::pair<FullCurrentMemoryVrsSnapshot, std::shared_ptr<const void>>
IncrementalResidentAssimilationController::read_generation() const {
    std::lock_guard lock(mutex_);
    return {owner_.snapshot(), runtime_};
}

ResidentAssimilationReceipt IncrementalResidentAssimilationController::assimilate(
    const std::string_view expected_pair_snapshot_id, const SealedOutcomeWave& wave) {
    const auto expected = normalized_digest(
        std::string(expected_pair_snapshot_id), "expected pair snapshot");
    std::lock_guard lock(mutex_);
    if (pending_marker_)
        throw std::invalid_argument("previous main commit marker requires explicit reconciliation");
    const auto base = owner_.snapshot();
    if (base.snapshot_id != expected)
        throw std::invalid_argument("resident pair changed before assimilation");
    if (wave.episodes.size() > maximum_rows_)
        throw std::invalid_argument("sealed outcome wave exceeds the row limit");
    wave.require_unchanged();

    const std::vector<std::string> required(memory_outcomes.begin(), memory_outcomes.end());
    auto staged = append_memory_activation_index(base.memory, wave.episodes, required);
    ++attempt_count_;
    auto prepared = prepare_(base, staged, wave, runtime_);
    const auto& replacement = prepared.pair;
    if (replacement.snapshot_id == base.snapshot_id ||
        replacement.vrs_snapshot_id == base.vrs_snapshot_id ||
        replacement.memory->lookup_requires_io() ||
        !subset(ordinary_leaf_identities(staged),
                ordinary_leaf_identities(replacement.memory)))
        throw std::invalid_argument("prepared resident generation broke the hot COW boundary");
    for (const auto& episode : wave.episodes) {
        if (!replacement.memory->contains_episode(episode.episode_id) ||
            replacement.memory->episode(episode.episode_id) != episode)
            throw std::invalid_argument("prepared generation lost a new outcome episode");
    }
    wave.require_unchanged();

    ResidentAssimilationReceipt receipt;
    receipt.previous_pair_snapshot_id = base.snapshot_id;
    receipt.replacement_pair_snapshot_id = replacement.snapshot_id;
    receipt.replacement_memory_snapshot_id = std::string(replacement.memory->snapshot_id());
    receipt.replacement_vrs_snapshot_id = replacement.vrs_snapshot_id;
    receipt.added_distinct_source_episode_count = wave.episodes.size();
    for (const auto& outcome : memory_outcomes) receipt.actual_outcome_counts.emplace(outcome, 0);
    for (const auto& episode : wave.episodes)
        if (!episode.steps.empty()) ++receipt.actual_outcome_counts.at(episode.steps.front().outcome);
    receipt.wave_sha256 = wave.sha256;
    receipt.wave_bytes = wave.bytes;
    receipt.lookup_requires_io = replacement.memory->lookup_requires_io();
    receipt.commit_count_this_resident = commit_count_ + 1;
    receipt.preparation = prepared.receipt;

    const auto pending = durable_marker_->prepare(receipt);
    if (pending.empty())
        throw std::invalid_argument("durable precommit marker must not be empty");
    pending_marker_ = pending;
    owner_.replace(base.snapshot_id, replacement);
    runtime_ = std::move(prepared.runtime);
    commit_count_ = receipt.commit_count_this_resident;
    receipt.durable_commit_marker = pending;
    try {
        durable_marker_->publish(pending);
        receipt.durable_commit_marker_ready = true;
        pending_marker_.reset();
    } catch (const std::exception& error) {
        receipt.durable_commit_marker_error = error.what();
    } catch (...) {
        receipt.durable_commit_marker_error = "durable marker publication failed";
    }
    return receipt;
}

void IncrementalResidentAssimilationController::reconcile_pending_marker(
    const std::string_view marker) {
    std::lock_guard lock(mutex_);
    if (!pending_marker_ || *pending_marker_ != marker)
        throw std::invalid_argument("pending main commit marker differs");
    pending_marker_.reset();
}

std::size_t IncrementalResidentAssimilationController::commit_count() const {
    std::lock_guard lock(mutex_);
    return commit_count_;
}

std::size_t IncrementalResidentAssimilationController::attempt_count() const {
    std::lock_guard lock(mutex_);
    return attempt_count_;
}

}  // namespace swegca::world
