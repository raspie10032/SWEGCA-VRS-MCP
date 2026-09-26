#include "vrs/connection.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace swegca::vrs {
namespace {
using namespace architecture::kernel;

// Hashing routes exact digest keys to containers; it never classifies evidence.
struct DigestHash {
    std::size_t operator()(const Digest& value) const noexcept {
        std::size_t hash = 0;
        for (const auto byte : value) hash = hash * 131 + std::to_integer<unsigned>(byte);
        return hash;
    }
};
struct GroupKey {
    Digest source, context;
    std::uint32_t axis;
    bool operator==(const GroupKey&) const = default;
};
struct GroupHash {
    std::size_t operator()(const GroupKey& key) const noexcept {
        return (DigestHash{}(key.source) * 131 + DigestHash{}(key.context)) * 131 + key.axis;
    }
};
struct Group { GroupKey key; std::uint32_t supports = 0, refutes = 0; };
using DigestSet = std::pmr::unordered_set<Digest, DigestHash>;

// Explicit Fisher-Yates and unbiased bounded draws make the traversal stable
// across standard-library shuffle implementations. This is data movement;
// only SWEGCA's observation and judgment kernels decide its interpretation.
void shuffle(std::span<RefinementSample> values, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    for (std::size_t count = values.size(); count > 1; --count) {
        const auto bound = static_cast<std::uint64_t>(count);
        const auto reject_below = (std::uint64_t{0} - bound) % bound;
        std::uint64_t draw;
        do { draw = random(); } while (draw < reject_below);
        std::swap(values[count - 1], values[draw % bound]);
    }
}
}  // namespace

Connection::Connection(const architecture::DigestBytes& identity, double initial_strength,
    const architecture::kernel::EvidenceRules& rules, MemoryBudget& memory)
    : identity_(identity), strength_(initial_strength), rules_(rules), memory_(memory),
      experiences_(&memory) {
    if (!named_digest(identity_) || !finite_count(strength_) || !rules_valid(rules_))
        throw std::invalid_argument("invalid VRS connection");
}

void Connection::append(const ExperienceEvidence& experience) {
    const auto& address = experience.original();
    const auto& value = experience.value();
    if (!named_digest(address.block) || address.offset < ExperienceBlock::header_bytes ||
        address.bytes < ExperienceBlock::record_overhead ||
        address.bytes > std::numeric_limits<std::uint64_t>::max() - address.offset ||
        address.digest != value.address ||
        admit_observation(rules_, identity_, value, value.observed_at, false) == ObservationUse::invalid)
        throw std::invalid_argument("invalid VRS experience observation");
    if (experiences_.size() >= std::numeric_limits<std::uint32_t>::max() ||
        revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("VRS connection revision or sample count exhausted");
    experiences_.push_back(experience);  // failed allocation leaves owner unchanged
    ++revision_;
}

ConnectionRefinement Connection::refine(std::uint64_t seed, std::uint64_t current_step) {
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("VRS connection revision exhausted");
    ConnectionRefinement report(memory_);
    report.connection_ = identity_;
    report.before_revision_ = report.after_revision_ = revision_;
    report.seed_ = seed;
    report.current_step_ = current_step;
    report.samples_.resize(experiences_.size());
    for (std::size_t i = 0; i < experiences_.size(); ++i)
        report.samples_[i].experience_index = static_cast<std::uint32_t>(i);
    shuffle(report.samples_, seed);

    DigestSet seen(&memory_), sources(&memory_), contexts(&memory_), producers(&memory_);
    std::pmr::unordered_map<GroupKey, std::size_t, GroupHash> group_index(&memory_);
    std::pmr::vector<Group> groups(&memory_);
    std::pmr::vector<std::uint8_t> recent(&memory_);
    recent.resize(std::min<std::size_t>(rules_.recent_window(), experiences_.size()));
    // All accumulators are fresh for this shuffled batch. A previous cycle's
    // tally is neither an input nor retained on the connection.
    auto& tally = report.evidence_;
    for (auto& sample : report.samples_) {
        const auto& value = experiences_[sample.experience_index].value();
        sample.use = admit_observation(rules_, identity_, value, current_step, seen.contains(value.address));
        if (sample.use != ObservationUse::applied) continue;
        seen.insert(value.address);
        sources.insert(value.source);
        contexts.insert(value.context);
        producers.insert(value.producer);
        const GroupKey key{value.source, value.context, value.axis};
        const auto [where, inserted] = group_index.try_emplace(key, groups.size());
        if (inserted) groups.push_back({key});
        auto& group = groups[where->second];
        if (value.outcome == EvidenceOutcome::support) ++group.supports;
        else ++group.refutes;
        const auto recent_value = std::uint8_t(value.outcome == EvidenceOutcome::support);
        const auto position = tally.revision % recent.size();
        if (tally.recent_count == recent.size()) tally.recent_sum -= recent[position];
        else ++tally.recent_count;
        recent[position] = recent_value;
        tally.recent_sum += recent_value;
        ++tally.revision;
    }
    // The original accumulator sums groups in first-observed order per axis.
    // Retain that numerical order, independently of hash-container order.
    for (const auto& group : groups) {
        const auto effective = normalize_evidence_group(group.supports, group.refutes);
        tally.axis_support[group.key.axis] += effective.support;
        tally.axis_refute[group.key.axis] += effective.refute;
    }
    tally.source_diversity = static_cast<std::uint32_t>(std::min(sources.size(), producers.size()));
    tally.context_diversity = static_cast<std::uint32_t>(std::min(contexts.size(), producers.size()));
    for (std::uint32_t axis = 0; axis < rules_.axis_count(); ++axis) {
        sources.clear();
        producers.clear();
        for (const auto& sample : report.samples_) {
            const auto& value = experiences_[sample.experience_index].value();
            if (sample.use == ObservationUse::applied && value.axis == axis) {
                sources.insert(value.source);
                producers.insert(value.producer);
            }
        }
        tally.axis_source_diversity[axis] = static_cast<std::uint32_t>(
            std::min(sources.size(), producers.size()));
    }
    report.result_ = verify_connection(rules_, tally, strength_);
    // Nothing after this point allocates or throws. The result cannot be
    // replayed onto another connection; there is deliberately no apply API.
    if (report.result_.strength().valid()) {
        strength_ = report.result_.strength().current();
        report.after_revision_ = ++revision_;
    }
    return report;
}

}  // namespace swegca::vrs
