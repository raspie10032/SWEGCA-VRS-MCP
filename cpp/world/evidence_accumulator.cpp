#include "world/evidence_accumulator.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace swegca::world {

class AccumulatorAccess final {
public:
    [[nodiscard]] static const std::shared_ptr<const void>& authority(
        const EvidenceAccumulatorState& value) noexcept {
        return value.authority_;
    }
};

namespace {

const std::shared_ptr<const void>& state_authority() {
    static const auto token = std::static_pointer_cast<const void>(
        std::make_shared<const int>(0));
    return token;
}

const std::shared_ptr<const void>& decision_authority() {
    static const auto token = std::static_pointer_cast<const void>(
        std::make_shared<const int>(0));
    return token;
}

void require_authoritative_state(const EvidenceAccumulatorState& state) {
    if (AccumulatorAccess::authority(state) != state_authority()) {
        throw AuthorityError("accumulator state was not issued by this accumulator");
    }
}

template <class Value>
bool contains(const std::vector<Value>& values, const Value& wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

void insert_unique(std::vector<std::string>& values, const std::string& value) {
    if (!contains(values, value)) values.push_back(value);
}

std::string hex_digest(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        out[index * 2] = digits[value >> 4U];
        out[index * 2 + 1] = digits[value & 15U];
    }
    return out;
}

void append_hex_escape(std::string& out, const unsigned value) {
    constexpr char digits[] = "0123456789abcdef";
    out += "\\u";
    out.push_back(digits[(value >> 12U) & 15U]);
    out.push_back(digits[(value >> 8U) & 15U]);
    out.push_back(digits[(value >> 4U) & 15U]);
    out.push_back(digits[value & 15U]);
}

// Python json.dumps defaults to ensure_ascii=True. C++ strings are UTF-8 at
// this boundary, so encode Unicode scalar values with the same \u spelling.
std::string python_json_string(const std::string& value) {
    std::string out{"\""};
    for (std::size_t at = 0; at != value.size();) {
        const auto first = static_cast<unsigned char>(value[at]);
        if (first < 0x80U) {
            ++at;
            switch (first) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (first < 0x20U) append_hex_escape(out, first);
                else out.push_back(static_cast<char>(first));
            }
            continue;
        }
        unsigned code = 0;
        std::size_t count = 0;
        if ((first & 0xe0U) == 0xc0U) { code = first & 0x1fU; count = 2; }
        else if ((first & 0xf0U) == 0xe0U) { code = first & 0x0fU; count = 3; }
        else if ((first & 0xf8U) == 0xf0U) { code = first & 0x07U; count = 4; }
        else { append_hex_escape(out, first); ++at; continue; }
        if (at + count > value.size()) {
            append_hex_escape(out, first); ++at; continue;
        }
        bool valid = true;
        for (std::size_t index = 1; index != count; ++index) {
            const auto next = static_cast<unsigned char>(value[at + index]);
            if ((next & 0xc0U) != 0x80U) { valid = false; break; }
            code = (code << 6U) | (next & 0x3fU);
        }
        const unsigned minimum = count == 2 ? 0x80U : count == 3 ? 0x800U : 0x10000U;
        if (!valid || code < minimum || code > 0x10ffffU ||
            (code >= 0xd800U && code <= 0xdfffU)) {
            append_hex_escape(out, first); ++at; continue;
        }
        at += count;
        if (code <= 0xffffU) append_hex_escape(out, code);
        else {
            code -= 0x10000U;
            append_hex_escape(out, 0xd800U + (code >> 10U));
            append_hex_escape(out, 0xdc00U + (code & 0x3ffU));
        }
    }
    out.push_back('"');
    return out;
}

std::string python_json_float(const double value) {
    char buffer[64]{};
    const auto [end, error] = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (error != std::errc{}) throw std::runtime_error("producer confidence formatting failed");
    std::string out(buffer, end);
    if (out.find_first_of(".eE") == std::string::npos) out += ".0";
    return out;
}

bool all_python_whitespace(const std::string& text) {
    for (std::size_t at = 0; at != text.size();) {
        const auto first = static_cast<unsigned char>(text[at]);
        unsigned code = first;
        std::size_t count = 1;
        if (first >= 0x80U) {
            if ((first & 0xe0U) == 0xc0U) { code = first & 0x1fU; count = 2; }
            else if ((first & 0xf0U) == 0xe0U) { code = first & 0x0fU; count = 3; }
            else if ((first & 0xf8U) == 0xf0U) { code = first & 0x07U; count = 4; }
            else return false;
            if (at + count > text.size()) return false;
            for (std::size_t index = 1; index != count; ++index) {
                const auto next = static_cast<unsigned char>(text[at + index]);
                if ((next & 0xc0U) != 0x80U) return false;
                code = (code << 6U) | (next & 0x3fU);
            }
        }
        at += count;
        const bool whitespace =
            (code >= 0x09U && code <= 0x0dU) ||
            (code >= 0x1cU && code <= 0x20U) || code == 0x85U ||
            code == 0xa0U || code == 0x1680U ||
            (code >= 0x2000U && code <= 0x200aU) ||
            code == 0x2028U || code == 0x2029U || code == 0x202fU ||
            code == 0x205fU || code == 0x3000U;
        if (!whitespace) return false;
    }
    return true;
}

double inverse_normal_cdf(const double probability) {
    // Wichura AS241, in the same expression order and binary64 constants used
    // by statistics.NormalDist.inv_cdf in the pinned Python source.
    const double q = probability - 0.5;
    if (std::abs(q) <= 0.425) {
        const double r = 0.180625 - q * q;
        const double numerator =
            (((((((2.5090809287301226727e+3 * r + 3.3430575583588128105e+4) * r +
                  6.7265770927008700853e+4) * r + 4.5921953931549871457e+4) * r +
                1.3731693765509461125e+4) * r + 1.9715909503065514427e+3) * r +
              1.3314166789178437745e+2) * r + 3.3871328727963666080) * q;
        const double denominator =
            (((((((5.2264952788528545610e+3 * r + 2.8729085735721942674e+4) * r +
                  3.9307895800092710610e+4) * r + 2.1213794301586595867e+4) * r +
                5.3941960214247511077e+3) * r + 6.8718700749205790830e+2) * r +
              4.2313330701600911252e+1) * r + 1.0);
        return numerator / denominator;
    }
    double r = probability;
    if (q > 0.0) r = 1.0 - probability;
    r = std::sqrt(-std::log(r));
    double numerator = 0.0;
    double denominator = 0.0;
    if (r <= 5.0) {
        r -= 1.6;
        numerator =
            (((((((7.74545014278341407640e-4 * r + 2.27238449892691845833e-2) * r +
                  2.41780725177450611770e-1) * r + 1.27045825245236838258) * r +
                3.64784832476320460504) * r + 5.76949722146069140550) * r +
              4.63033784615654529590) * r + 1.42343711074968357734);
        denominator =
            (((((((1.05075007164441684324e-9 * r + 5.47593808499534494600e-4) * r +
                  1.51986665636164571966e-2) * r + 1.48103976427480074590e-1) * r +
                6.89767334985100004550e-1) * r + 1.67638483018380384940) * r +
              2.05319162663775882187) * r + 1.0);
    } else {
        r -= 5.0;
        numerator =
            (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) * r +
                  1.24266094738807843860e-3) * r + 2.65321895265761230930e-2) * r +
                2.96560571828504891230e-1) * r + 1.78482653991729133580) * r +
              5.46378491116411436990) * r + 6.65790464350110377720);
        denominator =
            (((((((2.04426310338993978564e-15 * r + 1.42151175831644588870e-7) * r +
                  1.84631831751005468180e-5) * r + 7.86869131145613259100e-4) * r +
                1.48753612908506148525e-2) * r + 1.36929880922735805310e-1) * r +
              5.99832206555887937690e-1) * r + 1.0);
    }
    double result = numerator / denominator;
    if (q < 0.0) result = -result;
    return result;
}

std::vector<EvidenceAxisState> initial_axes(const EvidenceAccumulatorConfig& config) {
    std::vector<EvidenceAxisState> axes;
    axes.reserve(config.required_axes.size());
    for (const auto& name : config.required_axes) axes.push_back({name, {}});
    return axes;
}

}  // namespace

EvidenceAccumulatorConfig::EvidenceAccumulatorConfig(
    const double chance_rate_value, const double accept_margin_value,
    const double confidence_level_value, const double beta_prior_alpha_value,
    const double beta_prior_beta_value,
    const std::uint64_t minimum_effective_samples_per_axis_value,
    const std::uint64_t minimum_source_diversity_value,
    const std::uint64_t minimum_source_diversity_per_axis_value,
    const std::uint64_t minimum_context_diversity_value,
    const std::uint64_t recent_window_value,
    const std::uint64_t minimum_recent_samples_value,
    const double regime_change_threshold_value,
    std::vector<std::string> required_axes_value)
    : chance_rate(chance_rate_value), accept_margin(accept_margin_value),
      confidence_level(confidence_level_value),
      beta_prior_alpha(beta_prior_alpha_value), beta_prior_beta(beta_prior_beta_value),
      minimum_effective_samples_per_axis(minimum_effective_samples_per_axis_value),
      minimum_source_diversity(minimum_source_diversity_value),
      minimum_source_diversity_per_axis(minimum_source_diversity_per_axis_value),
      minimum_context_diversity(minimum_context_diversity_value),
      recent_window(recent_window_value), minimum_recent_samples(minimum_recent_samples_value),
      regime_change_threshold(regime_change_threshold_value),
      required_axes(std::move(required_axes_value)) {
    if (!(chance_rate >= 0.0 && chance_rate < 1.0))
        throw std::invalid_argument("chance rate must be in [0, 1)");
    if (!(accept_margin >= 0.0 && accept_margin <= 1.0 - chance_rate))
        throw std::invalid_argument("accept margin is outside the probability range");
    if (!(confidence_level > 0.0 && confidence_level < 1.0))
        throw std::invalid_argument("confidence level must be in (0, 1)");
    if (std::min(beta_prior_alpha, beta_prior_beta) <= 0.0)
        throw std::invalid_argument("Beta prior values must be positive");
    if (std::min({minimum_effective_samples_per_axis, minimum_source_diversity,
                  minimum_source_diversity_per_axis, minimum_context_diversity,
                  recent_window, minimum_recent_samples}) == 0)
        throw std::invalid_argument("sample, diversity, and window limits must be positive");
    if (minimum_recent_samples > recent_window)
        throw std::invalid_argument("minimum recent samples exceed the recent window");
    if (!(regime_change_threshold >= 0.0 && regime_change_threshold <= 1.0))
        throw std::invalid_argument("regime threshold must be in [0, 1]");
    std::set<std::string> unique(required_axes.begin(), required_axes.end());
    if (required_axes.empty() || unique.size() != required_axes.size())
        throw std::invalid_argument("required evidence axes must be unique and nonempty");
}

EvidenceObservation::EvidenceObservation(
    std::string hypothesis_id_value, std::string evidence_address_value,
    std::string source_family_value, std::string context_hash_value,
    std::string axis_value, std::string outcome_value,
    const std::int64_t observed_at_value,
    const std::optional<std::int64_t> expires_at_value,
    std::string producer_id_value, const double producer_confidence_value,
    std::string source_address_value, std::string source_revision_value)
    : hypothesis_id(std::move(hypothesis_id_value)),
      evidence_address(std::move(evidence_address_value)),
      source_family(std::move(source_family_value)),
      context_hash(std::move(context_hash_value)), axis(std::move(axis_value)),
      outcome(std::move(outcome_value)), observed_at(observed_at_value),
      expires_at(expires_at_value), producer_id(std::move(producer_id_value)),
      producer_confidence(producer_confidence_value),
      source_address(std::move(source_address_value)),
      source_revision(std::move(source_revision_value)) {}

void EvidenceObservation::validate() const {
    if (hypothesis_id.empty()) throw std::invalid_argument("hypothesis_id must be nonempty");
    if (evidence_address.empty()) throw std::invalid_argument("evidence_address must be nonempty");
    if (source_family.empty()) throw std::invalid_argument("source_family must be nonempty");
    if (context_hash.empty()) throw std::invalid_argument("context_hash must be nonempty");
    if (axis.empty()) throw std::invalid_argument("axis must be nonempty");
    if (producer_id.empty()) throw std::invalid_argument("producer_id must be nonempty");
    if (outcome != "support" && outcome != "refute" && outcome != "insufficient")
        throw std::invalid_argument("unsupported evidence outcome");
    if (observed_at < 0) throw std::invalid_argument("observed_at must be nonnegative");
    if (expires_at && *expires_at < observed_at)
        throw std::invalid_argument("expires_at precedes the observation");
    if (!std::isfinite(producer_confidence) || producer_confidence < 0.0 ||
        producer_confidence > 1.0)
        throw std::invalid_argument("producer confidence must be finite and in [0, 1]");
    if (source_address.empty() != source_revision.empty())
        throw std::invalid_argument("source address and revision must be provided together");
    if (!source_address.empty() &&
        (all_python_whitespace(source_address) || all_python_whitespace(source_revision)))
        throw std::invalid_argument("source address and revision must be nonempty text");
}

std::string EvidenceObservation::proposal_hash() const {
    std::string payload;
    payload += "{\"axis\":" + python_json_string(axis);
    payload += ",\"context_hash\":" + python_json_string(context_hash);
    payload += ",\"evidence_address\":" + python_json_string(evidence_address);
    payload += ",\"expires_at\":" +
               (expires_at ? std::to_string(*expires_at) : std::string("null"));
    payload += ",\"hypothesis_id\":" + python_json_string(hypothesis_id);
    payload += ",\"observed_at\":" + std::to_string(observed_at);
    payload += ",\"outcome\":" + python_json_string(outcome);
    payload += ",\"producer_confidence\":" + python_json_float(producer_confidence);
    payload += ",\"producer_id\":" + python_json_string(producer_id);
    payload += ",\"source_address\":" + python_json_string(source_address);
    payload += ",\"source_family\":" + python_json_string(source_family);
    payload += ",\"source_revision\":" + python_json_string(source_revision) + "}";
    architecture::Sha256 digest;
    digest.update(payload);
    return hex_digest(digest.finish());
}

double EvidenceGroup::effective_support() const noexcept {
    const auto total = supports + refutes;
    return total == 0 ? 0.0 : static_cast<double>(supports) / static_cast<double>(total);
}

double EvidenceGroup::effective_refute() const noexcept {
    const auto total = supports + refutes;
    return total == 0 ? 0.0 : static_cast<double>(refutes) / static_cast<double>(total);
}

double EvidenceAxisState::effective_support() const noexcept {
    double total = 0.0;
    for (const auto& group : groups) total += group.effective_support();
    return total;
}

double EvidenceAxisState::effective_refute() const noexcept {
    double total = 0.0;
    for (const auto& group : groups) total += group.effective_refute();
    return total;
}

double EvidenceAxisState::effective_samples() const noexcept {
    return effective_support() + effective_refute();
}

std::size_t EvidenceAxisState::source_diversity() const {
    std::set<std::string> sources;
    std::set<std::string> producers;
    for (const auto& group : groups) {
        sources.insert(group.source_family);
        producers.insert(group.producer_ids.begin(), group.producer_ids.end());
    }
    return std::min(sources.size(), producers.size());
}

EvidenceAccumulatorState::EvidenceAccumulatorState(
    std::string hypothesis_id, const EvidenceAccumulatorConfig& config)
    : hypothesis_id_(std::move(hypothesis_id)), axes_(initial_axes(config)) {
    if (hypothesis_id_.empty()) throw std::invalid_argument("hypothesis id must be nonempty");
}

EvidenceAccumulatorState::EvidenceAccumulatorState(
    std::string hypothesis_id, std::vector<EvidenceAxisState> axes,
    std::vector<std::string> seen_addresses,
    std::vector<std::string> source_families,
    std::vector<std::string> context_hashes,
    std::vector<std::string> producer_ids,
    std::vector<int> recent_outcomes, const std::uint64_t revision,
    std::shared_ptr<const void> authority)
    : hypothesis_id_(std::move(hypothesis_id)), axes_(std::move(axes)),
      seen_addresses_(std::move(seen_addresses)),
      source_families_(std::move(source_families)),
      context_hashes_(std::move(context_hashes)),
      producer_ids_(std::move(producer_ids)),
      recent_outcomes_(std::move(recent_outcomes)), revision_(revision),
      authority_(std::move(authority)) {}

std::shared_ptr<const EvidenceAccumulatorState> EvidenceAccumulatorState::empty(
    std::string hypothesis_id, const EvidenceAccumulatorConfig& config) {
    if (hypothesis_id.empty()) throw std::invalid_argument("hypothesis id must be nonempty");
    return std::shared_ptr<const EvidenceAccumulatorState>(new EvidenceAccumulatorState(
        std::move(hypothesis_id), initial_axes(config), {}, {}, {}, {}, {}, 0,
        state_authority()));
}

const std::string& EvidenceAccumulatorState::hypothesis_id() const noexcept { return hypothesis_id_; }
std::span<const EvidenceAxisState> EvidenceAccumulatorState::axes() const noexcept { return axes_; }
std::span<const std::string> EvidenceAccumulatorState::seen_addresses() const noexcept { return seen_addresses_; }
std::span<const std::string> EvidenceAccumulatorState::source_families() const noexcept { return source_families_; }
std::span<const std::string> EvidenceAccumulatorState::context_hashes() const noexcept { return context_hashes_; }
std::span<const std::string> EvidenceAccumulatorState::producer_ids() const noexcept { return producer_ids_; }
std::span<const int> EvidenceAccumulatorState::recent_outcomes() const noexcept { return recent_outcomes_; }
std::uint64_t EvidenceAccumulatorState::revision() const noexcept { return revision_; }

AccumulatorDecision::AccumulatorDecision(
    std::string status_value, std::string reason_value,
    const double posterior_mean_value, const double causal_lower_bound_value,
    const double overall_upper_bound_value, const double effective_sample_size_value,
    const std::size_t source_diversity_value,
    const std::size_t context_diversity_value,
    const double regime_change_score_value, const std::uint64_t revision_value,
    std::string hypothesis_id_value, std::vector<std::string> evidence_addresses_value)
    : AccumulatorDecision(std::move(status_value), std::move(reason_value),
          posterior_mean_value, causal_lower_bound_value, overall_upper_bound_value,
          effective_sample_size_value, source_diversity_value, context_diversity_value,
          regime_change_score_value, revision_value, std::move(hypothesis_id_value),
          std::move(evidence_addresses_value), {}) {}

AccumulatorDecision::AccumulatorDecision(
    std::string status_value, std::string reason_value,
    const double posterior_mean_value, const double causal_lower_bound_value,
    const double overall_upper_bound_value, const double effective_sample_size_value,
    const std::size_t source_diversity_value,
    const std::size_t context_diversity_value,
    const double regime_change_score_value, const std::uint64_t revision_value,
    std::string hypothesis_id_value, std::vector<std::string> evidence_addresses_value,
    std::shared_ptr<const void> authority)
    : status(std::move(status_value)), reason(std::move(reason_value)),
      posterior_mean(posterior_mean_value), causal_lower_bound(causal_lower_bound_value),
      overall_upper_bound(overall_upper_bound_value),
      effective_sample_size(effective_sample_size_value),
      source_diversity(source_diversity_value), context_diversity(context_diversity_value),
      regime_change_score(regime_change_score_value), revision(revision_value),
      hypothesis_id(std::move(hypothesis_id_value)),
      evidence_addresses(std::move(evidence_addresses_value)),
      authority_(std::move(authority)) {}

std::pair<double, double> wilson_interval(
    const double supports, const double refutes, const double confidence_level) {
    const double samples = supports + refutes;
    if (samples <= 0.0) return {0.0, 1.0};
    const double probability = supports / samples;
    const double z_score = inverse_normal_cdf(0.5 + confidence_level / 2.0);
    const double z_squared = z_score * z_score;
    const double denominator = 1.0 + z_squared / samples;
    const double center = (probability + z_squared / (2.0 * samples)) / denominator;
    const double radius = z_score * std::sqrt(
        probability * (1.0 - probability) / samples +
        z_squared / (4.0 * samples * samples)) / denominator;
    return {std::max(0.0, center - radius), std::min(1.0, center + radius)};
}

std::shared_ptr<const AccumulatorDecision> assess_accumulator(
    const EvidenceAccumulatorState& state,
    const EvidenceAccumulatorConfig& config) {
    require_authoritative_state(state);
    std::vector<const EvidenceAxisState*> required;
    required.reserve(config.required_axes.size());
    for (const auto& name : config.required_axes) {
        const auto found = std::find_if(state.axes_.begin(), state.axes_.end(),
            [&](const EvidenceAxisState& axis) { return axis.name == name; });
        if (found == state.axes_.end()) throw std::out_of_range(name);
        required.push_back(&*found);
    }
    double supports = 0.0;
    double refutes = 0.0;
    for (const auto* axis : required) {
        supports += axis->effective_support();
        refutes += axis->effective_refute();
    }
    const double samples = supports + refutes;
    const double posterior_mean = (supports + config.beta_prior_alpha) /
        (samples + config.beta_prior_alpha + config.beta_prior_beta);
    double causal_lower_bound = 1.0;
    for (const auto* axis : required) {
        causal_lower_bound = std::min(causal_lower_bound,
            wilson_interval(axis->effective_support(), axis->effective_refute(),
                            config.confidence_level).first);
    }
    const double overall_upper_bound =
        wilson_interval(supports, refutes, config.confidence_level).second;
    double regime_change_score = 0.0;
    if (state.recent_outcomes_.size() >= config.minimum_recent_samples) {
        double recent_sum = 0.0;
        for (const int outcome : state.recent_outcomes_) recent_sum += outcome;
        regime_change_score = std::abs(
            recent_sum / static_cast<double>(state.recent_outcomes_.size()) - posterior_mean);
    }
    const double threshold = config.chance_rate + config.accept_margin;
    const std::size_t source_diversity =
        std::min(state.source_families_.size(), state.producer_ids_.size());
    const std::size_t context_diversity =
        std::min(state.context_hashes_.size(), state.producer_ids_.size());
    std::string status;
    std::string reason;
    if (std::any_of(required.begin(), required.end(), [&](const auto* axis) {
            return axis->effective_samples() <
                   static_cast<double>(config.minimum_effective_samples_per_axis);
        })) {
        status = "abstain"; reason = "minimum_effective_samples";
    } else if (source_diversity < config.minimum_source_diversity) {
        status = "abstain"; reason = "source_diversity";
    } else if (std::any_of(required.begin(), required.end(), [&](const auto* axis) {
            return axis->source_diversity() < config.minimum_source_diversity_per_axis;
        })) {
        status = "abstain"; reason = "axis_source_diversity";
    } else if (context_diversity < config.minimum_context_diversity) {
        status = "abstain"; reason = "context_diversity";
    } else if (regime_change_score >= config.regime_change_threshold) {
        status = "abstain"; reason = "regime_change_suspected";
    } else if (causal_lower_bound > threshold) {
        status = "accept"; reason = "causal_lower_bound";
    } else if (overall_upper_bound <= threshold) {
        status = "reject"; reason = "upper_bound_below_threshold";
    } else {
        status = "abstain"; reason = "uncertain";
    }
    auto addresses = state.seen_addresses_;
    std::sort(addresses.begin(), addresses.end());
    return std::shared_ptr<const AccumulatorDecision>(new AccumulatorDecision(
        std::move(status), std::move(reason), posterior_mean, causal_lower_bound,
        overall_upper_bound, samples, source_diversity, context_diversity,
        regime_change_score, state.revision_, state.hypothesis_id_, std::move(addresses),
        decision_authority()));
}

AccumulatorUpdate update_accumulator(
    const std::shared_ptr<const EvidenceAccumulatorState>& state,
    const EvidenceObservation& observation,
    const EvidenceAccumulatorConfig& config, const std::int64_t current_step) {
    if (!state) throw std::invalid_argument("accumulator state is null");
    require_authoritative_state(*state);
    observation.validate();
    if (observation.hypothesis_id != state->hypothesis_id_)
        throw std::invalid_argument("observation and accumulator hypothesis differ");
    const std::string hash = observation.proposal_hash();
    const auto previous = assess_accumulator(*state, config);
    if (!contains(config.required_axes, observation.axis))
        throw std::invalid_argument("observation axis is not registered");
    const auto unchanged = [&](std::string reason) {
        return AccumulatorUpdate{state, previous, previous, false, std::move(reason), hash};
    };
    if (observation.expires_at && current_step > *observation.expires_at)
        return unchanged("expired");
    if (observation.outcome == "insufficient") return unchanged("insufficient");
    if (contains(state->seen_addresses_, observation.evidence_address))
        return unchanged("duplicate");

    auto axes = state->axes_;
    for (auto& axis : axes) {
        if (axis.name != observation.axis) continue;
        const auto group = std::find_if(axis.groups.begin(), axis.groups.end(),
            [&](const EvidenceGroup& candidate) {
                return candidate.source_family == observation.source_family &&
                       candidate.context_hash == observation.context_hash;
            });
        if (group == axis.groups.end()) {
            axis.groups.push_back({observation.source_family, observation.context_hash,
                static_cast<std::uint64_t>(observation.outcome == "support"),
                static_cast<std::uint64_t>(observation.outcome == "refute"),
                {observation.producer_id}});
        } else {
            group->supports += static_cast<std::uint64_t>(observation.outcome == "support");
            group->refutes += static_cast<std::uint64_t>(observation.outcome == "refute");
            insert_unique(group->producer_ids, observation.producer_id);
        }
        break;
    }
    auto recent = state->recent_outcomes_;
    recent.push_back(observation.outcome == "support" ? 1 : 0);
    if (recent.size() > config.recent_window)
        recent.erase(recent.begin(), recent.end() -
            static_cast<std::ptrdiff_t>(config.recent_window));
    auto addresses = state->seen_addresses_;
    auto sources = state->source_families_;
    auto contexts = state->context_hashes_;
    auto producers = state->producer_ids_;
    insert_unique(addresses, observation.evidence_address);
    insert_unique(sources, observation.source_family);
    insert_unique(contexts, observation.context_hash);
    insert_unique(producers, observation.producer_id);
    const auto updated = std::shared_ptr<const EvidenceAccumulatorState>(
        new EvidenceAccumulatorState(state->hypothesis_id_, std::move(axes),
            std::move(addresses), std::move(sources), std::move(contexts),
            std::move(producers), std::move(recent), state->revision_ + 1,
            state_authority()));
    return {updated, previous, assess_accumulator(*updated, config), true, "applied", hash};
}

bool is_authoritative_accumulator_decision(
    const AccumulatorDecision& decision) noexcept {
    return decision.authority_ == decision_authority();
}

void require_authoritative_accumulator_decision(
    const AccumulatorDecision& decision) {
    if (!is_authoritative_accumulator_decision(decision))
        throw AuthorityError("accumulator decision is not an authority capability");
}

}  // namespace swegca::world
