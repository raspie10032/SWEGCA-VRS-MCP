#pragma once

#include "world/cognitive_state.hpp"
#include "world/evidence_accumulator.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view memory_promotion_source_sha256 =
    "336073a8eb69357d5761add74316a6f06719c61bbaeb93b82fd3e4005390d9e5";
inline constexpr double verified_experience_promotion_strength = 1.0;

enum class MemoryTier : std::uint8_t {
    none, episodic, semantic, quarantined, retracted,
};

struct MemoryCandidate final {
    std::string hypothesis_id;
    std::string key;
    std::string value;
    std::vector<std::string> evidence_refs;
    std::string source_id;
    std::string source_revision;
    std::string timestamp;
    std::string license;
    std::string attribution;
    std::vector<std::string> retrieval_aliases;

    MemoryCandidate(std::string hypothesis_id, std::string key, std::string value,
                    std::vector<std::string> evidence_refs, std::string source_id,
                    std::string source_revision, std::string timestamp,
                    std::string license, std::string attribution,
                    std::vector<std::string> retrieval_aliases = {});
};

class MemoryPromotionDecision final {
public:
    [[nodiscard]] MemoryTier previous_tier() const noexcept;
    [[nodiscard]] MemoryTier next_tier() const noexcept;
    [[nodiscard]] std::string_view action() const noexcept;
    [[nodiscard]] std::string_view reason() const noexcept;
    [[nodiscard]] bool semantic_read_allowed() const noexcept;

private:
    MemoryPromotionDecision(MemoryTier previous_tier, MemoryTier next_tier,
                            std::string action, std::string reason,
                            bool semantic_read_allowed);
    MemoryTier previous_tier_;
    MemoryTier next_tier_;
    std::string action_;
    std::string reason_;
    bool semantic_read_allowed_{};
    friend MemoryPromotionDecision decide_memory_promotion(
        MemoryTier, const AccumulatorDecision&, bool, bool);
};

struct MemoryCandidateDocument final {
    std::string docid;
    std::string source_id;
    std::string source_revision;
    std::string page_id;
    std::string revision_id;
    std::string namespace_name;
    std::string timestamp;
    bool redirect{};
    std::string title;
    std::string text;
    std::string content_sha256;
    std::string duplicate_status;
    std::string source_url;
    std::string license;
    std::string attribution;
};

[[nodiscard]] MemoryPromotionDecision decide_memory_promotion(
    MemoryTier current_tier, const AccumulatorDecision& evidence,
    bool counterfactual_verified, bool provenance_complete);
[[nodiscard]] MemoryCandidateDocument memory_candidate_document(
    const MemoryCandidate& candidate, MemoryTier tier,
    std::string docid = {});
[[nodiscard]] std::string_view memory_tier_name(MemoryTier tier) noexcept;

}  // namespace swegca::world
