#include "world/memory_promotion.hpp"

#include "swegca_architecture/memory_promotion_kernel.hpp"
#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

using CoreAction = architecture::kernel::MemoryPromotionAction;
using CoreReason = architecture::kernel::EvidenceReason;
using CoreStatus = architecture::kernel::EvidenceStatus;
using CoreTier = architecture::kernel::MemoryTier;

bool has_text(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    });
}

CoreTier core_tier(const MemoryTier tier) {
    return static_cast<CoreTier>(tier);
}

CoreStatus core_status(const std::string_view value) {
    if (value == "accept") return CoreStatus::accept;
    if (value == "reject") return CoreStatus::reject;
    if (value == "abstain") return CoreStatus::abstain;
    throw std::invalid_argument("unsupported accumulator status");
}

CoreReason core_reason(const std::string_view value) {
    if (value == "minimum_effective_samples") return CoreReason::minimum_effective_samples;
    if (value == "source_diversity") return CoreReason::source_diversity;
    if (value == "axis_source_diversity") return CoreReason::axis_source_diversity;
    if (value == "context_diversity") return CoreReason::context_diversity;
    if (value == "regime_change_suspected") return CoreReason::regime_change_suspected;
    if (value == "causal_lower_bound") return CoreReason::causal_lower_bound;
    if (value == "upper_bound_below_threshold") return CoreReason::upper_bound_below_threshold;
    if (value == "uncertain") return CoreReason::uncertain;
    throw std::invalid_argument("unsupported accumulator reason");
}

std::string_view action_name(const CoreAction action) {
    switch (action) {
    case CoreAction::quarantine: return "quarantine";
    case CoreAction::retract: return "retract";
    case CoreAction::promote: return "promote";
    case CoreAction::refresh_semantic: return "refresh_semantic";
    case CoreAction::record_episode: return "record_episode";
    case CoreAction::none: break;
    }
    return "none";
}

std::string_view reason_name(const architecture::kernel::MemoryPromotionReason reason) {
    using R = architecture::kernel::MemoryPromotionReason;
    switch (reason) {
    case R::minimum_effective_samples: return "minimum_effective_samples";
    case R::source_diversity: return "source_diversity";
    case R::axis_source_diversity: return "axis_source_diversity";
    case R::context_diversity: return "context_diversity";
    case R::regime_change_suspected: return "regime_change_suspected";
    case R::causal_lower_bound: return "causal_lower_bound";
    case R::upper_bound_below_threshold: return "upper_bound_below_threshold";
    case R::uncertain: return "uncertain";
    case R::incomplete_provenance: return "incomplete_provenance";
    case R::invalid_input: break;
    }
    return "invalid_input";
}

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[2 * index] = digits[value >> 4U];
        result[2 * index + 1] = digits[value & 15U];
    }
    return result;
}

JsonValue aliases_json(const std::vector<std::string>& aliases) {
    JsonValue::Array values;
    for (const auto& alias : aliases) values.emplace_back(alias);
    return values;
}

}  // namespace

std::string_view memory_tier_name(const MemoryTier tier) noexcept {
    switch (tier) {
    case MemoryTier::none: return "none";
    case MemoryTier::episodic: return "episodic";
    case MemoryTier::semantic: return "semantic";
    case MemoryTier::quarantined: return "quarantined";
    case MemoryTier::retracted: return "retracted";
    }
    return "none";
}

MemoryCandidate::MemoryCandidate(
    std::string hypothesis_id_value, std::string key_value, std::string value_value,
    std::vector<std::string> evidence_refs_value, std::string source_id_value,
    std::string source_revision_value, std::string timestamp_value,
    std::string license_value, std::string attribution_value,
    std::vector<std::string> aliases)
    : hypothesis_id(std::move(hypothesis_id_value)), key(std::move(key_value)),
      value(std::move(value_value)), evidence_refs(std::move(evidence_refs_value)),
      source_id(std::move(source_id_value)), source_revision(std::move(source_revision_value)),
      timestamp(std::move(timestamp_value)), license(std::move(license_value)),
      attribution(std::move(attribution_value)) {
    if (!has_text(hypothesis_id) || !has_text(key) || !has_text(value) ||
        !has_text(source_id) || !has_text(source_revision) || !has_text(timestamp) ||
        !has_text(license) || !has_text(attribution) || evidence_refs.empty() ||
        std::ranges::any_of(evidence_refs, [](const auto& ref) { return !has_text(ref); }) ||
        std::ranges::any_of(aliases, [](const auto& alias) { return !has_text(alias); }))
        throw std::invalid_argument("memory candidate fields and provenance must be nonempty");
    for (auto& alias : aliases)
        if (std::ranges::find(retrieval_aliases, alias) == retrieval_aliases.end())
            retrieval_aliases.push_back(std::move(alias));
}

MemoryPromotionDecision::MemoryPromotionDecision(
    const MemoryTier previous, const MemoryTier next, std::string action,
    std::string reason, const bool allowed)
    : previous_tier_(previous), next_tier_(next), action_(std::move(action)),
      reason_(std::move(reason)), semantic_read_allowed_(allowed) {}
MemoryTier MemoryPromotionDecision::previous_tier() const noexcept { return previous_tier_; }
MemoryTier MemoryPromotionDecision::next_tier() const noexcept { return next_tier_; }
std::string_view MemoryPromotionDecision::action() const noexcept { return action_; }
std::string_view MemoryPromotionDecision::reason() const noexcept { return reason_; }
bool MemoryPromotionDecision::semantic_read_allowed() const noexcept { return semantic_read_allowed_; }

MemoryPromotionDecision decide_memory_promotion(
    const MemoryTier current_tier, const AccumulatorDecision& evidence,
    const bool counterfactual_verified, const bool provenance_complete) {
    require_authoritative_accumulator_decision(evidence);
    architecture::kernel::MemoryPromotionDecision result;
    if (!architecture::kernel::decide_memory_promotion_fields(
            core_tier(current_tier), core_status(evidence.status), core_reason(evidence.reason),
            counterfactual_verified, provenance_complete, result))
        throw std::invalid_argument("memory promotion input changed");
    return MemoryPromotionDecision(
        static_cast<MemoryTier>(result.previous_tier),
        static_cast<MemoryTier>(result.next_tier), std::string(action_name(result.action)),
        std::string(reason_name(result.reason)), result.semantic_read_allowed);
}

MemoryCandidateDocument memory_candidate_document(
    const MemoryCandidate& candidate, const MemoryTier tier, std::string docid) {
    const auto tier_name = std::string(memory_tier_name(tier));
    std::string text = "FACT key=" + candidate.key + " value=" + candidate.value;
    if (!candidate.retrieval_aliases.empty())
        text += " aliases=" + semantic_canonical_json(aliases_json(candidate.retrieval_aliases));
    if (docid.empty()) docid = tier_name + ":" + candidate.hypothesis_id;
    return {std::move(docid), candidate.source_id, candidate.source_revision,
            candidate.key, candidate.source_revision, tier_name + "_memory",
            candidate.timestamp, false, tier_name + " memory " + candidate.key,
            text, hex(architecture::Sha256::of(
                std::as_bytes(std::span(text.data(), text.size())))), "unique",
            "evidence://" + [&] {
                std::string joined;
                for (const auto& ref : candidate.evidence_refs) {
                    if (!joined.empty()) joined += ',';
                    joined += ref;
                }
                return joined;
            }(), candidate.license, candidate.attribution};
}

}  // namespace swegca::world
