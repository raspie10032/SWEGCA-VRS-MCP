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
    [[nodiscard]] bool has_input_key() const noexcept { return input_key_; }
private:
    friend class EvidenceReader;
    friend class ExperiencePage;
    friend ExperienceEvidence record_evidence(ExperienceBlock&,
        const architecture::kernel::EvidenceRules&, const OriginalExperienceView&,
        const architecture::kernel::EvidenceObservation&, std::optional<architecture::DigestBytes>);
    friend ExperienceEvidence decode_evidence(const architecture::kernel::EvidenceRules&,
        const StoredExperience&);
    friend ExperienceEvidence read_evidence(const architecture::kernel::EvidenceRules&,
        const ExperienceBlock&, const ExperienceLocation&, std::uint64_t);
    ExperienceEvidence(const ExperienceLocation& original,
        const architecture::kernel::EvidenceObservation& value, const architecture::DigestBytes& cue, bool input_key)
        : original_(original), value_(value), cue_(cue), input_key_(input_key) {}
    ExperienceLocation original_;
    architecture::kernel::EvidenceObservation value_;
    architecture::DigestBytes cue_;
    bool input_key_;
};

// The observation is the recorded result of an experiment or producer, never
// a generated verdict for arbitrary prose. `address` must be empty on input;
// the persisted record supplies it. Its timestamp must match the original.
[[nodiscard]] ExperienceEvidence record_evidence(ExperienceBlock& block,
    const architecture::kernel::EvidenceRules& rules, const OriginalExperienceView& original,
    const architecture::kernel::EvidenceObservation& value,
    std::optional<architecture::DigestBytes> input_key = std::nullopt);
[[nodiscard]] ExperienceEvidence decode_evidence(const architecture::kernel::EvidenceRules& rules,
    const StoredExperience& stored);
// Validate the entire original with fixed scratch space, retaining only its
// observation and exact cue. Selected Replay still uses the full read API.
[[nodiscard]] ExperienceEvidence read_evidence(const architecture::kernel::EvidenceRules&,
    const ExperienceBlock&, const ExperienceLocation&, std::uint64_t max_read_bytes);

// A verified byte range of one original payload, not a completed Replay receipt
// or an evidence verdict. The entire enclosing record is checked before return.
// The caller's memory budget must outlive this object.
class EvidencePayloadSlice final {
public:
    EvidencePayloadSlice(const EvidencePayloadSlice&) = delete;
    EvidencePayloadSlice& operator=(const EvidencePayloadSlice&) = delete;
    EvidencePayloadSlice(EvidencePayloadSlice&&) noexcept = default;
    [[nodiscard]] const ExperienceEvidence& evidence() const noexcept { return evidence_; }
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_; }
    [[nodiscard]] std::span<const std::byte> content() const noexcept { return content_; }
private:
    friend EvidencePayloadSlice read_evidence_slice(const architecture::kernel::EvidenceRules&,
        const ExperienceBlock&, const ExperienceLocation&, std::uint64_t,
        std::uint64_t, std::uint64_t, MemoryBudget&);
    EvidencePayloadSlice(ExperienceEvidence evidence, std::uint64_t offset,
        std::uint64_t total, std::pmr::vector<std::byte> content)
        : evidence_(std::move(evidence)), offset_(offset), total_(total), content_(std::move(content)) {}
    ExperienceEvidence evidence_;
    std::uint64_t offset_, total_;
    std::pmr::vector<std::byte> content_;
};

// Strict range bounds: offset + count must fit the raw payload. Retains only
// count bytes and uses fixed scratch space; still reads/hashes the whole record.
[[nodiscard]] EvidencePayloadSlice read_evidence_slice(const architecture::kernel::EvidenceRules&,
    const ExperienceBlock&, const ExperienceLocation&, std::uint64_t max_read_bytes,
    std::uint64_t offset, std::uint64_t count, MemoryBudget&);

// Sealed delivery provenance, exposed only after full original authentication
// and SWEGCA observation admission. Does not retain the original payload.
class OriginalDelivery final {
public:
    [[nodiscard]] ExperienceSender sender() const noexcept{return sender_;}
    [[nodiscard]] const ExperienceLocation& original() const noexcept{return original_;}
    [[nodiscard]] std::uint64_t sequence() const noexcept{return sequence_;}
    [[nodiscard]] const architecture::DigestBytes& fingerprint() const noexcept{return fingerprint_;}
    [[nodiscard]] const architecture::DigestBytes& context() const noexcept{return context_;}
private:
    friend OriginalDelivery read_delivery(const architecture::kernel::EvidenceRules&,
        const ExperienceBlock&,const ExperienceLocation&,std::uint64_t,
        std::string_view,std::string_view,std::string_view);
    OriginalDelivery(ExperienceLocation original,std::uint64_t sequence,architecture::DigestBytes fingerprint,architecture::DigestBytes context,ExperienceSender sender)
        :original_(original),sequence_(sequence),fingerprint_(fingerprint),context_(context),sender_(sender){}
    ExperienceLocation original_;
    std::uint64_t sequence_;
    architecture::DigestBytes fingerprint_,context_;
    ExperienceSender sender_;
};
[[nodiscard]] OriginalDelivery read_delivery(const architecture::kernel::EvidenceRules&,
    const ExperienceBlock&,const ExperienceLocation&,std::uint64_t limit,
    std::string_view session,std::string_view source,std::string_view media);

// Zero-copy access to the exact original media type/payload. The StoredExperience
// must outlive the view. No evidence verdict is inferred by this read.
[[nodiscard]] OriginalExperienceView evidence_payload(const StoredExperience& stored);

}  // namespace swegca::vrs
