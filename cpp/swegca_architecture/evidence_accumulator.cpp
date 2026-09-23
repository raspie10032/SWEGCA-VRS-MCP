#include "swegca_architecture/evidence_accumulator.hpp"

#include "swegca_architecture/cognitive_state.hpp"
#include "swegca_architecture/experience.hpp"
#include "swegca_architecture/journal_store.hpp"
#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace swegca::architecture {
namespace {

// The commit phase of admission moves the kept original; it must not throw.
static_assert(std::is_nothrow_move_constructible_v<AdmittedEvidence>);

// A group's count stays below 2^32, so a nonzero ratio exceeds 2^-32 and is
// an exact multiple of 2^-84 (its last significand bit is worth >= 2^-84).
constexpr std::uint64_t max_group_count = std::numeric_limits<std::uint32_t>::max();
constexpr int exact_scale = 84;
using ExactSum = unsigned __int128;

// Length-prefixed field so no two field sequences hash the same bytes.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:133-136
void hash_field(Sha256& hash, std::string_view text) {
    const auto length = static_cast<std::uint64_t>(text.size());
    std::array<std::byte, 8> prefix{};
    for (std::size_t at = 0; at < prefix.size(); ++at)
        prefix[at] = static_cast<std::byte>((length >> (8 * at)) & 0xff);
    hash.update(prefix);
    hash.update(text);
}

// Fixed-width little-endian integer field.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:137-140
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:81-180
void hash_generation(Sha256& hash, const StateGeneration& generation) {
    hash_u64(hash, generation.ordinal());
    hash.update(generation.digest().bytes());
}

// The author's group ratio, unchanged: part / total as an IEEE double
// (correctly rounded; both operands are exact below 2^53).
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:138-155
double group_ratio(std::uint64_t part, std::uint64_t total) noexcept {
    return total == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(total);
}

// The exact integer count of 2^-84 in a ratio in {0} or (2^-32, 1]. Scaling by
// a power of two is exact, and the scaled value is an integer below 2^85 whose
// two 64-bit halves are each exact in a double.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:158-170
ExactSum exact_multiple(double ratio) noexcept {
    const double scaled = std::ldexp(ratio, exact_scale);
    const auto high = static_cast<std::uint64_t>(std::ldexp(scaled, -64));
    const auto low = static_cast<std::uint64_t>(scaled - std::ldexp(static_cast<double>(high), 64));
    return (static_cast<ExactSum>(high) << 64) | low;
}

// Replaces one group's old ratio by its new ratio in an exact axis sum.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:158-170
ExactSum replace_ratio(ExactSum sum, double old_ratio, double new_ratio) {
    const auto rest = sum - exact_multiple(old_ratio);  // old ratio is part of sum
    const auto added = exact_multiple(new_ratio);
    if (added > ~ExactSum{0} - rest) throw std::overflow_error("evidence_axis_sum_exhausted");
    return rest + added;
}

// The exact sum rounded to double once (round to nearest), then unscaled
// exactly (a nonzero sum is at least 2^-32, far from the subnormal range).
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:158-170
double rounded_sum(ExactSum sum) noexcept {
    return std::ldexp(static_cast<double>(sum), -exact_scale);
}

// A single-element node for `like`'s container type, allocated from the same
// context, built in the prepare phase so the commit phase only splices it.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:359-428
template <class Container, class... Args>
typename Container::node_type detached(const Container& like, Args&&... args) {
    Container scratch(like.key_comp(), like.get_allocator());
    scratch.emplace(std::forward<Args>(args)...);
    return scratch.extract(scratch.begin());
}

// Capacity for one more element, grown geometrically so that n appends cost
// O(n) in total; reserve either succeeds or leaves the vector unchanged.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:359-428
template <class Vector>
void reserve_one_more(Vector& vector) {
    if (vector.size() < vector.capacity()) return;
    const auto limit = vector.max_size();
    if (vector.size() == limit) throw std::length_error("evidence_container_exhausted");
    const auto doubled = vector.capacity() > limit / 2 ? limit : vector.capacity() * 2;
    vector.reserve(std::max<std::size_t>(doubled, 8));
}

// Every admitted original exactly (address, content digest, observation
// generation, expiry, outcome, axis, source family, context, producer,
// observation step, producer confidence bits) in admission order, then every Re-evidence
// result (original, content digest, re-evidence generation, re-evidencer,
// outcome) in recording order, then the positions of exactly repeated results.
// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:81-180
template <class Positions>
Digest256 evidence_record_digest(std::span<const AdmittedEvidence> evidence,
                                 std::span<const ReEvidenceResult> results,
                                 const Positions& repeated) {
    Sha256 hash;
    hash_field(hash, "swegca.admitted_evidence.v3");
    hash_u64(hash, evidence.size());
    for (const auto& item : evidence) {
        hash_field(hash, item.address.value());
        hash.update(item.record_digest);
        hash_generation(hash, item.judged_against);
        hash_u64(hash, item.expires_at ? 1 : 0);
        hash_u64(hash, item.expires_at.value_or(0));
        hash_u64(hash, static_cast<std::uint64_t>(item.outcome));
        hash_u64(hash, item.axis);
        hash_field(hash, item.source_family.value());
        hash.update(item.context.bytes());
        hash_field(hash, item.producer.value());
        hash_u64(hash, item.observed_at);
        hash_u64(hash, std::bit_cast<std::uint64_t>(item.producer_confidence));
    }
    hash_u64(hash, results.size());
    for (const auto& result : results) {
        hash_field(hash, result.address().value());
        hash.update(result.record_digest());
        hash_generation(hash, result.generation());
        hash_field(hash, result.re_evidenced_by().value());
        hash_u64(hash, static_cast<std::uint64_t>(result.outcome()));
    }
    hash_u64(hash, repeated.size());
    for (const auto position : repeated) hash_u64(hash, position);
    return Digest256(hash.finish());
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:359-428
bool outcome_valid(EvidenceOutcome outcome) noexcept {
    return outcome == EvidenceOutcome::support || outcome == EvidenceOutcome::refute ||
           outcome == EvidenceOutcome::insufficient;
}

}  // namespace

// Bind over a decision's admitted set (already sorted and unique).
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
static Digest256 bind_admitted(const AllocationContext& memory, const ClaimRevision& claim,
                               std::span<const ExperienceAddress> admitted, const Digest256& delta_digest,
                        const Digest256& mask_digest) {
    EvidenceVector<std::string_view> addresses(memory.allocator<std::string_view>());
    addresses.reserve(admitted.size());
    for (const auto& address : admitted) addresses.push_back(address.value());
    return evidence_binding_digest(memory, claim.claim().value(), claim.revision(), addresses,
                                   delta_digest, mask_digest);
}

// Rule: Bind(D_t, addresses_i, delta_i, mask_i), ARCHITECTURE_SPEC.md@5901a5a:154-162.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
Digest256 evidence_binding_digest(const AllocationContext& memory, std::string_view claim,
                                  std::uint64_t claim_revision,
                                  std::span<const std::string_view> addresses,
                                  const Digest256& delta_digest,
                                  const Digest256& mask_digest) {
    EvidenceVector<std::string_view> sorted(memory.allocator<std::string_view>());
    sorted.assign(addresses.begin(), addresses.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    Sha256 hash;
    hash_field(hash, "swegca.evidence_binding.v2");
    hash_field(hash, claim);
    hash_field(hash, std::to_string(claim_revision));
    hash_field(hash, std::to_string(sorted.size()));
    for (const auto address : sorted) hash_field(hash, address);
    hash.update(delta_digest.bytes());
    hash.update(mask_digest.bytes());
    return Digest256(hash.finish());
}

// SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
ReEvidenceResult::ReEvidenceResult(ClaimRevision claim, ExperienceAddress address,
                                   DigestBytes record_digest, StateGeneration generation,
                                   ProducerId by, EvidenceOutcome outcome)
    : claim_(std::move(claim)), address_(std::move(address)), record_digest_(record_digest),
      generation_(std::move(generation)), by_(std::move(by)), outcome_(outcome) {}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:222-236
EvidenceAccumulator::EvidenceAccumulator(const AllocationContext& memory, ClaimRevision claim,
                                         const EvidencePolicy& policy)
    : memory_(memory), origin_(std::make_shared<const int>(0)), claim_(std::move(claim)),
      rules_(make_evidence_rules(policy)), rules_digest_(evidence_policy_digest(policy)),
      recent_window_(policy.recent_window) {
    axes_.reserve(policy.axis_count);
    for (std::uint32_t axis = 0; axis < policy.axis_count; ++axis)
        axes_.push_back(Axis{Map<GroupKey, Group>(memory.allocator<std::pair<const GroupKey, Group>>()),
                             Set<std::uint32_t>(memory.allocator<std::uint32_t>()),
                             Set<std::uint32_t>(memory.allocator<std::uint32_t>()), 0, 0});
    recent_.reserve(recent_window_);
}

// Id an identity text has, or would get if admitted now. Nothing is inserted:
// the table only grows in the commit phase.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:184-209
std::uint32_t EvidenceAccumulator::identity_id(const Map<Text, std::uint32_t>& table,
                                               std::string_view text) const {
    const auto found = table.find(text);
    if (found != table.end()) return found->second;
    if (table.size() >= std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("evidence_identity_space_exhausted");
    return static_cast<std::uint32_t>(table.size());
}

// Strong guarantee: every step that can allocate or throw runs first on
// detached nodes and reserved capacity; the commit phase only splices nodes,
// moves (noexcept) and updates integers, so a failure leaves the accumulator
// exactly as it was.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:359-428
AdmissionResult EvidenceAccumulator::admit(const EvidenceObservation& observation,
                                           const ExperienceRecord& replayed,
                                           std::span<const DigestBytes> root_families,
                                           std::span<const DigestBytes> root_contexts,
                                           const StateGeneration& current,
                                           std::uint64_t current_step) {
    if (observation.claim != claim_.claim().value() || observation.claim_revision != claim_.revision())
        throw std::invalid_argument("evidence_claim_mismatch");
    detail::require_identity_text(observation.address, ExperienceAddressTag::name);
    detail::require_identity_text(observation.source_family, SourceFamilyTag::name);
    detail::require_identity_text(observation.producer, ProducerIdTag::name);
    if (observation.axis >= axes_.size()) throw std::invalid_argument("evidence_axis_unknown");
    if (!outcome_valid(observation.outcome))
        throw std::invalid_argument("evidence_outcome_invalid");
    if (observation.expires_at && *observation.expires_at < observation.observed_at)
        throw std::invalid_argument("evidence_expiry_precedes_observation");
    if (!(std::isfinite(observation.producer_confidence) &&
          observation.producer_confidence >= 0 && observation.producer_confidence <= 1))
        throw std::invalid_argument("evidence_producer_confidence_invalid");
    // Spec :118 — the observation cites the record Main replayed, nothing else.
    if (replayed.record().address != observation.address)
        throw std::invalid_argument("evidence_record_address_mismatch");
    const auto family = Sha256::of(std::as_bytes(std::span(observation.source_family.data(),
                                                           observation.source_family.size())));
    const auto increasing = [](std::span<const DigestBytes> set) {
        return std::adjacent_find(set.begin(), set.end(), std::greater_equal<>()) == set.end();
    };
    if (!increasing(root_families) || !increasing(root_contexts) ||
        !std::binary_search(root_families.begin(), root_families.end(), family) ||
        !std::binary_search(root_contexts.begin(), root_contexts.end(), observation.context.bytes()))
        throw std::invalid_argument("evidence_correlation_invalid");

    if (originals_.contains(observation.address))
        return reject(observation.address, AdmissionResult::duplicate);
    // Author: every audit row's world hash is the current state's.
    if (observation.judged_against != current)
        return reject(observation.address, AdmissionResult::stale);
    if (observation.expires_at && current_step > *observation.expires_at)
        return reject(observation.address, AdmissionResult::expired);
    if (observation.outcome == EvidenceOutcome::insufficient)
        return reject(observation.address, AdmissionResult::insufficient);
    if (tally_.revision == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("evidence_revision_exhausted");

    // Prepare phase (may throw; nothing observable changes).
    const std::string_view source_text = observation.source_family;
    const std::string_view producer_text = observation.producer;
    const auto producer = identity_id(producer_ids_, producer_text);
    const bool support = observation.outcome == EvidenceOutcome::support;
    auto families = prepare_link(family_groups_, root_families);
    auto contexts = prepare_link(context_groups_, root_contexts);
    const auto source = families.root;
    const auto context = contexts.root;

    // When the evidence joins existing groups, the axes and the diversity
    // sets are rebuilt under the joined roots (one pass over the groups, as
    // the author's admission is) and swapped in at commit; otherwise they
    // are updated in place.
    const bool joining = !families.merged.empty() || !contexts.merged.empty();
    const auto alloc = memory_.allocator<char>();
    EvidenceVector<Axis> rebuilt(alloc);
    Set<std::uint32_t> rebuilt_sources(alloc);
    Set<std::uint32_t> rebuilt_contexts(alloc);
    if (joining) {
        rebuilt.reserve(axes_.size());
        for (const auto& old : axes_) {
            Axis joined{Map<GroupKey, Group>(alloc), Set<std::uint32_t>(alloc), old.producers, 0, 0};
            for (const auto& [key, group] : old.groups) {
                auto& into = joined.groups[GroupKey{families.remap(key.source), contexts.remap(key.context)}];
                into.supports += group.supports;
                into.refutes += group.refutes;
                if (into.supports + into.refutes >= max_group_count)
                    throw std::overflow_error("evidence_group_count_exhausted");
            }
            for (const auto kept : old.sources) joined.sources.insert(families.remap(kept));
            for (const auto& [key, group] : joined.groups) {
                const auto total = group.supports + group.refutes;
                joined.support = replace_ratio(joined.support, 0.0, group_ratio(group.supports, total));
                joined.refute = replace_ratio(joined.refute, 0.0, group_ratio(group.refutes, total));
            }
            rebuilt.push_back(std::move(joined));
        }
        for (const auto kept : sources_) rebuilt_sources.insert(families.remap(kept));
        for (const auto kept : contexts_) rebuilt_contexts.insert(contexts.remap(kept));
    }
    auto& axis = joining ? rebuilt[observation.axis] : axes_[observation.axis];
    auto& sources = joining ? rebuilt_sources : sources_;
    auto& context_set = joining ? rebuilt_contexts : contexts_;
    const GroupKey group_key{source, context};
    const auto group_found = axis.groups.find(group_key);
    const Group before = group_found == axis.groups.end() ? Group{} : group_found->second;
    Group after = before;
    (support ? after.supports : after.refutes) += 1;
    const auto total_before = before.supports + before.refutes;
    const auto total_after = after.supports + after.refutes;
    if (total_after >= max_group_count) throw std::overflow_error("evidence_group_count_exhausted");
    const auto support_sum =
        replace_ratio(axis.support, group_ratio(before.supports, total_before),
                      group_ratio(after.supports, total_after));
    const auto refute_sum =
        replace_ratio(axis.refute, group_ratio(before.refutes, total_before),
                      group_ratio(after.refutes, total_after));

    using TextIds = Map<Text, std::uint32_t>;
    TextIds::node_type producer_id_node;
    if (!producer_ids_.contains(producer_text))
        producer_id_node = detached(producer_ids_, Text(producer_text, alloc), producer);
    auto original_node = detached(originals_, Text(observation.address, alloc),
                                  admitted_evidence_.size());
    Map<GroupKey, Group>::node_type group_node;
    if (group_found == axis.groups.end()) group_node = detached(axis.groups, group_key, after);
    Set<std::uint32_t>::node_type axis_source_node;
    if (!axis.sources.contains(source)) axis_source_node = detached(axis.sources, source);
    Set<std::uint32_t>::node_type axis_producer_node;
    if (!axis.producers.contains(producer)) axis_producer_node = detached(axis.producers, producer);
    Set<std::uint32_t>::node_type source_node;
    if (!sources.contains(source)) source_node = detached(sources, source);
    Set<std::uint32_t>::node_type context_node;
    if (!context_set.contains(context)) context_node = detached(context_set, context);
    Set<std::uint32_t>::node_type producer_node;
    if (!producers_.contains(producer)) producer_node = detached(producers_, producer);
    const auto coverage_found = coverage_.find(observation.judged_against);
    Map<StateGeneration, Coverage>::node_type coverage_node;
    if (coverage_found == coverage_.end())
        coverage_node = detached(coverage_, observation.judged_against, Coverage{});
    reserve_one_more(admitted_evidence_);
    AdmittedEvidence kept{ExperienceAddress(memory_, observation.address),
                          replayed.record().record_digest,
                          observation.judged_against, observation.expires_at, observation.outcome,
                          observation.axis, SourceFamily(memory_, source_text),
                          observation.context, ProducerId(memory_, producer_text),
                          observation.observed_at,
                          observation.producer_confidence + 0.0};  // -0 is kept as +0

    // Commit phase: splices, noexcept moves and swaps, and integer updates
    // only (the group links' capacity was reserved when prepared). The
    // recent ring never reallocates: its capacity was reserved at construction.
    const auto splice = [](auto& container, auto& node) {
        if (!node.empty()) container.insert(std::move(node));
    };
    commit_link(family_groups_, families);
    commit_link(context_groups_, contexts);
    splice(producer_ids_, producer_id_node);
    splice(originals_, original_node);
    if (group_node.empty()) group_found->second = after;
    splice(axis.groups, group_node);
    splice(axis.sources, axis_source_node);
    splice(axis.producers, axis_producer_node);
    splice(sources, source_node);
    splice(context_set, context_node);
    splice(producers_, producer_node);
    splice(coverage_, coverage_node);
    coverage_.find(observation.judged_against)->second.observed += 1;
    admitted_evidence_.push_back(std::move(kept));
    if (observation.expires_at)
        earliest_expiry_ = std::min(earliest_expiry_.value_or(*observation.expires_at),
                                    *observation.expires_at);

    axis.support = support_sum;
    axis.refute = refute_sum;
    if (joining) {  // equal allocators (all from memory_): the swaps are noexcept
        axes_.swap(rebuilt);
        sources_.swap(rebuilt_sources);
        contexts_.swap(rebuilt_contexts);
    }
    for (std::size_t at = 0; at < axes_.size(); ++at) {
        const auto& each = axes_[at];
        tally_.axis_support[at] = rounded_sum(each.support);
        tally_.axis_refute[at] = rounded_sum(each.refute);
        tally_.axis_source_diversity[at] =
            static_cast<std::uint32_t>(std::min(each.sources.size(), each.producers.size()));
    }
    tally_.source_diversity =
        static_cast<std::uint32_t>(std::min(sources_.size(), producers_.size()));
    tally_.context_diversity =
        static_cast<std::uint32_t>(std::min(contexts_.size(), producers_.size()));
    if (recent_.size() < recent_window_) {
        recent_.push_back(support ? 1 : 0);
    } else {
        recent_[recent_next_] = support ? 1 : 0;
        recent_next_ = (recent_next_ + 1) % recent_window_;
    }
    std::uint32_t recent_supports = 0;
    for (const auto value : recent_) recent_supports += value;
    tally_.recent_count = static_cast<std::uint32_t>(recent_.size());
    tally_.recent_sum = recent_supports;
    tally_.revision += 1;
    return AdmissionResult::applied;
}

// Prepares linking `identities` (sorted, unique): the component every one
// of them ends in is the largest existing one among theirs (the earliest
// root on a tie), or the first new id when none exists; new identities get
// the next ids. Allocates the new nodes and reserves the parent and size
// capacity, so commit_link cannot fail; changes no link.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
EvidenceAccumulator::Link EvidenceAccumulator::prepare_link(Groups& groups,
                                                           std::span<const DigestBytes> identities) {
    using Node = Map<DigestBytes, std::uint32_t>::node_type;
    const auto alloc = groups.parent.get_allocator();
    Link link{0, EvidenceVector<std::uint32_t>(alloc), EvidenceVector<Node>(alloc)};
    EvidenceVector<std::uint32_t> roots(alloc);
    std::size_t fresh = 0;
    for (const auto& identity : identities) {
        const auto found = groups.ids.find(identity);
        if (found == groups.ids.end()) {
            ++fresh;
        } else {
            roots.push_back(groups.find(found->second));
        }
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    const auto next = groups.ids.size();
    if (fresh > std::numeric_limits<std::uint32_t>::max() - next)
        throw std::overflow_error("evidence_identity_space_exhausted");
    if (roots.empty()) {
        link.root = static_cast<std::uint32_t>(next);
    } else {
        link.root = *std::max_element(roots.begin(), roots.end(), [&](auto left, auto right) {
            return groups.size[left] != groups.size[right] ? groups.size[left] < groups.size[right]
                                                           : left > right;
        });
    }
    link.merged.reserve(roots.size());
    for (const auto root : roots)
        if (root != link.root) link.merged.push_back(root);  // stays sorted
    link.fresh.reserve(fresh);
    auto id = static_cast<std::uint32_t>(next);
    for (const auto& identity : identities)
        if (!groups.ids.contains(identity)) link.fresh.push_back(detached(groups.ids, identity, id++));
    groups.parent.reserve(next + fresh);
    groups.size.reserve(next + fresh);
    return link;
}

// Union by size: every new id and every absorbed root points at the root.
// Splices and pushes into reserved capacity only.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void EvidenceAccumulator::commit_link(Groups& groups, Link& link) noexcept {
    const auto root = link.root;
    for (auto& node : link.fresh) {
        const auto id = node.mapped();
        groups.ids.insert(std::move(node));
        groups.parent.push_back(id == root ? id : root);
        groups.size.push_back(1);
        if (id != root) groups.size[root] += 1;
    }
    for (const auto absorbed : link.merged) {
        groups.parent[absorbed] = root;
        groups.size[root] += groups.size[absorbed];
    }
}

// A rejected observation leaves the tally counts unchanged and is recorded;
// the single insertion either happens or throws with nothing changed. A new
// entry changes the rejected set a decision carries, so it advances the
// revision; an exact repeat of a kept rejection changes nothing.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:375-398
AdmissionResult EvidenceAccumulator::reject(std::string_view address, AdmissionResult reason) {
    if (tally_.revision == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("evidence_revision_exhausted");
    if (rejected_.emplace(Text(address, memory_.allocator<char>()), reason).second)
        tally_.revision += 1;
    return reason;
}

// Records one Re-evidence result for an admitted original. The tally counts
// are untouched (the original is counted once); the revision advances so an
// earlier decision is stale, and the coverage of the result's generation
// changes at most once per original for consistency and once for conflict.
// SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
AdmissionResult EvidenceAccumulator::record(ReEvidenceResult result) {
    if (result.claim() != claim_) throw std::invalid_argument("re_evidence_claim_mismatch");
    if (!outcome_valid(result.outcome())) throw std::invalid_argument("re_evidence_outcome_invalid");
    const auto original_found = originals_.find(std::string_view(result.address().value()));
    if (original_found == originals_.end())
        throw std::invalid_argument("re_evidence_original_not_admitted");
    const auto original = original_found->second;
    const auto& admitted = admitted_evidence_[original];
    if (admitted.record_digest != result.record_digest())
        throw std::invalid_argument("re_evidence_record_mismatch");

    // Prepare phase.
    const std::string_view by_text = result.re_evidenced_by().value();
    const auto by = identity_id(producer_ids_, by_text);
    if (tally_.revision == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("evidence_revision_exhausted");
    const ResultKey result_key{original, result.generation(), by, result.outcome()};
    if (const auto kept = results_.find(result_key); kept != results_.end()) {
        // An exact repeat: nothing new to count or to conflict, but the
        // refusal is kept, and a new refusal advances the revision.
        if (repeated_results_.insert(kept->second).second) tally_.revision += 1;
        return AdmissionResult::duplicate;
    }

    const CoverKey cover_key{original, result.generation()};
    const auto cover_found = covers_.find(cover_key);
    const Cover before = cover_found == covers_.end() ? Cover{} : cover_found->second;
    Cover after = before;
    (result.outcome() == admitted.outcome ? after.consistent : after.conflicted) = true;
    const bool observed_here = admitted.judged_against == result.generation();
    const bool newly_re_evidenced = !observed_here && after.consistent && !before.consistent;
    const bool newly_conflicted = after.conflicted && !before.conflicted;

    Map<Text, std::uint32_t>::node_type by_node;
    if (!producer_ids_.contains(by_text))
        by_node = detached(producer_ids_, Text(by_text, memory_.allocator<char>()), by);
    auto result_node = detached(results_, result_key, re_evidence_.size());
    Map<CoverKey, Cover>::node_type cover_node;
    if (cover_found == covers_.end()) cover_node = detached(covers_, cover_key, after);
    const auto coverage_found = coverage_.find(result.generation());
    Map<StateGeneration, Coverage>::node_type coverage_node;
    if (coverage_found == coverage_.end())
        coverage_node = detached(coverage_, result.generation(), Coverage{});
    reserve_one_more(re_evidence_);
    const auto generation = result.generation();

    // Commit phase.
    const auto splice = [](auto& container, auto& node) {
        if (!node.empty()) container.insert(std::move(node));
    };
    splice(producer_ids_, by_node);
    splice(results_, result_node);
    if (cover_node.empty()) cover_found->second = after;
    splice(covers_, cover_node);
    splice(coverage_, coverage_node);
    auto& coverage = coverage_.find(generation)->second;
    if (newly_re_evidenced) coverage.re_evidenced += 1;
    if (newly_conflicted) coverage.conflicted += 1;
    re_evidence_.push_back(std::move(result));
    tally_.revision += 1;
    return AdmissionResult::applied;
}

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:81-180
bool EvidenceAccumulator::evidence_current(const StateGeneration& generation,
                                           std::uint64_t current_step) const {
    if (earliest_expiry_ && current_step > *earliest_expiry_) return false;
    const auto found = coverage_.find(generation);
    const Coverage coverage = found == coverage_.end() ? Coverage{} : found->second;
    return coverage.conflicted == 0 &&
           coverage.observed + coverage.re_evidenced == admitted_evidence_.size();
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
EvidenceDecision::EvidenceDecision(const AllocationContext& memory, ClaimRevision claim,
    kernel::EvidenceJudgment judgment, EvidenceVector<ExperienceAddress> admitted,
    EvidenceVector<RejectedEvidence> rejected, Digest256 delta_digest, Digest256 mask_digest,
    Digest256 rules_digest, Digest256 evidence_digest, std::weak_ptr<const void> origin)
    : claim_(std::move(claim)), judgment_(judgment), admitted_(std::move(admitted)),
      rejected_(std::move(rejected)), delta_digest_(delta_digest), mask_digest_(mask_digest),
      rules_digest_(rules_digest), evidence_digest_(evidence_digest), origin_(std::move(origin)),
      binding_(bind_admitted(memory, claim_, admitted_, delta_digest_, mask_digest_)),
      decision_digest_(compute_decision_digest()) {}

// The visible decision digest covers every field above, so a receipt can
// name exactly which decision it relied on. Runs after every other member
// is initialized (declaration order).
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:252-259
Digest256 EvidenceDecision::compute_decision_digest() const {
    Sha256 hash;
    hash_field(hash, "swegca.evidence_decision.v2");
    hash_field(hash, claim_.claim().value());
    hash_u64(hash, claim_.revision());
    hash_u64(hash, judgment_.revision);
    hash_u64(hash, static_cast<std::uint64_t>(judgment_.status));
    hash_u64(hash, static_cast<std::uint64_t>(judgment_.reason));
    for (const double value : {judgment_.posterior_mean, judgment_.causal_lower_bound,
                               judgment_.overall_upper_bound, judgment_.effective_sample_size,
                               judgment_.regime_change_score})
        hash_u64(hash, std::bit_cast<std::uint64_t>(value));
    hash_u64(hash, judgment_.source_diversity);
    hash_u64(hash, judgment_.context_diversity);
    hash_u64(hash, admitted_.size());
    for (const auto& address : admitted_) hash_field(hash, address.value());
    hash_u64(hash, rejected_.size());
    for (const auto& item : rejected_) {
        hash_field(hash, item.address.value());
        hash_u64(hash, static_cast<std::uint64_t>(item.reason));
    }
    hash.update(delta_digest_.bytes());
    hash.update(mask_digest_.bytes());
    hash.update(rules_digest_.bytes());
    hash.update(evidence_digest_.bytes());
    hash.update(binding_.bytes());
    return Digest256(hash.finish());
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:285-357
EvidenceDecision EvidenceAccumulator::decide(const Digest256& delta_digest,
                                             const Digest256& mask_digest) const {
    EvidenceVector<ExperienceAddress> admitted(memory_.allocator<ExperienceAddress>());
    admitted.reserve(originals_.size());
    for (const auto& [address, index] : originals_)  // sorted by address
        admitted.emplace_back(memory_, std::string_view(address.data(), address.size()));
    EvidenceVector<RejectedEvidence> rejected(memory_.allocator<RejectedEvidence>());
    rejected.reserve(rejected_.size());
    for (const auto& [address, reason] : rejected_)
        rejected.push_back(
            RejectedEvidence{ExperienceAddress(memory_, std::string_view(address.data(), address.size())),
                             reason});
    return EvidenceDecision(memory_, claim_, kernel::judge_evidence(rules_, tally_),
                            std::move(admitted), std::move(rejected), delta_digest, mask_digest,
                            rules_digest_,
                            evidence_record_digest(admitted_evidence_, re_evidence_,
                                                   repeated_results_),
                            origin_);
}

// Owner identity, not address: a destroyed accumulator's identity is never
// reused while a decision still refers to it.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:28-45
bool EvidenceDecision::issued_by(const EvidenceAccumulator& accumulator) const noexcept {
    return accumulator.origin_ != nullptr && !origin_.owner_before(accumulator.origin_) &&
           !accumulator.origin_.owner_before(origin_);
}

// Replay first (the journal resolves the address through its published view
// and verifies the record, or throws), then Main judges the replayed record
// against the claim revision and the state it passes, then the result is
// bound to what was replayed and to that state's generation, and recorded.
// SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
ReEvidenceRecorded ReEvidence::apply(EvidenceAccumulator& accumulator,
                                     const ExperienceAddress& address,
                                     const CognitiveState& state, std::string_view by,
                                     ReEvidenceJudge judge) const {
    // One snapshot gives both the generation HEAD names and the original.
    auto at_head = journal_.replay_at_head(address);
    if (state.generation() != at_head.state)
        throw std::invalid_argument("re_evidence_state_not_current");
    const auto experience = ExperienceRecord::decode(std::move(at_head.record), memory_, journal_);
    experience.verify_parts();  // codex 16:32: fail closed before it counts
    const auto& view = experience.record();
    if (view.address != address.value())
        throw std::invalid_argument("re_evidence_replay_address_mismatch");
    const auto outcome = judge(experience, accumulator.claim(), state);
    if (!outcome_valid(outcome)) throw std::invalid_argument("re_evidence_outcome_invalid");
    ReEvidenceResult result(accumulator.claim(), address, view.record_digest, state.generation(),
                            ProducerId(memory_, by), outcome);
    const auto admission = accumulator.record(result);
    return ReEvidenceRecorded{std::move(result), admission};
}

// Replay and the generation HEAD names come from one snapshot of Main's
// journal, never from the caller; admission is judged against that pair.
// SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
AdmissionResult EvidenceAdmission::admit(EvidenceAccumulator& accumulator,
                                         const EvidenceObservation& observation,
                                         std::uint64_t current_step) {
    detail::require_identity_text(observation.address, ExperienceAddressTag::name);
    detail::require_identity_text(observation.source_family, SourceFamilyTag::name);
    auto at_head =
        journal_.replay_at_head(ExperienceAddress(accumulator.memory_, observation.address));
    const auto experience =
        ExperienceRecord::decode(std::move(at_head.record), accumulator.memory_, journal_);
    experience.verify_parts();  // codex 16:32: fail closed before it counts
    // Provenance is the experience's (COMPONENT_LEDGER.md@5901a5a:44-50), and
    // the evidence is linked to all of it: every root context (already
    // increasing, for_each_root_context checks) and every root family.
    const auto memory = accumulator.memory_;
    SourceFamilies::Digests contexts(memory.allocator<DigestBytes>());
    experience.for_each_root_context([&](const DigestBytes& context) {
        contexts.push_back(context);
        return true;
    });
    if (contexts.empty()) throw std::invalid_argument("evidence_context_unbound");
    if (!std::binary_search(contexts.begin(), contexts.end(), observation.context.bytes()))
        throw std::invalid_argument("evidence_provenance_mismatch:context");
    if (experience.observed_at() != observation.observed_at)
        throw std::invalid_argument("evidence_provenance_mismatch:observed_at");
    SourceFamilies::Digests families(memory.allocator<DigestBytes>());
    SourceFamilies::Fixes fixes(memory.allocator<SourceFamilies::Map::node_type>());
    if (!families_.root_families(experience, observation.source_family, families, fixes))
        throw std::invalid_argument("evidence_provenance_mismatch:source_family");
    const auto result =
        accumulator.admit(observation, experience, families, contexts, at_head.state, current_step);
    if (result == AdmissionResult::applied) families_.commit(fixes);
    return result;
}

// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void SourceFamilies::assign(std::string_view source, std::string_view family) {
    detail::require_identity_text(source, ProducerIdTag::name);
    detail::require_identity_text(family, SourceFamilyTag::name);
    const auto root = Sha256::of(std::as_bytes(std::span(source.data(), source.size())));
    const auto grouped = Sha256::of(std::as_bytes(std::span(family.data(), family.size())));
    const auto found = families_.find(root);
    if (found != families_.end()) {
        if (found->second != grouped) throw std::invalid_argument("source_family_reassigned");
        return;
    }
    families_.emplace(root, grouped);
}

// A grouped root's family is Main's; an ungrouped root's is its own name
// (digest: the root), and the entry fixing it is prepared before the
// accumulator's step. One pass over the roots.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
bool SourceFamilies::root_families(const ExperienceRecord& experience, std::string_view family,
                                   Digests& families, Fixes& fixes) const {
    experience.for_each_root_source([&](const DigestBytes& root) {
        const auto grouped = families_.find(root);
        if (grouped != families_.end()) {
            families.push_back(grouped->second);
        } else {
            families.push_back(root);
            Map staging(families_.get_allocator());
            staging.emplace(root, root);
            fixes.push_back(staging.extract(staging.begin()));
        }
        return true;
    });
    std::sort(families.begin(), families.end());
    families.erase(std::unique(families.begin(), families.end()), families.end());
    const auto named = Sha256::of(std::as_bytes(std::span(family.data(), family.size())));
    return std::binary_search(families.begin(), families.end(), named);
}

// Inserting prepared nodes allocates nothing and the key comparison
// cannot throw.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void SourceFamilies::commit(Fixes& fixes) noexcept {
    for (auto& fix : fixes) (void)families_.insert(std::move(fix));
}

}  // namespace swegca::architecture
