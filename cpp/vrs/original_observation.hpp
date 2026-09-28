#pragma once

#include "vrs/evidence_experience.hpp"
#include "swegca_architecture/content_observation_kernel.hpp"
#include <stdexcept>

namespace swegca::vrs {

// Measured predicate, not a core judgment. Both sealed origins and their
// recorded provenance travel with the result; their old outcome is not reused.
struct OriginalContentObservation {
    ExperienceLocation left, right;
    architecture::kernel::EvidenceObservation left_provenance, right_provenance;
    std::uint64_t left_bytes=0, right_bytes=0;
    bool expect_equal=true;
    architecture::kernel::EvidenceOutcome outcome=
        architecture::kernel::EvidenceOutcome::insufficient;
};

// The owner obtains StoredExperience through authenticated Replay/read paths.
// This explicitly measures raw payload byte equality, not semantic similarity.
// Read/authentication failures must not be converted to a negative observation.
// The owner must retain both origin addresses in the derived record, bind it
// to the current hypothesis and use the normal admission/shuffle/core path.
// Repeating this pair supplies no additional independent evidence.
[[nodiscard]] inline OriginalContentObservation observe_original_content_relation(
    const StoredExperience& left,const architecture::kernel::EvidenceRules& left_rules,
    const StoredExperience& right,const architecture::kernel::EvidenceRules& right_rules,
    bool expect_equal=true) {
    if(left.location()==right.location())
        throw std::invalid_argument("an original cannot independently corroborate itself");
    const auto l=decode_evidence(left_rules,left),r=decode_evidence(right_rules,right);
    const auto a=evidence_payload(left),b=evidence_payload(right);
    return {l.original(),r.original(),l.value(),r.value(),a.content.size(),b.content.size(),
        expect_equal,architecture::kernel::observe_raw_content_relation(
            a.content,b.content,true,true,expect_equal)};
}

} // namespace swegca::vrs
