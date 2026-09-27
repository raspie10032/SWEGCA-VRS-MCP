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
struct GroupCounts { std::uint32_t supports = 0, refutes = 0; };
using DigestSet = std::pmr::unordered_set<Digest, DigestHash>;

// Explicit Fisher-Yates and unbiased bounded draws make the traversal stable
// across standard-library shuffle implementations. This is data movement;
// only SWEGCA's observation and judgment kernels decide its interpretation.
void shuffle(std::span<std::uint32_t> values, std::uint64_t seed) {
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
      experiences_(memory) {
    if (!named_digest(identity_) || !finite_count(strength_) || !rules_valid(rules_))
        throw std::invalid_argument("invalid VRS connection");
}

void Connection::append(const ExperienceEvidence& experience) {
    prepare_append(experience);
    commit_append(experience);
}

void Connection::validate_experience(const ExperienceEvidence& experience) const {
    const auto& address = experience.original();
    const auto& value = experience.value();
    if (!named_digest(address.block) || address.offset < ExperienceBlock::header_bytes ||
        address.bytes < ExperienceBlock::record_overhead ||
        address.bytes > std::numeric_limits<std::uint64_t>::max() - address.offset ||
        address.digest != value.address ||
        admit_observation(rules_, identity_, value, value.observed_at, false) == ObservationUse::invalid)
        throw std::invalid_argument("invalid VRS experience observation");
}

void Connection::inherit_experiences(const Connection& previous) {
    if(identity_!=previous.identity_||revision_||experiences_.size())
        throw std::logic_error("invalid Main experience inheritance");
    // Preserve the original per-experience core admission check. Sharing only
    // replaces physical metadata copies, never the later full shuffled tally.
    auto reader=previous.experience_reader();
    if(&memory_==&previous.memory_) {
        for(std::size_t i=0;i<reader.size();++i)validate_experience(reader[i]);
        experiences_.share_prefix(previous.experiences_);
        revision_=experiences_.size();
    } else {
        // Separate budget lifetimes cannot own each other's segments.
        for(std::size_t i=0;i<reader.size();++i)append(reader[i]);
    }
}

void Connection::prepare_append(const ExperienceEvidence& experience) {
    validate_experience(experience);
    if (experiences_.size() >= std::numeric_limits<std::uint32_t>::max() ||
        revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("VRS connection revision or sample count exhausted");
    experiences_.prepare_append();
}

void Connection::commit_append(const ExperienceEvidence& experience) noexcept {
    // Only prepare_append followed by this call on the same serialized owner.
    experiences_.commit_append(experience);
    ++revision_;
}

ConnectionRefinement Connection::refine(std::uint64_t seed, std::uint64_t current_step) {
    auto report = prepare_refinement(seed, current_step);
    commit_refinement(report);
    return report;
}

ConnectionRefinement Connection::prepare_refinement(std::uint64_t seed, std::uint64_t current_step) const {
    return prepare_refinement(seed,current_step,0,experiences_.size(),revision_);
}
ConnectionRefinement Connection::evaluate_suffix(std::size_t begin,std::uint64_t seed,std::uint64_t step) const {
    if(begin>experiences_.size())throw std::out_of_range("evaluation suffix boundary");
    // Preserve fresh-connection admission validation without duplicating values.
    {
        auto reader=experiences_.reader();
        for(auto index=begin;index<experiences_.size();++index)validate_experience(reader[index]);
    }
    const auto count=experiences_.size()-begin;
    return prepare_refinement(seed,step,begin,count,count);
}
ConnectionRefinement Connection::prepare_refinement(std::uint64_t seed,std::uint64_t current_step,
    std::size_t begin,std::size_t count,std::uint64_t revision) const {
    if (revision == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("VRS connection revision exhausted");
    ConnectionRefinement report(memory_);
    report.connection_ = identity_;
    report.before_revision_ = report.after_revision_ = revision;
    report.seed_ = seed;
    report.current_step_ = current_step;
    report.indices_.resize(count);
    report.uses_.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        report.indices_[i] = static_cast<std::uint32_t>(i);
    shuffle(report.indices_, seed);

    static_assert(max_axes<=8);
    std::size_t producer_count=0;
    std::array<std::uint32_t,max_axes> axis_producers{};
    using GroupIndex=std::pmr::unordered_map<GroupKey,GroupCounts,GroupHash>;
    GroupIndex group_index(&memory_);
    // Rehash preserves node addresses. Keep first-observed order without
    // duplicating source/context/axis keys in the ordered sequence.
    std::pmr::vector<const GroupIndex::value_type*> groups(&memory_);
    std::pmr::vector<std::uint8_t> recent(&memory_);
    recent.resize(std::min<std::size_t>(rules_.recent_window(), count));
    // All accumulators are fresh for this shuffled batch. A previous cycle's
    // tally is neither an input nor retained on the connection.
    auto& tally = report.evidence_;
    {
        std::pmr::unordered_map<Digest,std::uint8_t,DigestHash> producers(&memory_);
        auto reader=experiences_.reader();
        DigestSet seen(&memory_);
        for (std::size_t ordinal=0;ordinal<report.indices_.size();++ordinal) {
            const auto& value = reader[begin+report.indices_[ordinal]].value();
            auto& use=report.uses_[ordinal];
            use = admit_observation(rules_, identity_, value, current_step, seen.contains(value.address));
            if (use != ObservationUse::applied) continue;
            seen.insert(value.address);
            auto& axes=producers.try_emplace(value.producer,0).first->second;
            const auto bit=static_cast<std::uint8_t>(1U<<value.axis);
            if(!(axes&bit)){axes|=bit;++axis_producers[value.axis];}
            const GroupKey key{value.source, value.context, value.axis};
            const auto [where, inserted] = group_index.try_emplace(key);
            if (inserted) groups.push_back(&*where);
            auto& group = where->second;
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
        producer_count=producers.size();
    } // Admission scratch and producer keys are no longer needed.
    // The original accumulator sums groups in first-observed order per axis.
    // Retain that numerical order, independently of hash-container order.
    for (const auto* entry : groups) {
        const auto& [key,group]=*entry;
        const auto effective = normalize_evidence_group(group.supports, group.refutes);
        tally.axis_support[key.axis] += effective.support;
        tally.axis_refute[key.axis] += effective.refute;
    }
    // These keys come only from applied observations. Reuse one set after
    // admission instead of holding both source/context sets alongside `seen`.
    DigestSet sources(&memory_);
    // The existing diversity value is min(distinct keys, producer count).
    // Once that exact value is known, additional distinct-key storage cannot
    // change it. This does not truncate admission, shuffle, or evidence sums.
    const auto distinct=[&](std::size_t limit,bool context,std::uint32_t axis=max_axes){
        sources.clear();
        if(!limit)return std::uint32_t{0};
        for(const auto* entry:groups){
            if(axis!=max_axes&&entry->first.axis!=axis)continue;
            sources.insert(context?entry->first.context:entry->first.source);
            if(sources.size()==limit)break;
        }
        return static_cast<std::uint32_t>(sources.size());
    };
    tally.source_diversity = distinct(producer_count,false);
    tally.context_diversity = distinct(producer_count,true);
    for (std::uint32_t axis = 0; axis < rules_.axis_count(); ++axis) {
        tally.axis_source_diversity[axis] = distinct(axis_producers[axis],false,axis);
    }
    report.result_ = verify_connection(rules_, tally, strength_);
    if (report.result_.strength().valid()) report.after_revision_ = revision + 1;
    return report;
}

void Connection::commit_refinement(const ConnectionRefinement& report) noexcept {
    // Private, serialized composition; no externally supplied result can be
    // installed. The persistent owner writes the exact result before this step.
    if (report.result().strength().valid()) strength_ = report.result().strength().current();
    revision_ = report.after_revision();
}

}  // namespace swegca::vrs
