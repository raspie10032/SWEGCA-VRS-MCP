#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace swegca::world {

class AuthorityError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class EvidenceAccumulatorConfig final {
public:
    EvidenceAccumulatorConfig(
        double chance_rate = 0.2, double accept_margin = 0.25,
        double confidence_level = 0.9, double beta_prior_alpha = 1.0,
        double beta_prior_beta = 1.0,
        std::uint64_t minimum_effective_samples_per_axis = 4,
        std::uint64_t minimum_source_diversity = 2,
        std::uint64_t minimum_source_diversity_per_axis = 1,
        std::uint64_t minimum_context_diversity = 4,
        std::uint64_t recent_window = 6,
        std::uint64_t minimum_recent_samples = 4,
        double regime_change_threshold = 0.3,
        std::vector<std::string> required_axes = {
            "observational", "counterfactual", "intervention", "cross_context"});

    const double chance_rate;
    const double accept_margin;
    const double confidence_level;
    const double beta_prior_alpha;
    const double beta_prior_beta;
    const std::uint64_t minimum_effective_samples_per_axis;
    const std::uint64_t minimum_source_diversity;
    const std::uint64_t minimum_source_diversity_per_axis;
    const std::uint64_t minimum_context_diversity;
    const std::uint64_t recent_window;
    const std::uint64_t minimum_recent_samples;
    const double regime_change_threshold;
    const std::vector<std::string> required_axes;
};

class EvidenceObservation final {
public:
    EvidenceObservation(
        std::string hypothesis_id, std::string evidence_address,
        std::string source_family, std::string context_hash, std::string axis,
        std::string outcome, std::int64_t observed_at,
        std::optional<std::int64_t> expires_at = std::nullopt,
        std::string producer_id = {}, double producer_confidence = 0.0,
        std::string source_address = {}, std::string source_revision = {});

    void validate() const;
    [[nodiscard]] std::string proposal_hash() const;

    const std::string hypothesis_id;
    const std::string evidence_address;
    const std::string source_family;
    const std::string context_hash;
    const std::string axis;
    const std::string outcome;
    const std::int64_t observed_at;
    const std::optional<std::int64_t> expires_at;
    const std::string producer_id;
    const double producer_confidence;
    const std::string source_address;
    const std::string source_revision;
};

struct EvidenceGroup final {
    std::string source_family;
    std::string context_hash;
    std::uint64_t supports{0};
    std::uint64_t refutes{0};
    std::vector<std::string> producer_ids;

    [[nodiscard]] double effective_support() const noexcept;
    [[nodiscard]] double effective_refute() const noexcept;
};

struct EvidenceAxisState final {
    std::string name;
    std::vector<EvidenceGroup> groups;

    [[nodiscard]] double effective_support() const noexcept;
    [[nodiscard]] double effective_refute() const noexcept;
    [[nodiscard]] double effective_samples() const noexcept;
    [[nodiscard]] std::size_t source_diversity() const;
};

class EvidenceAccumulatorState final {
public:
    // Public construction models an untrusted/deserialized value and never
    // grants authority. Use empty() to mint an accumulator-owned state.
    EvidenceAccumulatorState(std::string hypothesis_id,
                             const EvidenceAccumulatorConfig& config);
    [[nodiscard]] static std::shared_ptr<const EvidenceAccumulatorState> empty(
        std::string hypothesis_id, const EvidenceAccumulatorConfig& config);

    [[nodiscard]] const std::string& hypothesis_id() const noexcept;
    [[nodiscard]] std::span<const EvidenceAxisState> axes() const noexcept;
    [[nodiscard]] std::span<const std::string> seen_addresses() const noexcept;
    [[nodiscard]] std::span<const std::string> source_families() const noexcept;
    [[nodiscard]] std::span<const std::string> context_hashes() const noexcept;
    [[nodiscard]] std::span<const std::string> producer_ids() const noexcept;
    [[nodiscard]] std::span<const int> recent_outcomes() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

private:
    EvidenceAccumulatorState(std::string hypothesis_id,
                             std::vector<EvidenceAxisState> axes,
                             std::vector<std::string> seen_addresses,
                             std::vector<std::string> source_families,
                             std::vector<std::string> context_hashes,
                             std::vector<std::string> producer_ids,
                             std::vector<int> recent_outcomes,
                             std::uint64_t revision,
                             std::shared_ptr<const void> authority);

    std::string hypothesis_id_;
    std::vector<EvidenceAxisState> axes_;
    std::vector<std::string> seen_addresses_;
    std::vector<std::string> source_families_;
    std::vector<std::string> context_hashes_;
    std::vector<std::string> producer_ids_;
    std::vector<int> recent_outcomes_;
    std::uint64_t revision_{0};
    std::shared_ptr<const void> authority_;

    friend class AccumulatorAccess;
    friend struct AccumulatorUpdate;
    friend std::shared_ptr<const class AccumulatorDecision> assess_accumulator(
        const EvidenceAccumulatorState&, const EvidenceAccumulatorConfig&);
    friend struct AccumulatorUpdate update_accumulator(
        const std::shared_ptr<const EvidenceAccumulatorState>&,
        const EvidenceObservation&, const EvidenceAccumulatorConfig&,
        std::int64_t);
};

class AccumulatorDecision final {
public:
    // A directly constructed decision is data only and carries no authority.
    AccumulatorDecision(std::string status, std::string reason,
                        double posterior_mean, double causal_lower_bound,
                        double overall_upper_bound, double effective_sample_size,
                        std::size_t source_diversity,
                        std::size_t context_diversity,
                        double regime_change_score, std::uint64_t revision,
                        std::string hypothesis_id = {},
                        std::vector<std::string> evidence_addresses = {});

    const std::string status;
    const std::string reason;
    const double posterior_mean;
    const double causal_lower_bound;
    const double overall_upper_bound;
    const double effective_sample_size;
    const std::size_t source_diversity;
    const std::size_t context_diversity;
    const double regime_change_score;
    const std::uint64_t revision;
    const std::string hypothesis_id;
    const std::vector<std::string> evidence_addresses;

private:
    AccumulatorDecision(std::string status, std::string reason,
                        double posterior_mean, double causal_lower_bound,
                        double overall_upper_bound, double effective_sample_size,
                        std::size_t source_diversity,
                        std::size_t context_diversity,
                        double regime_change_score, std::uint64_t revision,
                        std::string hypothesis_id,
                        std::vector<std::string> evidence_addresses,
                        std::shared_ptr<const void> authority);
    std::shared_ptr<const void> authority_;

    friend std::shared_ptr<const AccumulatorDecision> assess_accumulator(
        const EvidenceAccumulatorState&, const EvidenceAccumulatorConfig&);
    friend bool is_authoritative_accumulator_decision(
        const AccumulatorDecision&) noexcept;
};

struct AccumulatorUpdate final {
    std::shared_ptr<const EvidenceAccumulatorState> state;
    std::shared_ptr<const AccumulatorDecision> previous_decision;
    std::shared_ptr<const AccumulatorDecision> decision;
    bool applied;
    std::string reason;
    std::string proposal_hash;
};

[[nodiscard]] std::pair<double, double> wilson_interval(
    double supports, double refutes, double confidence_level);
[[nodiscard]] std::shared_ptr<const AccumulatorDecision> assess_accumulator(
    const EvidenceAccumulatorState& state,
    const EvidenceAccumulatorConfig& config);
[[nodiscard]] AccumulatorUpdate update_accumulator(
    const std::shared_ptr<const EvidenceAccumulatorState>& state,
    const EvidenceObservation& observation,
    const EvidenceAccumulatorConfig& config, std::int64_t current_step);
[[nodiscard]] bool is_authoritative_accumulator_decision(
    const AccumulatorDecision& decision) noexcept;
void require_authoritative_accumulator_decision(
    const AccumulatorDecision& decision);

}  // namespace swegca::world
