#pragma once

#include "swegca_architecture/evidence_observation_kernel.hpp"
#include "vrs/experience_block.hpp"

namespace swegca::vrs {

// Observation values and original payload share the block record checksum.
// This is a decoded record, not an arbitrary (address, outcome) pair. Storage
// binding does not prove the producer's observation is true: SWEGCA decides.
class ExperienceEvidence final {
public:
    [[nodiscard]] const ExperienceLocation& original() const noexcept { return original_; }
    [[nodiscard]] const architecture::kernel::EvidenceObservation& value() const noexcept { return value_; }
    [[nodiscard]] const architecture::DigestBytes& cue() const noexcept { return cue_; }
private:
    friend ExperienceEvidence record_evidence(ExperienceBlock&,
        const architecture::kernel::EvidenceRules&, const OriginalExperienceView&,
        const architecture::kernel::EvidenceObservation&);
    friend ExperienceEvidence decode_evidence(const architecture::kernel::EvidenceRules&,
        const StoredExperience&);
    ExperienceEvidence(const ExperienceLocation& original,
        const architecture::kernel::EvidenceObservation& value, const architecture::DigestBytes& cue)
        : original_(original), value_(value), cue_(cue) {}
    ExperienceLocation original_;
    architecture::kernel::EvidenceObservation value_;
    architecture::DigestBytes cue_;
};

// The observation is the recorded result of an experiment or producer, never
// a generated verdict for arbitrary prose. `address` must be empty on input;
// the persisted record supplies it. Its timestamp must match the original.
[[nodiscard]] ExperienceEvidence record_evidence(ExperienceBlock& block,
    const architecture::kernel::EvidenceRules& rules, const OriginalExperienceView& original,
    const architecture::kernel::EvidenceObservation& value);
[[nodiscard]] ExperienceEvidence decode_evidence(const architecture::kernel::EvidenceRules& rules,
    const StoredExperience& stored);

// Zero-copy access to the exact original media type/payload. The StoredExperience
// must outlive the view. No evidence verdict is inferred by this read.
[[nodiscard]] OriginalExperienceView evidence_payload(const StoredExperience& stored);

}  // namespace swegca::vrs
