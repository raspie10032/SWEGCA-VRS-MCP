#pragma once

#include "swegca_vrs/memory_candidate_reference.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Main's prospective memory value before promotion. It preserves the
// author's required fields, provenance references and alias order; owning
// this value does not make it semantic evidence or authorize a write.
// Native references distinguish addressed original experience from a
// Main-held write receipt, as the linked transaction requires.
// SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:234-239
namespace swegca::vrs {

struct MemoryCandidateInput final {
    std::string_view hypothesis_id;
    std::string_view key;
    std::string_view value;
    std::span<const MemoryCandidateReference> evidence_refs;
    std::string_view source_id;
    std::string_view source_revision;
    std::string_view timestamp;
    std::string_view license;
    std::string_view attribution;
    std::span<const std::string_view> retrieval_aliases;
};

class MemoryCandidate final {
public:
    using Text = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;
    using Aliases = std::vector<Text, AllocationAdapter<Text>>;

    // Validates the author's str.strip requirements and removes repeated
    // aliases after their first appearance. Generalized UTF-8 is the native
    // byte representation of Python str; no candidate-specific byte cap is
    // imposed. All owned bytes use the supplied host account.
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    MemoryCandidate(const AllocationContext& memory, const MemoryCandidateInput& input);

    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view hypothesis_id() const noexcept { return hypothesis_id_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view key() const noexcept { return key_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view value() const noexcept { return value_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::span<const MemoryCandidateReference> evidence_refs() const noexcept {
        return evidence_refs_;
    }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view source_id() const noexcept { return source_id_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view source_revision() const noexcept { return source_revision_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view timestamp() const noexcept { return timestamp_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view license() const noexcept { return license_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::string_view attribution() const noexcept { return attribution_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
    [[nodiscard]] std::span<const Text> retrieval_aliases() const noexcept { return aliases_; }

private:
    Text hypothesis_id_;
    Text key_;
    Text value_;
    MemoryCandidateReferences evidence_refs_;
    Text source_id_;
    Text source_revision_;
    Text timestamp_;
    Text license_;
    Text attribution_;
    Aliases aliases_;
};

}  // namespace swegca::vrs
