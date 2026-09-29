#include "world/counterfactual_replay.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

std::string json_string(const std::string_view value) {
    std::string result{"\""};
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '\b': result += "\\b"; break;
        case '\t': result += "\\t"; break;
        case '\n': result += "\\n"; break;
        case '\f': result += "\\f"; break;
        case '\r': result += "\\r"; break;
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        default:
            if (byte < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[byte >> 4U]);
                result.push_back(digits[byte & 15U]);
            } else result.push_back(raw);
        }
    }
    result.push_back('"');
    return result;
}

std::string python_float(const double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
    char storage[64]{};
    const auto [end, error] = std::to_chars(
        std::begin(storage), std::end(storage), value, std::chars_format::general);
    if (error != std::errc{}) throw std::runtime_error("cannot encode replay number");
    std::string result(storage, end);
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result;
}

std::string json_strings(const std::span<const std::string> values) {
    std::string result{"["};
    for (std::size_t index = 0; index != values.size(); ++index) {
        if (index != 0) result.push_back(',');
        result += json_string(values[index]);
    }
    result.push_back(']');
    return result;
}

std::string digest(const std::string_view value) {
    architecture::Sha256 sha;
    sha.update(value);
    constexpr char digits[] = "0123456789abcdef";
    const auto bytes = sha.finish();
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        const auto byte = std::to_integer<unsigned>(bytes[index]);
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 15U];
    }
    return result;
}

std::string config_json(const EvidenceAccumulatorConfig& config) {
    return "{\"accept_margin\":" + python_float(config.accept_margin) +
        ",\"beta_prior_alpha\":" + python_float(config.beta_prior_alpha) +
        ",\"beta_prior_beta\":" + python_float(config.beta_prior_beta) +
        ",\"chance_rate\":" + python_float(config.chance_rate) +
        ",\"confidence_level\":" + python_float(config.confidence_level) +
        ",\"minimum_context_diversity\":" +
            std::to_string(config.minimum_context_diversity) +
        ",\"minimum_effective_samples_per_axis\":" +
            std::to_string(config.minimum_effective_samples_per_axis) +
        ",\"minimum_recent_samples\":" +
            std::to_string(config.minimum_recent_samples) +
        ",\"minimum_source_diversity\":" +
            std::to_string(config.minimum_source_diversity) +
        ",\"minimum_source_diversity_per_axis\":" +
            std::to_string(config.minimum_source_diversity_per_axis) +
        ",\"recent_window\":" + std::to_string(config.recent_window) +
        ",\"regime_change_threshold\":" +
            python_float(config.regime_change_threshold) +
        ",\"required_axes\":" + json_strings(config.required_axes) + "}";
}

std::string observation_json(const EvidenceObservation& item) {
    return "{\"axis\":" + json_string(item.axis) +
        ",\"context_hash\":" + json_string(item.context_hash) +
        ",\"evidence_address\":" + json_string(item.evidence_address) +
        ",\"expires_at\":" +
            (item.expires_at ? std::to_string(*item.expires_at) : "null") +
        ",\"hypothesis_id\":" + json_string(item.hypothesis_id) +
        ",\"observed_at\":" + std::to_string(item.observed_at) +
        ",\"outcome\":" + json_string(item.outcome) +
        ",\"producer_confidence\":" + python_float(item.producer_confidence) +
        ",\"producer_id\":" + json_string(item.producer_id) +
        ",\"source_address\":" + json_string(item.source_address) +
        ",\"source_family\":" + json_string(item.source_family) +
        ",\"source_revision\":" + json_string(item.source_revision) + "}";
}

std::string cache_hash(
    const std::string_view hypothesis_id,
    const EvidenceAccumulatorConfig& config,
    const std::span<const EvidenceObservation> observations) {
    std::string encoded = "{\"config\":" + config_json(config) +
        ",\"hypothesis_id\":" + json_string(hypothesis_id) +
        ",\"observations\":[";
    for (std::size_t index = 0; index != observations.size(); ++index) {
        if (index != 0) encoded.push_back(',');
        encoded += observation_json(observations[index]);
    }
    encoded += "]}";
    return digest(encoded);
}

bool contains(const std::span<const std::string> values, const std::string_view target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

struct GroupCount final {
    double support = 0.0;
    double refute = 0.0;
};

ReplayDecision evaluate_one(
    const CompiledEvidenceReplay& cache,
    const ReplayIntervention& intervention,
    const EvidenceAccumulatorConfig& config) {
    const auto axis_count = config.required_axes.size();
    std::map<std::string, std::size_t, std::less<>> axis_lookup;
    for (std::size_t index = 0; index != axis_count; ++index) {
        axis_lookup.emplace(config.required_axes[index], index);
    }
    std::map<std::tuple<std::size_t, std::string, std::string>, GroupCount> groups;
    std::set<std::string> sources;
    std::set<std::string> contexts;
    std::vector<std::set<std::string>> axis_sources(axis_count);
    std::vector<int> active_outcomes;
    for (const auto& item : cache.observations) {
        if (contains(intervention.drop_axes, item.axis) ||
            contains(intervention.drop_evidence_addresses, item.evidence_address)) continue;
        const auto axis = axis_lookup.at(item.axis);
        auto outcome = item.outcome == "support" ? 1 : 0;
        if (contains(intervention.flip_axes, item.axis)) outcome = 1 - outcome;
        auto& group = groups[{axis, item.source_family, item.context_hash}];
        if (outcome == 1) group.support += 1.0;
        else group.refute += 1.0;
        sources.insert(item.source_family);
        contexts.insert(item.context_hash);
        axis_sources[axis].insert(item.source_family);
        active_outcomes.push_back(outcome);
    }

    std::vector<double> axis_support(axis_count, 0.0);
    std::vector<double> axis_refute(axis_count, 0.0);
    for (const auto& [key, count] : groups) {
        const auto total = count.support + count.refute;
        if (total == 0.0) continue;
        const auto axis = std::get<0>(key);
        axis_support[axis] += count.support / total;
        axis_refute[axis] += count.refute / total;
    }
    double supports = 0.0;
    double refutes = 0.0;
    double causal_lower = 1.0;
    bool minimum_samples_failed = false;
    bool axis_diversity_failed = false;
    for (std::size_t axis = 0; axis != axis_count; ++axis) {
        const auto samples = axis_support[axis] + axis_refute[axis];
        supports += axis_support[axis];
        refutes += axis_refute[axis];
        causal_lower = std::min(
            causal_lower,
            wilson_interval(axis_support[axis], axis_refute[axis],
                            config.confidence_level).first);
        minimum_samples_failed = minimum_samples_failed ||
            samples < static_cast<double>(config.minimum_effective_samples_per_axis);
        axis_diversity_failed = axis_diversity_failed ||
            axis_sources[axis].size() < config.minimum_source_diversity_per_axis;
    }
    const auto samples = supports + refutes;
    const auto posterior = (supports + config.beta_prior_alpha) /
        (samples + config.beta_prior_alpha + config.beta_prior_beta);
    const auto overall_upper = wilson_interval(
        supports, refutes, config.confidence_level).second;
    double regime_change = 0.0;
    const auto recent_count = std::min<std::size_t>(
        static_cast<std::size_t>(config.recent_window), cache.observation_count());
    const auto valid_count = std::min(recent_count, active_outcomes.size());
    if (valid_count >= config.minimum_recent_samples) {
        double recent_support = 0.0;
        for (std::size_t index = active_outcomes.size() - valid_count;
             index != active_outcomes.size(); ++index) {
            recent_support += active_outcomes[index];
        }
        regime_change = std::abs(recent_support / valid_count - posterior);
    }

    std::string status = "abstain";
    std::string reason = "uncertain";
    const auto threshold = config.chance_rate + config.accept_margin;
    if (minimum_samples_failed) reason = "minimum_effective_samples";
    else if (sources.size() < config.minimum_source_diversity) reason = "source_diversity";
    else if (axis_diversity_failed) reason = "axis_source_diversity";
    else if (contexts.size() < config.minimum_context_diversity) reason = "context_diversity";
    else if (regime_change >= config.regime_change_threshold) {
        reason = "regime_change_suspected";
    } else if (causal_lower > threshold) {
        status = "accept";
        reason = "causal_lower_bound";
    } else if (overall_upper <= threshold) {
        status = "reject";
        reason = "upper_bound_below_threshold";
    }
    return {intervention.name, std::move(status), std::move(reason), posterior,
            causal_lower, overall_upper, samples, sources.size(), contexts.size(),
            regime_change};
}

}  // namespace

CompiledEvidenceReplay compile_evidence_replay(
    const std::span<const EvidenceObservation> observations,
    const EvidenceAccumulatorConfig& config,
    std::string hypothesis_id,
    const std::int64_t current_step) {
    if (hypothesis_id.empty()) throw std::invalid_argument("hypothesis id must be nonempty");
    if (current_step < 0) throw std::invalid_argument("current step must be nonnegative");
    std::set<std::string> seen;
    std::vector<EvidenceObservation> accepted;
    ReplayCompileStats stats;
    for (const auto& item : observations) {
        item.validate();
        if (item.hypothesis_id != hypothesis_id) {
            ++stats.other_hypothesis;
            continue;
        }
        if (!contains(config.required_axes, item.axis)) {
            throw std::invalid_argument("observation axis is not registered");
        }
        if (item.expires_at && current_step > *item.expires_at) {
            ++stats.expired;
            continue;
        }
        if (item.outcome == "insufficient") {
            ++stats.insufficient;
            continue;
        }
        if (!seen.insert(item.evidence_address).second) {
            ++stats.duplicate;
            continue;
        }
        accepted.push_back(item);
        ++stats.applied;
    }
    std::vector<std::string> addresses;
    addresses.reserve(accepted.size());
    for (const auto& item : accepted) addresses.push_back(item.evidence_address);
    auto compiled_hash = cache_hash(hypothesis_id, config, observations);
    return {std::move(hypothesis_id),
            std::move(compiled_hash),
            config.required_axes, std::move(addresses), std::move(accepted),
            stats, current_step};
}

CompiledReplayInterventions compile_replay_interventions(
    const CompiledEvidenceReplay& cache,
    const std::span<const ReplayIntervention> interventions) {
    if (interventions.empty()) {
        throw std::invalid_argument("at least one replay intervention is required");
    }
    std::set<std::string> names;
    std::set<std::string> axes(cache.axes.begin(), cache.axes.end());
    std::set<std::string> addresses(
        cache.evidence_addresses.begin(), cache.evidence_addresses.end());
    std::vector<std::string> ordered_names;
    std::vector<ReplayIntervention> variants;
    for (const auto& item : interventions) {
        if (item.name.empty() || !names.insert(item.name).second) {
            throw std::invalid_argument("intervention names must be unique and nonempty");
        }
        std::set<std::string> dropped(item.drop_axes.begin(), item.drop_axes.end());
        std::set<std::string> flipped(item.flip_axes.begin(), item.flip_axes.end());
        for (const auto& axis : dropped) {
            if (!axes.contains(axis)) throw std::invalid_argument("unknown intervention axes");
            if (flipped.contains(axis)) {
                throw std::invalid_argument("an axis cannot be dropped and flipped");
            }
        }
        for (const auto& axis : flipped) {
            if (!axes.contains(axis)) throw std::invalid_argument("unknown intervention axes");
        }
        for (const auto& address : item.drop_evidence_addresses) {
            if (!addresses.contains(address)) {
                throw std::invalid_argument("unknown evidence addresses");
            }
        }
        ordered_names.push_back(item.name);
        variants.push_back(item);
    }
    return {std::move(ordered_names), std::move(variants)};
}

ReplayBatchResult evaluate_compiled_counterfactual_replays(
    const CompiledEvidenceReplay& cache,
    const CompiledReplayInterventions& interventions,
    const EvidenceAccumulatorConfig& config) {
    if (cache.axes != config.required_axes) {
        throw std::invalid_argument("cache and accumulator axes differ");
    }
    if (interventions.names.size() != interventions.variants.size() ||
        interventions.variants.empty()) {
        throw std::invalid_argument("invalid compiled replay interventions");
    }
    std::vector<ReplayDecision> decisions;
    decisions.reserve(interventions.variant_count());
    for (std::size_t index = 0; index != interventions.variant_count(); ++index) {
        if (interventions.names[index] != interventions.variants[index].name) {
            throw std::invalid_argument("compiled replay intervention names differ");
        }
        decisions.push_back(evaluate_one(cache, interventions.variants[index], config));
    }
    return {cache.cache_hash, std::move(decisions), cache.stats.applied};
}

ReplayBatchResult evaluate_counterfactual_replays(
    const CompiledEvidenceReplay& cache,
    const std::span<const ReplayIntervention> interventions,
    const EvidenceAccumulatorConfig& config) {
    const auto compiled = compile_replay_interventions(cache, interventions);
    return evaluate_compiled_counterfactual_replays(cache, compiled, config);
}

}  // namespace swegca::world
