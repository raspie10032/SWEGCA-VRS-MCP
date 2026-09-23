#include "swegca_vrs/memory_candidate.hpp"

#include "swegca_vrs/identity_types.hpp"

#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <unordered_set>
#include <variant>

namespace swegca::vrs {
namespace {

// SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
MemoryCandidate::Text own(const AllocationContext& memory, std::string_view value) {
    return MemoryCandidate::Text(value.data(), value.size(), memory.allocator<char>());
}

}  // namespace

// The source requires all eight scalar fields and each provenance reference
// to have str.strip content; references here already have typed identity.
// Its dict.fromkeys removes repeated aliases while retaining the first.
// This constructor owns a detached candidate, not a promotion decision.
// SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
MemoryCandidate::MemoryCandidate(const AllocationContext& memory,
                                 const MemoryCandidateInput& input)
    : hypothesis_id_(memory.allocator<char>()), key_(memory.allocator<char>()),
      value_(memory.allocator<char>()), evidence_refs_(memory.allocator<MemoryCandidateReference>()),
      source_id_(memory.allocator<char>()), source_revision_(memory.allocator<char>()),
      timestamp_(memory.allocator<char>()), license_(memory.allocator<char>()),
      attribution_(memory.allocator<char>()), aliases_(memory.allocator<Text>()) {
    for (const auto field : {input.hypothesis_id, input.key, input.value, input.source_id,
                             input.source_revision, input.timestamp, input.license,
                             input.attribution})
        if (!detail::has_python_strip_content(field))
            throw std::invalid_argument("memory_candidate_fields_invalid");
    if (input.evidence_refs.empty())
        throw std::invalid_argument("memory_candidate_evidence_refs_invalid");
    for (const auto alias : input.retrieval_aliases)
        if (!detail::has_python_strip_content(alias))
            throw std::invalid_argument("memory_candidate_alias_invalid");

    hypothesis_id_ = own(memory, input.hypothesis_id);
    key_ = own(memory, input.key);
    value_ = own(memory, input.value);
    source_id_ = own(memory, input.source_id);
    source_revision_ = own(memory, input.source_revision);
    timestamp_ = own(memory, input.timestamp);
    license_ = own(memory, input.license);
    attribution_ = own(memory, input.attribution);

    evidence_refs_.reserve(input.evidence_refs.size());
    for (const auto& reference : input.evidence_refs) {
        if (const auto* address = std::get_if<ExperienceAddress>(&reference))
            evidence_refs_.emplace_back(std::in_place_type<ExperienceAddress>, memory,
                                        address->value());
        else
            evidence_refs_.emplace_back(std::get<WriteReceiptLink>(reference));
    }
    // This temporary index borrows the input only for this constructor; the
    // ordered, deduplicated aliases remain wholly owned on `memory`.
    std::unordered_set<std::string_view, std::hash<std::string_view>,
                       std::equal_to<std::string_view>, AllocationAdapter<std::string_view>>
        seen(0, std::hash<std::string_view>{}, std::equal_to<std::string_view>{},
             memory.allocator<std::string_view>());
    for (const auto alias : input.retrieval_aliases)
        if (seen.insert(alias).second) aliases_.push_back(own(memory, alias));
}

}  // namespace swegca::vrs
