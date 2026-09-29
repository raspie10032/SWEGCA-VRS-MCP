#include "world/memory_activation.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"
#include "world/snapshot_digest.hpp"
#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace swegca::world {
namespace {

[[nodiscard]] bool text(const std::string_view value) {
    return !strip_unicode_whitespace(value).empty();
}

void require_text(const std::string_view value, const char* label) {
    if (!text(value)) throw std::invalid_argument(std::string(label) + " must not be empty");
}

[[nodiscard]] std::string cue(const std::string_view value) {
    require_text(value, "cue");
    return unicode_casefold(collapse_unicode_whitespace(value));
}

void append_unique(std::vector<std::string>& rows, std::string value) {
    if (std::ranges::find(rows, value) == rows.end()) rows.push_back(std::move(value));
}

[[nodiscard]] bool sha256_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i != digest.size(); ++i) {
        const auto value = std::to_integer<unsigned>(digest[i]);
        result[2 * i] = digits[value >> 4U];
        result[2 * i + 1] = digits[value & 15U];
    }
    return result;
}

[[nodiscard]] std::string digest(const std::string_view value) {
    architecture::Sha256 hash;
    hash.update(value);
    return hex(hash.finish());
}

void validate_required_outcomes(
    const std::vector<std::string>& required,
    const std::map<std::string, std::size_t, std::less<>>& counts) {
    for (const auto& outcome : required) {
        if (!memory_outcomes.contains(outcome))
            throw std::invalid_argument("unsupported required historical outcomes");
        const auto found = counts.find(outcome);
        if (found == counts.end() || found->second == 0)
            throw std::invalid_argument("missing required historical outcomes");
    }
}

}  // namespace

MemoryStep::MemoryStep(
    std::string phase_value, JsonValue::Object observation_value,
    std::vector<std::string> relations_value, std::string judgment_value,
    std::string outcome_value, std::vector<std::string> evidence_refs_value)
    : phase(std::move(phase_value)), observation(std::move(observation_value)),
      relations(std::move(relations_value)), judgment(std::move(judgment_value)),
      outcome(std::move(outcome_value)), evidence_refs(std::move(evidence_refs_value)) {
    require_text(phase, "memory phase");
    require_text(judgment, "memory judgment");
    if (!memory_outcomes.contains(outcome))
        throw std::invalid_argument("unsupported historical outcome");
    if (evidence_refs.empty() || std::ranges::any_of(evidence_refs, [](const auto& ref) {
        return !text(ref);
    })) throw std::invalid_argument("memory step requires evidence provenance");
}

MemoryEpisode::MemoryEpisode(
    std::string episode_id_value, std::vector<std::string> cues_value,
    std::vector<MemoryStep> steps_value, std::vector<std::string> source_addresses_value,
    std::string revision_value, std::string verification_state_value)
    : episode_id(std::move(episode_id_value)), steps(std::move(steps_value)),
      source_addresses(std::move(source_addresses_value)), revision(std::move(revision_value)),
      verification_state(std::move(verification_state_value)) {
    require_text(episode_id, "episode_id");
    for (const auto& value : cues_value) append_unique(cues, cue(value));
    if (cues.empty() || steps.empty() || source_addresses.empty())
        throw std::invalid_argument("memory episode is incomplete");
    auto unique_addresses = source_addresses;
    std::ranges::sort(unique_addresses);
    if (std::ranges::adjacent_find(unique_addresses) != unique_addresses.end())
        throw std::invalid_argument("memory source addresses must be unique");
    require_text(revision, "memory revision");
    require_text(verification_state, "verification state");
}

MemoryActivationIndex::MemoryActivationIndex(
    std::string snapshot_id_value,
    std::map<std::string, MemoryEpisode, std::less<>> episodes,
    std::map<std::string, std::vector<std::string>, std::less<>> postings,
    std::map<std::string, std::optional<bool>, std::less<>> coverage,
    const bool lookup_requires_io_value)
    : episodes_by_id(std::move(episodes)), postings_by_cue(std::move(postings)),
      proposition_directory([&] {
          std::vector<std::pair<std::string, std::vector<JsonValue>>> rows;
          for (const auto& [identifier, episode] : episodes_by_id) {
              std::vector<JsonValue> observations;
              for (const auto& step : episode.steps) observations.emplace_back(step.observation);
              rows.emplace_back(identifier, std::move(observations));
          }
          return PropositionDirectory::from_rows(rows);
      }()), snapshot_id_(std::move(snapshot_id_value)),
      lookup_requires_io_(lookup_requires_io_value) {
    if (!snapshot_id_.empty()) require_text(snapshot_id_, "snapshot_id");
    for (const auto& [identifier, episode] : episodes_by_id)
        if (identifier != episode.episode_id)
            throw std::invalid_argument("memory activation index episode identity changed");
    for (const auto& [key, identifiers] : postings_by_cue) {
        if (cue(key) != key || identifiers.empty() || !std::ranges::is_sorted(identifiers) ||
            std::ranges::adjacent_find(identifiers) != identifiers.end() ||
            std::ranges::any_of(identifiers, [&](const auto& identifier) {
                return !episodes_by_id.contains(identifier);
            })) throw std::invalid_argument("memory activation posting changed");
    }
    for (const auto& outcome : memory_outcomes) outcome_counts_.emplace(outcome, 0);
    for (const auto& [unused, episode] : episodes_by_id) {
        (void)unused;
        for (const auto& step : episode.steps) ++outcome_counts_.at(step.outcome);
    }
    for (const auto& [outcome, supplied] : coverage)
        if (supplied && *supplied != (outcome_counts_.at(outcome) != 0))
            throw std::invalid_argument("memory activation outcome coverage changed");
    if (lookup_requires_io_)
        throw std::invalid_argument("hot memory activation cannot perform I/O");
    const auto computed = snapshot_digest(episodes_by_id, postings_by_cue);
    if (!snapshot_id_.empty() && snapshot_id_ != computed)
        throw std::invalid_argument("memory activation snapshot content changed");
    if (snapshot_id_.empty()) snapshot_id_ = computed;
}

std::string_view MemoryActivationIndex::snapshot_id() const noexcept { return snapshot_id_; }
bool MemoryActivationIndex::lookup_requires_io() const noexcept { return lookup_requires_io_; }
std::size_t MemoryActivationIndex::episode_count() const noexcept { return episodes_by_id.size(); }
const std::map<std::string, std::size_t, std::less<>>&
MemoryActivationIndex::outcome_counts() const noexcept { return outcome_counts_; }

const MemoryEpisode& MemoryActivationIndex::episode(const std::string_view identifier) const {
    const auto found = episodes_by_id.find(identifier);
    if (found == episodes_by_id.end()) throw std::out_of_range("memory episode unavailable");
    return found->second;
}

std::vector<std::string> MemoryActivationIndex::episode_ids_for_cue(
    const std::string_view value) const {
    const auto found = postings_by_cue.find(cue(value));
    return found == postings_by_cue.end() ? std::vector<std::string>{} : found->second;
}

std::vector<std::string> MemoryActivationIndex::iter_episode_ids() const {
    std::vector<std::string> result;
    for (const auto& [identifier, unused] : episodes_by_id) {
        (void)unused; result.push_back(identifier);
    }
    return result;
}

CompositeMemoryActivationIndex::CompositeMemoryActivationIndex(
    std::vector<std::shared_ptr<const HotMemoryIndex>> source_values)
    : sources(std::move(source_values)) {
    if (sources.empty() || std::ranges::any_of(sources, [](const auto& source) {
        return !source || source->lookup_requires_io();
    })) throw std::invalid_argument("composite hot memory source changed");
    JsonValue::Array ids;
    for (const auto& source : sources) ids.emplace_back(source->snapshot_id());
    snapshot_id_ = digest(semantic_canonical_json(JsonValue::Object{
        {"schema_version", "rozephine-composite-hot-memory-v1"},
        {"source_snapshot_ids", JsonValue(std::move(ids))}}));
    for (const auto& outcome : memory_outcomes) outcome_counts_[outcome] = 0;
    for (const auto& source : sources) {
        for (const auto& outcome : memory_outcomes)
            outcome_counts_[outcome] += source->outcome_counts().at(outcome);
        if (const auto direct = std::dynamic_pointer_cast<const MemoryActivationIndex>(source)) {
            for (const auto& [identifier, unused] : direct->episodes_by_id) {
                (void)unused;
                if (!routed_.emplace(identifier, source).second)
                    throw std::invalid_argument("hot memory episode identity overlaps");
            }
            for (const auto& [key, identifiers] : direct->postings_by_cue) {
                auto& rows = postings_[key];
                for (const auto& identifier : identifiers) append_unique(rows, identifier);
                std::ranges::sort(rows);
            }
        } else unrouted_.push_back(source);
    }
}

std::string_view CompositeMemoryActivationIndex::snapshot_id() const noexcept { return snapshot_id_; }
std::size_t CompositeMemoryActivationIndex::episode_count() const noexcept {
    std::size_t result = 0; for (const auto& source : sources) result += source->episode_count();
    return result;
}
const std::map<std::string, std::size_t, std::less<>>&
CompositeMemoryActivationIndex::outcome_counts() const noexcept { return outcome_counts_; }

const MemoryEpisode& CompositeMemoryActivationIndex::episode(
    const std::string_view identifier) const {
    const MemoryEpisode* found = nullptr;
    if (const auto routed = routed_.find(identifier); routed != routed_.end())
        found = &routed->second->episode(identifier);
    for (const auto& source : unrouted_) {
        try {
            const auto& candidate = source->episode(identifier);
            if (found) throw std::invalid_argument("hot memory episode identity overlaps");
            found = &candidate;
        } catch (const std::out_of_range&) {}
    }
    if (!found) throw std::out_of_range("memory episode unavailable");
    return *found;
}

std::vector<std::string> CompositeMemoryActivationIndex::episode_ids_for_cue(
    const std::string_view value) const {
    std::vector<std::string> result;
    const auto normalized = cue(value);
    if (const auto found = postings_.find(normalized); found != postings_.end()) result = found->second;
    for (const auto& source : unrouted_)
        for (auto identifier : source->episode_ids_for_cue(normalized))
            append_unique(result, std::move(identifier));
    std::ranges::sort(result);
    return result;
}

std::vector<std::string> CompositeMemoryActivationIndex::iter_episode_ids() const {
    std::vector<std::string> result;
    for (const auto& source : sources)
        for (auto identifier : source->iter_episode_ids()) {
            if (std::ranges::find(result, identifier) != result.end())
                throw std::invalid_argument("hot memory episode identity overlaps");
            result.push_back(std::move(identifier));
        }
    return result;
}

std::vector<SemanticFamilyDirectory>
CompositeMemoryActivationIndex::semantic_family_directories() const {
    std::vector<SemanticFamilyDirectory> result;
    for (const auto& source : sources) {
        auto rows = source->semantic_family_directories();
        result.insert(result.end(), rows.begin(), rows.end());
    }
    return result;
}

std::shared_ptr<const MemoryActivationIndex> build_memory_activation_index(
    const std::vector<MemoryEpisode>& episodes,
    const std::vector<std::string>& required_outcomes) {
    std::map<std::string, MemoryEpisode, std::less<>> by_id;
    std::map<std::string, std::set<std::string>, std::less<>> sets;
    for (const auto& episode : episodes) {
        if (!by_id.emplace(episode.episode_id, episode).second)
            throw std::invalid_argument("memory episode IDs must be unique");
        for (const auto& key : episode.cues) sets[key].insert(episode.episode_id);
    }
    std::map<std::string, std::vector<std::string>, std::less<>> postings;
    for (const auto& [key, values] : sets)
        postings.emplace(key, std::vector<std::string>(values.begin(), values.end()));
    auto result = std::make_shared<const MemoryActivationIndex>(
        "", std::move(by_id), std::move(postings));
    validate_required_outcomes(required_outcomes, result->outcome_counts());
    return result;
}

std::shared_ptr<const HotMemoryIndex> append_memory_activation_index(
    std::shared_ptr<const HotMemoryIndex> base,
    const std::vector<MemoryEpisode>& episodes,
    const std::vector<std::string>& required_outcomes) {
    if (!base) throw std::invalid_argument("hot memory source required");
    std::shared_ptr<const HotMemoryIndex> replacement = base;
    if (!episodes.empty()) {
        for (const auto& episode : episodes) {
            try { (void)base->episode(episode.episode_id); }
            catch (const std::out_of_range&) { continue; }
            throw std::invalid_argument("memory episode IDs must be unique");
        }
        auto added = build_memory_activation_index(episodes);
        std::vector<std::shared_ptr<const HotMemoryIndex>> sources;
        if (const auto composite =
                std::dynamic_pointer_cast<const CompositeMemoryActivationIndex>(base))
            sources = composite->sources;
        else sources.push_back(base);
        sources.push_back(std::move(added));
        replacement = std::make_shared<const CompositeMemoryActivationIndex>(std::move(sources));
    }
    validate_required_outcomes(required_outcomes, replacement->outcome_counts());
    return replacement;
}

AtomicMemoryActivationOwner::AtomicMemoryActivationOwner(
    std::shared_ptr<const HotMemoryIndex> initial) : current_(std::move(initial)) {
    if (!current_) throw std::invalid_argument("hot memory source required");
}

std::shared_ptr<const HotMemoryIndex> AtomicMemoryActivationOwner::snapshot() const {
    std::lock_guard lock(mutex_); return current_;
}

std::string AtomicMemoryActivationOwner::replace(
    const std::string_view expected, std::shared_ptr<const HotMemoryIndex> replacement) {
    std::lock_guard lock(mutex_);
    if (current_->snapshot_id() != expected)
        throw std::invalid_argument("hot memory snapshot changed before replacement");
    if (!replacement) throw std::invalid_argument("hot memory source required");
    current_ = std::move(replacement);
    return std::string(current_->snapshot_id());
}

FullCurrentMemoryVrsSnapshot::FullCurrentMemoryVrsSnapshot(
    std::shared_ptr<const HotMemoryIndex> memory_value, std::string vrs_snapshot_id_value)
    : memory(std::move(memory_value)), vrs_snapshot_id(std::move(vrs_snapshot_id_value)) {
    if (!memory) throw std::invalid_argument("full-current memory must satisfy the hot-memory contract");
    if (!sha256_id(vrs_snapshot_id))
        throw std::invalid_argument("VRS snapshot ID must be a SHA-256 digest");
    snapshot_id = digest(semantic_canonical_json(JsonValue::Object{
        {"memory_snapshot_id", std::string(memory->snapshot_id())},
        {"schema_version", "rozephine-full-current-memory-vrs-snapshot-v1"},
        {"vrs_snapshot_id", vrs_snapshot_id}}));
}

AtomicFullCurrentMemoryVrsOwner::AtomicFullCurrentMemoryVrsOwner(
    FullCurrentMemoryVrsSnapshot initial) : current_(std::move(initial)) {}

FullCurrentMemoryVrsSnapshot AtomicFullCurrentMemoryVrsOwner::snapshot() const {
    std::lock_guard lock(mutex_); return current_;
}

std::string AtomicFullCurrentMemoryVrsOwner::replace(
    const std::string_view expected, FullCurrentMemoryVrsSnapshot replacement) {
    std::lock_guard lock(mutex_);
    if (current_.snapshot_id != expected)
        throw std::invalid_argument("full-current snapshot changed before replacement");
    current_ = std::move(replacement);
    return current_.snapshot_id;
}

DejaVuSignal::DejaVuSignal(
    std::string snapshot_id_value, std::string query_value,
    std::vector<std::string> current_cues_value,
    std::vector<std::string> matched_cues_value,
    const double recognition_strength_value, const std::size_t candidate_count_value,
    const bool memory_identifiers_exposed_value, const bool action_authorized_value)
    : snapshot_id(std::move(snapshot_id_value)), query(std::move(query_value)),
      current_cues(std::move(current_cues_value)), matched_cues(std::move(matched_cues_value)),
      recognition_strength(recognition_strength_value), candidate_count(candidate_count_value),
      memory_identifiers_exposed(memory_identifiers_exposed_value),
      action_authorized(action_authorized_value) {
    require_text(snapshot_id, "snapshot_id");
    require_text(query, "déjà vu query");
    if (recognition_strength < 0 || recognition_strength > 1)
        throw std::invalid_argument("déjà vu signal metrics changed");
    if (memory_identifiers_exposed || action_authorized)
        throw std::invalid_argument("déjà vu is only an anonymous retrieval trigger");
}

RuntimeCueSelection::RuntimeCueSelection(
    std::string snapshot_id_value, std::string query_value,
    std::map<std::string, std::size_t, std::less<>> candidate_counts_value,
    std::vector<std::string> selected_cues_value,
    std::vector<std::string> rejected_cues_value, std::string selection_method_value,
    const bool allowlist)
    : snapshot_id(std::move(snapshot_id_value)), query(std::move(query_value)),
      candidate_counts(std::move(candidate_counts_value)),
      selected_cues(std::move(selected_cues_value)), rejected_cues(std::move(rejected_cues_value)),
      selection_method(std::move(selection_method_value)),
      codex_or_evaluator_allowlist_used(allowlist) {
    require_text(snapshot_id, "runtime cue snapshot"); require_text(query, "runtime cue query");
    for (const auto& cue_name : selected_cues)
        if (!candidate_counts.contains(cue_name))
            throw std::invalid_argument("runtime cue selection references an unknown cue");
    for (const auto& cue_name : rejected_cues)
        if (!candidate_counts.contains(cue_name))
            throw std::invalid_argument("runtime cue selection references an unknown cue");
    if (selected_cues.size() > 1 || allowlist)
        throw std::invalid_argument("runtime cue selection authority changed");
}

RuntimeCueSelection select_runtime_cues(
    const HotMemoryIndex& index, std::string query,
    const std::vector<std::string>& candidate_cues,
    const std::vector<std::string>& preferred_current_evidence_cues) {
    std::vector<std::string> cues;
    for (const auto& value : candidate_cues) append_unique(cues, cue(value));
    std::vector<std::string> preferred;
    for (const auto& value : preferred_current_evidence_cues) append_unique(preferred, cue(value));
    for (const auto& value : preferred)
        if (std::ranges::find(cues, value) == cues.end())
            throw std::invalid_argument("preferred current-evidence cue is not a candidate cue");
    std::map<std::string, std::size_t, std::less<>> counts;
    for (const auto& value : cues) counts.emplace(value, index.episode_ids_for_cue(value).size());
    std::vector<std::string> selected;
    std::string method = "minimum_nonempty_hot_fanout_then_query_order";
    for (const auto& value : preferred) if (counts.at(value)) { selected.push_back(value); break; }
    if (!selected.empty()) method = "current_evidence_semantic_priority_then_hot_fanout";
    else {
        std::optional<std::tuple<std::size_t, std::size_t, std::string>> best;
        for (std::size_t i = 0; i != cues.size(); ++i)
            if (counts.at(cues[i]) && (!best || std::tuple{counts.at(cues[i]), i, cues[i]} < *best))
                best = std::tuple{counts.at(cues[i]), i, cues[i]};
        if (best) selected.push_back(std::get<2>(*best));
    }
    std::vector<std::string> rejected;
    for (const auto& value : cues)
        if (std::ranges::find(selected, value) == selected.end()) rejected.push_back(value);
    return {std::string(index.snapshot_id()), std::move(query), std::move(counts),
            std::move(selected), std::move(rejected), std::move(method)};
}

DejaVuSignal detect_deja_vu(
    const HotMemoryIndex& index, std::string query,
    const std::vector<std::string>& current_cues) {
    std::vector<std::string> cues;
    for (const auto& value : current_cues) append_unique(cues, cue(value));
    std::map<std::string, std::vector<std::string>, std::less<>> postings;
    std::vector<std::string> matched, identifiers;
    for (const auto& value : cues) {
        auto rows = index.episode_ids_for_cue(value);
        if (!rows.empty()) matched.push_back(value);
        for (auto identifier : rows) append_unique(identifiers, std::move(identifier));
        postings.emplace(value, std::move(rows));
    }
    const auto strength = static_cast<double>(matched.size()) /
        static_cast<double>(std::max<std::size_t>(1, cues.size()));
    return {std::string(index.snapshot_id()), std::move(query), std::move(cues),
            std::move(matched), strength, identifiers.size()};
}

RecallResult::RecallResult(
    std::string query_value, std::vector<RecallCandidate> candidates_value,
    std::string snapshot_id_value, const bool allowlist, const bool action,
    const bool persistent, std::vector<std::pair<std::string, std::string>> dependencies)
    : query(std::move(query_value)), candidates(std::move(candidates_value)),
      snapshot_id(std::move(snapshot_id_value)), codex_per_item_allowlist_used(allowlist),
      action_authorized(action), persistent_write_authorized(persistent),
      source_dependencies(std::move(dependencies)) {
    require_text(query, "recall query"); require_text(snapshot_id, "snapshot_id");
    if (allowlist || action || persistent) throw std::invalid_argument("recall authority changed");
}

RecallResult recall_memory(
    const HotMemoryIndex& index, const DejaVuSignal& signal,
    const std::vector<std::string>& navigation_cues) {
    if (signal.snapshot_id != index.snapshot_id())
        throw std::invalid_argument("déjà vu snapshot changed before recall");
    std::vector<std::string> navigation;
    for (const auto& value : navigation_cues) append_unique(navigation, cue(value));
    std::vector<std::string> keys = signal.matched_cues;
    for (const auto& value : navigation) append_unique(keys, value);
    std::vector<std::string> identifiers;
    for (const auto& value : keys)
        for (auto identifier : index.episode_ids_for_cue(value))
            append_unique(identifiers, std::move(identifier));
    const auto directories = index.semantic_family_directories();
    std::vector<std::string> families;
    for (const auto& identifier : identifiers)
        for (auto parent : family_keys(directories, identifier))
            append_unique(families, std::move(parent));
    std::vector<std::pair<std::string, std::string>> dependencies;
    for (const auto& parent : families)
        for (const auto& span : family_spans(directories, parent))
            for (const auto& identifier : span) {
                append_unique(identifiers, identifier);
                dependencies.emplace_back(identifier, parent);
            }
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    std::set<std::string, std::less<>> current(signal.current_cues.begin(), signal.current_cues.end());
    current.insert(navigation.begin(), navigation.end());
    std::vector<RecallCandidate> candidates;
    for (const auto& identifier : identifiers) {
        const auto& episode = index.episode(identifier);
        std::vector<std::string> matched;
        for (const auto& value : episode.cues) if (current.contains(value)) matched.push_back(value);
        std::set<std::string, std::less<>> unioned(episode.cues.begin(), episode.cues.end());
        unioned.insert(current.begin(), current.end());
        std::vector<std::string> outcomes;
        for (const auto& step : episode.steps) outcomes.push_back(step.outcome);
        candidates.push_back({identifier, std::move(matched), 0.0, episode.revision,
            episode.verification_state, std::move(outcomes)});
        candidates.back().cue_overlap = static_cast<double>(candidates.back().matched_cues.size()) /
            static_cast<double>(unioned.size());
    }
    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        if (left.cue_overlap != right.cue_overlap) return left.cue_overlap > right.cue_overlap;
        return left.episode_id < right.episode_id;
    });
    return {signal.query, std::move(candidates), std::string(index.snapshot_id()),
            false, false, false, std::move(dependencies)};
}

ReplayedEpisode::ReplayedEpisode(
    std::string episode_id_value, std::vector<std::string> matched_cues_value,
    std::vector<MemoryStep> steps_value, std::vector<std::string> source_addresses_value,
    std::string verification_state_value, const bool historical_truth)
    : episode_id(std::move(episode_id_value)), matched_cues(std::move(matched_cues_value)),
      steps(std::move(steps_value)), source_addresses(std::move(source_addresses_value)),
      verification_state(std::move(verification_state_value)),
      historical_truth_authorized(historical_truth) {
    require_text(verification_state, "replayed verification state");
    if (historical_truth) throw std::invalid_argument("replay is reconstruction, not historical truth");
}

ReplayResult::ReplayResult(std::string query_value,
    std::vector<ReplayedEpisode> episodes_value, const bool action, const bool persistent)
    : query(std::move(query_value)), episodes(std::move(episodes_value)),
      action_authorized(action), persistent_write_authorized(persistent) {
    if (action || persistent) throw std::invalid_argument("replay grants no authority");
}

ReplayResult replay_memory(const HotMemoryIndex& index, const RecallResult& recalled) {
    if (recalled.snapshot_id != index.snapshot_id())
        throw std::invalid_argument("recall snapshot changed before replay");
    std::vector<ReplayedEpisode> result;
    for (const auto& candidate : recalled.candidates) {
        const auto& source = index.episode(candidate.episode_id);
        result.emplace_back(candidate.episode_id, candidate.matched_cues, source.steps,
                            source.source_addresses, source.verification_state);
    }
    return {recalled.query, std::move(result)};
}

CurrentEvidenceVerdict::CurrentEvidenceVerdict(
    std::string episode_id_value, std::string proposition_value,
    std::string verdict_value, std::string rationale_value,
    std::vector<std::string> current_refs, std::vector<std::string> contradiction_refs_value)
    : episode_id(std::move(episode_id_value)), proposition(std::move(proposition_value)),
      verdict(std::move(verdict_value)), rationale(std::move(rationale_value)),
      current_evidence_refs(std::move(current_refs)),
      contradiction_refs(std::move(contradiction_refs_value)) {
    require_text(episode_id, "re-evidence episode_id"); require_text(proposition, "re-evidence proposition");
    if (!evidence_verdicts.contains(verdict))
        throw std::invalid_argument("unsupported re-evidence verdict");
    require_text(rationale, "re-evidence rationale");
    if ((verdict == "support" || verdict == "refute" || verdict == "available" ||
         verdict == "retained") && current_evidence_refs.empty())
        throw std::invalid_argument("current evidence required");
    if (verdict == "conflict" && current_evidence_refs.empty() && contradiction_refs.empty())
        throw std::invalid_argument("conflict requires current or contradiction evidence");
    if (std::ranges::any_of(current_evidence_refs, [](const auto& ref) { return !text(ref); }))
        throw std::invalid_argument("current evidence provenance changed");
    if (std::ranges::any_of(contradiction_refs, [](const auto& ref) { return !text(ref); }))
        throw std::invalid_argument("contradiction evidence provenance changed");
}

CurrentEvidenceVerdict current_experience_verdict(
    const ReplayedEpisode& episode, std::string memory_snapshot_id,
    std::string vrs_snapshot_id, const std::optional<double> current_strength,
    std::optional<std::string> proposition) {
    if (current_strength && (!std::isfinite(*current_strength) || *current_strength < 0))
        throw std::invalid_argument("invalid current VRS strength");
    require_text(memory_snapshot_id, "current memory snapshot");
    require_text(vrs_snapshot_id, "current VRS snapshot");
    const bool retained = current_strength && *current_strength >= 1.0;
    std::vector<std::string> refs{"memory-snapshot:" + memory_snapshot_id,
                                  "vrs-snapshot:" + vrs_snapshot_id};
    refs.insert(refs.end(), episode.source_addresses.begin(), episode.source_addresses.end());
    return {episode.episode_id, proposition.value_or("experience:" + episode.episode_id),
        retained ? "retained" : "available",
        retained ? "current VRS promotion retained; absence of fresh evidence is not refutation" :
                   "record available with original uncertainty; not promoted or fresh factual support",
        std::move(refs)};
}

ReEvidenceResult::ReEvidenceResult(
    std::string query_value, std::vector<CurrentEvidenceVerdict> judgments_value,
    std::vector<std::string> support, std::vector<std::string> refutation,
    std::vector<std::string> conflicts, const bool unresolved, const bool insufficient,
    const bool abstain, const bool action, const bool persistent, const bool promotion)
    : query(std::move(query_value)), judgments(std::move(judgments_value)),
      selected_support(std::move(support)), selected_refutation(std::move(refutation)),
      conflicting_propositions(std::move(conflicts)), unresolved_conflict(unresolved),
      insufficient_evidence(insufficient), should_abstain(abstain), action_authorized(action),
      persistent_write_authorized(persistent), semantic_promotion_authorized(promotion) {
    if (action || persistent || promotion)
        throw std::invalid_argument("re-evidence authority changed");
    if (should_abstain != (unresolved_conflict || insufficient_evidence))
        throw std::invalid_argument("conflict or insufficient re-evidence must abstain");
    std::set<std::string, std::less<>> ids;
    for (const auto& row : judgments)
        if (!ids.insert(row.episode_id).second)
            throw std::invalid_argument("re-evidence judged an episode more than once");
    std::vector<std::string> expected_support, expected_refutation;
    std::set<std::string, std::less<>> supported, refuted, expected_conflicts;
    bool usable = false;
    for (const auto& row : judgments) {
        if (row.verdict == "support") { expected_support.push_back(row.episode_id); supported.insert(row.proposition); }
        if (row.verdict == "refute") { expected_refutation.push_back(row.episode_id); refuted.insert(row.proposition); }
        if (row.verdict == "conflict") expected_conflicts.insert(row.proposition);
        usable = usable || row.verdict == "available" || row.verdict == "retained";
    }
    for (const auto& value : supported) if (refuted.contains(value)) expected_conflicts.insert(value);
    if (selected_support != expected_support)
        throw std::invalid_argument("re-evidence support selection changed");
    if (selected_refutation != expected_refutation)
        throw std::invalid_argument("re-evidence refutation selection changed");
    if (conflicting_propositions != std::vector<std::string>(expected_conflicts.begin(), expected_conflicts.end()))
        throw std::invalid_argument("re-evidence conflict selection changed");
    if (unresolved_conflict != !expected_conflicts.empty())
        throw std::invalid_argument("re-evidence conflict flag changed");
    const bool expected_insufficient = expected_conflicts.empty() && expected_support.empty() &&
        expected_refutation.empty() && !usable;
    if (insufficient_evidence != expected_insufficient)
        throw std::invalid_argument("re-evidence insufficient flag changed");
}

ReEvidenceResult re_evidence_memory(
    const ReplayResult& replayed, const EvidenceJudge& judge) {
    std::vector<CurrentEvidenceVerdict> judgments;
    for (const auto& episode : replayed.episodes) {
        auto judgment = judge(episode);
        if (judgment.episode_id != episode.episode_id)
            throw std::invalid_argument("re-evidence judgment episode identity changed");
        judgments.push_back(std::move(judgment));
    }
    std::vector<std::string> support, refutation;
    std::set<std::string, std::less<>> supported, refuted, conflicts;
    bool usable = false;
    for (const auto& row : judgments) {
        if (row.verdict == "support") { support.push_back(row.episode_id); supported.insert(row.proposition); }
        if (row.verdict == "refute") { refutation.push_back(row.episode_id); refuted.insert(row.proposition); }
        if (row.verdict == "conflict") conflicts.insert(row.proposition);
        usable = usable || row.verdict == "available" || row.verdict == "retained";
    }
    for (const auto& value : supported) if (refuted.contains(value)) conflicts.insert(value);
    const bool conflict = !conflicts.empty();
    const bool insufficient = !conflict && support.empty() && refutation.empty() && !usable;
    return {replayed.query, std::move(judgments), std::move(support), std::move(refutation),
            {conflicts.begin(), conflicts.end()}, conflict, insufficient, conflict || insufficient};
}

MemoryActivationReceipt::MemoryActivationReceipt(
    std::string schema, std::string snapshot, DejaVuSignal deja_vu_value,
    RecallResult recall_value, ReplayResult replay_value, ReEvidenceResult re_evidence_value,
    std::vector<std::string> stage_order_value, const bool action, const bool persistent)
    : schema_version(std::move(schema)), snapshot_id(std::move(snapshot)),
      deja_vu(std::move(deja_vu_value)), recall(std::move(recall_value)),
      replay(std::move(replay_value)), re_evidence(std::move(re_evidence_value)),
      stage_order(std::move(stage_order_value)), action_authorized(action),
      persistent_write_authorized(persistent) {
    if (schema_version != "rozephine-memory-activation-v1")
        throw std::invalid_argument("memory activation receipt schema changed");
    if (stage_order != std::vector<std::string>{"deja_vu", "recall", "replay", "re_evidence"})
        throw std::invalid_argument("memory activation stage order changed");
    if (snapshot_id != recall.snapshot_id)
        throw std::invalid_argument("memory activation snapshot changed between stages");
    if (snapshot_id != deja_vu.snapshot_id)
        throw std::invalid_argument("déjà vu snapshot changed between stages");
    if (deja_vu.query != recall.query || deja_vu.query != replay.query || deja_vu.query != re_evidence.query)
        throw std::invalid_argument("memory activation query changed between stages");
    if (recall.candidates.size() != replay.episodes.size() ||
        recall.candidates.size() != re_evidence.judgments.size())
        throw std::invalid_argument("memory activation Replay/Re-evidence cardinality changed");
    for (std::size_t i = 0; i != recall.candidates.size(); ++i) {
        const auto& candidate = recall.candidates[i]; const auto& played = replay.episodes[i];
        const auto& judgment = re_evidence.judgments[i];
        std::vector<std::string> outcomes;
        for (const auto& step : played.steps) outcomes.push_back(step.outcome);
        if (candidate.episode_id != played.episode_id || played.episode_id != judgment.episode_id ||
            candidate.matched_cues != played.matched_cues ||
            candidate.verification_state != played.verification_state ||
            candidate.historical_outcomes != outcomes)
            throw std::invalid_argument("memory activation Replay/Re-evidence source binding changed");
    }
    if (action || persistent)
        throw std::invalid_argument("memory activation receipt grants no authority");
}

MemoryActivationReceipt activate_memory(
    const HotMemoryIndex& index, std::string query,
    const std::vector<std::string>& current_cues, const EvidenceJudge& judge) {
    auto signal = detect_deja_vu(index, query, current_cues);
    auto recalled = recall_memory(index, signal);
    auto replayed = replay_memory(index, recalled);
    auto evidenced = re_evidence_memory(replayed, judge);
    return {"rozephine-memory-activation-v1", std::string(index.snapshot_id()),
            std::move(signal), std::move(recalled), std::move(replayed), std::move(evidenced)};
}

}  // namespace swegca::world
