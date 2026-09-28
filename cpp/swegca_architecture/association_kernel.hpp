#pragma once

#include "swegca_architecture/evidence_observation_kernel.hpp"
#include <span>
#include <algorithm>

namespace swegca::architecture::kernel {

// A verified SOURCE BINDING, not image identity, tag truth, or World authority.
// Host supplies digests from authenticated original records and the existing
// paired processing record. The user's verified image/tag/DINO bundle remains
// one observation; its modalities are not independent producers.
class BoundExperience final {
public:
    constexpr BoundExperience() noexcept = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
private:
    friend BoundExperience bind_experience(const Digest&,const Digest&,const Digest&,const Digest&) noexcept;
    bool valid_=false;
};
[[nodiscard]] inline BoundExperience bind_experience(const Digest& actual_image,
    const Digest& declared_image,const Digest& tag_record,const Digest& dino_record) noexcept {
    BoundExperience result;
    result.valid_=named_digest(actual_image)&&actual_image==declared_image&&
        named_digest(tag_record)&&named_digest(dino_record);
    return result;
}

// Scope: a recorded association witness in the already verified bundle.
// No score threshold, causal requirement, or absence-as-refutation rule.
[[nodiscard]] inline EvidenceOutcome observe_tag_association(const BoundExperience& binding,
    std::span<const std::uint32_t> actual_tags,std::uint32_t left,std::uint32_t right) noexcept {
    if(!binding.valid()||left==right)return EvidenceOutcome::insufficient;
    bool seen_left=false,seen_right=false;
    for(auto tag:actual_tags){seen_left|=tag==left;seen_right|=tag==right;}
    return seen_left&&seen_right?EvidenceOutcome::support:EvidenceOutcome::insufficient;
}

// An observed member of a verified bundle, not a semantic truth verdict.
[[nodiscard]] inline EvidenceOutcome observe_recorded_member(
    const BoundExperience& binding, std::span<const std::uint32_t> members,
    std::uint32_t member) noexcept {
    return binding.valid() && std::find(members.begin(),members.end(),member)!=members.end()
        ? EvidenceOutcome::support : EvidenceOutcome::insufficient;
}
// User-defined exhaustive tag-versus-image check: a recorded matching tag
// supports this relation; a nonmatching tag refutes it. This binary contract
// is scoped to tag-image verification, not generic missing evidence.
[[nodiscard]] inline EvidenceOutcome observe_tag_image_match(
    const BoundExperience& binding, std::span<const std::uint32_t> members,
    std::uint32_t member) noexcept {
    if(!binding.valid())return EvidenceOutcome::insufficient;
    return std::find(members.begin(),members.end(),member)!=members.end()
        ? EvidenceOutcome::support : EvidenceOutcome::refute;
}
// The proposition is local to THIS pair: does this member occur in both?
// A third input may corroborate a member, but cannot invent it in either end.
[[nodiscard]] inline EvidenceOutcome observe_common_member(
    const BoundExperience& left, std::span<const std::uint32_t> left_members,
    const BoundExperience& right, std::span<const std::uint32_t> right_members,
    std::uint32_t member) noexcept {
    return observe_recorded_member(left,left_members,member)==EvidenceOutcome::support &&
        observe_recorded_member(right,right_members,member)==EvidenceOutcome::support
        ? EvidenceOutcome::support : EvidenceOutcome::insufficient;
}

struct AssociationEvidence {
    std::uint64_t support=0;
    // Only an explicit, source-bound refutation of THIS relation may enter
    // here. A missing tag, unseen pair or failed input read is not refutation.
    std::uint64_t refute=0;
};
enum class AssociationReason : std::uint8_t {
    unresolved=1, observed_association=2, explicit_refutation=3, conflicting_evidence=4,
};
// This distinct result cannot be passed to World/semantic promotion gates that
// require EvidenceJudgment. The general four-axis decision remains unchanged.
class AssociationJudgment final {
public:
    constexpr AssociationJudgment() noexcept = default;
    [[nodiscard]] constexpr EvidenceStatus status() const noexcept{return status_;}
    [[nodiscard]] constexpr AssociationReason reason() const noexcept{return reason_;}
private:
    friend AssociationJudgment judge_association(const AssociationEvidence&) noexcept;
    EvidenceStatus status_=EvidenceStatus::abstain;
    AssociationReason reason_=AssociationReason::unresolved;
};
// Existing local VRS rule: support strengthens, explicit refutation weakens,
// unresolved/conflicting evidence preserves. No truth probability is claimed.
[[nodiscard]] inline AssociationJudgment judge_association(const AssociationEvidence& e) noexcept {
    AssociationJudgment result;
    if(e.support&&e.refute)result.reason_=AssociationReason::conflicting_evidence;
    else if(e.support){result.status_=EvidenceStatus::accept;result.reason_=AssociationReason::observed_association;}
    else if(e.refute){result.status_=EvidenceStatus::reject;result.reason_=AssociationReason::explicit_refutation;}
    return result;
}
class AssociationStrength final {
public:
    [[nodiscard]] constexpr bool valid() const noexcept{return valid_;}
    [[nodiscard]] constexpr double current() const noexcept{return current_;}
private:
    friend AssociationStrength revise_association_strength(double,const AssociationJudgment&) noexcept;
    bool valid_=false;double current_=0;
};
[[nodiscard]] inline AssociationStrength revise_association_strength(
    double previous,const AssociationJudgment& judgment) noexcept {
    AssociationStrength result;
    if(!finite_count(previous))return result;
    const auto status=judgment.status();
    const auto current=status==EvidenceStatus::accept?previous*1.01:
        status==EvidenceStatus::reject?previous*.995:previous;
    if(!finite_count(current))return result;
    result.valid_=true;result.current_=current;return result;
}
} // namespace swegca::architecture::kernel
