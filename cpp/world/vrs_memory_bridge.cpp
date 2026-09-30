#include "world/vrs_memory_bridge.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"
#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>

namespace swegca::world {
namespace {

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

std::string digest(const std::string_view value) {
    return hex(architecture::Sha256::of(
        std::as_bytes(std::span(value.data(), value.size()))));
}

std::vector<std::string> unique(std::vector<std::string> values) {
    std::vector<std::string> result;
    for (auto& value : values)
        if (std::ranges::find(result, value) == result.end()) result.push_back(std::move(value));
    return result;
}

std::size_t parse_id(const std::string_view text, const std::string_view prefix) {
    if (!text.starts_with(prefix)) throw std::out_of_range(std::string(text));
    const auto suffix = text.substr(prefix.size());
    if (suffix.empty() || !std::ranges::all_of(suffix, [](const unsigned char c) {
            return c >= '0' && c <= '9';
        })) throw std::out_of_range(std::string(text));
    std::size_t value{};
    for (const unsigned char c : suffix) {
        if (value > (std::numeric_limits<std::size_t>::max() - (c - '0')) / 10)
            throw std::out_of_range(std::string(text));
        value = value * 10 + (c - '0');
    }
    return value;
}

MemoryStep step(std::string phase, JsonValue::Object observation,
                std::vector<std::string> relations, std::string judgment,
                std::string outcome, std::vector<std::string> refs) {
    return {std::move(phase), std::move(observation), std::move(relations),
            std::move(judgment), std::move(outcome), std::move(refs)};
}

std::map<std::string, std::vector<std::size_t>, std::less<>> term_cues(
    const std::vector<std::string>& terms) {
    std::map<std::string, std::vector<std::size_t>, std::less<>> result;
    for (std::size_t index = 0; index < terms.size(); ++index) {
        if (terms[index].empty()) throw std::invalid_argument("VRS query memory term must not be empty");
        result[canonical_vrs_cue(terms[index])].push_back(index);
    }
    return result;
}

std::shared_ptr<const VrsHotAddressIndex> address_index(
    const std::vector<std::string>& terms,
    const std::vector<std::uint32_t>& source,
    const std::vector<std::uint32_t>& target,
    const std::vector<VrsEvidenceRequest>& requests) {
    std::vector<std::vector<std::size_t>> incident(terms.size());
    for (std::size_t edge = 0; edge < source.size(); ++edge) {
        incident[source[edge]].push_back(edge);
        if (target[edge] != source[edge]) incident[target[edge]].push_back(edge);
    }
    std::vector<std::size_t> offsets(terms.size() + 1), edges;
    for (std::size_t term = 0; term < terms.size(); ++term) {
        offsets[term] = edges.size();
        edges.insert(edges.end(), incident[term].begin(), incident[term].end());
    }
    offsets.back() = edges.size();
    std::map<std::size_t, std::vector<std::size_t>> by_term;
    for (std::size_t index = 0; index < requests.size(); ++index)
        by_term[requests[index].term_id].push_back(index);
    return std::make_shared<const VrsHotAddressIndex>(
        term_cues(terms), std::move(offsets), std::move(edges),
        std::move(by_term), terms.size(), source.size());
}

std::vector<std::string> query_keys(const std::string_view query) {
    std::vector<std::string> result{canonical_vrs_cue(query)};
    std::string token;
    for (const unsigned char byte : query) {
        const bool separator = byte < 128 && !(std::isalnum(byte) || byte == '_' || byte == '-');
        if (separator) {
            if (!token.empty()) { result.push_back(canonical_vrs_cue(token)); token.clear(); }
        } else token.push_back(static_cast<char>(byte));
    }
    if (!token.empty()) result.push_back(canonical_vrs_cue(token));
    return unique(std::move(result));
}

JsonValue request_value(const VrsEvidenceRequest& request, const std::string_view key) {
    const auto found = request.fields.find(key);
    return found == request.fields.end() ? JsonValue(nullptr) : found->second;
}

}  // namespace

std::string canonical_vrs_cue(const std::string_view value) {
    std::string expanded(value);
    std::ranges::replace(expanded, '_', ' ');
    return unicode_casefold(collapse_unicode_whitespace(expanded));
}

VrsHotAddressIndex::VrsHotAddressIndex(
    std::map<std::string, std::vector<std::size_t>, std::less<>> cues,
    std::vector<std::size_t> offsets, std::vector<std::size_t> edges,
    std::map<std::size_t, std::vector<std::size_t>> requests,
    const std::size_t terms, const std::size_t edge_count_value)
    : term_ids_by_cue(std::move(cues)), edge_offsets(std::move(offsets)),
      edge_ids(std::move(edges)), request_ids_by_term(std::move(requests)),
      term_count(terms), edge_count(edge_count_value) {
    if (edge_offsets.size() != term_count + 1 || edge_offsets.front() != 0 ||
        edge_offsets.back() != edge_ids.size() ||
        !std::ranges::is_sorted(edge_offsets) ||
        std::ranges::any_of(edge_ids, [&](const auto id) { return id >= edge_count; }))
        throw std::invalid_argument("VRS hot address offsets changed");
}

std::vector<std::size_t> VrsHotAddressIndex::term_ids(const std::string_view cue) const {
    const auto found = term_ids_by_cue.find(canonical_vrs_cue(cue));
    return found == term_ids_by_cue.end() ? std::vector<std::size_t>{} : found->second;
}

std::span<const std::size_t> VrsHotAddressIndex::edge_ids_for_term(
    const std::size_t term_id) const {
    if (term_id >= term_count) throw std::out_of_range("term ID outside address index");
    return std::span(edge_ids).subspan(edge_offsets[term_id],
        edge_offsets[term_id + 1] - edge_offsets[term_id]);
}

VrsHotMemorySource::VrsHotMemorySource(
    std::vector<std::string> terms_value, std::vector<double> score_value,
    std::vector<std::int64_t> support_value, std::vector<std::int64_t> refute_value,
    std::vector<std::uint32_t> source_value, std::vector<std::uint32_t> target_value,
    std::vector<std::int8_t> sign_value, std::vector<double> strength_value,
    std::vector<VrsEvidenceRequest> requests_value,
    std::shared_ptr<const VrsHotAddressIndex> index_value,
    std::string address_value, const double threshold)
    : terms(std::move(terms_value)), score(std::move(score_value)),
      support(std::move(support_value)), refute(std::move(refute_value)),
      edge_source(std::move(source_value)), edge_target(std::move(target_value)),
      edge_sign(std::move(sign_value)), vrs_strength(std::move(strength_value)),
      evidence_requests(std::move(requests_value)), address_index(std::move(index_value)),
      source_address(std::move(address_value)), promotion_threshold(threshold) {
    const auto term_count = terms.size();
    const auto edge_count = edge_source.size();
    if (source_address.empty() || !std::isfinite(promotion_threshold) || promotion_threshold <= 0 ||
        score.size() != term_count || support.size() != term_count || refute.size() != term_count ||
        edge_target.size() != edge_count || edge_sign.size() != edge_count ||
        vrs_strength.size() != edge_count || !address_index ||
        address_index->term_count != term_count || address_index->edge_count != edge_count)
        throw std::invalid_argument("VRS query memory input changed");
    if (std::ranges::any_of(terms, [](const auto& term) { return term.empty(); }) ||
        std::ranges::any_of(vrs_strength, [](const double value) {
            return !std::isfinite(value) || value < 0;
        })) throw std::invalid_argument("VRS query memory input changed");
    for (std::size_t edge = 0; edge < edge_count; ++edge)
        if (edge_source[edge] >= term_count || edge_target[edge] >= term_count ||
            (edge_sign[edge] != 1 && edge_sign[edge] != -1))
            throw std::invalid_argument("VRS query memory input changed");
    for (const auto& request : evidence_requests)
        if (request.term_id >= term_count || request.term != terms[request.term_id] ||
            request.requested_evidence.empty())
            throw std::invalid_argument("VRS evidence request changed");

    for (const auto& outcome : memory_outcomes) outcome_counts_[outcome] = 0;
    for (std::size_t edge = 0; edge < edge_count; ++edge) {
        if (edge_sign[edge] > 0) ++outcome_counts_["success"];
        else ++outcome_counts_["negative"];
    }
    for (std::size_t term = 0; term < term_count; ++term) {
        if (support[term] > 0) ++outcome_counts_["success"];
        if (refute[term] > 0) { ++outcome_counts_["failure"]; ++outcome_counts_["negative"]; }
        if (support[term] > 0 && refute[term] > 0) ++outcome_counts_["conflict"];
        if (support[term] > 0 || refute[term] > 0) ++episode_count_;
    }
    outcome_counts_["uncertain"] += evidence_requests.size();
    outcome_counts_["pending"] += evidence_requests.size();
    episode_count_ += edge_count + evidence_requests.size();
    snapshot_id_ = digest(semantic_canonical_json(JsonValue::Object{
        {"edge_count", static_cast<std::int64_t>(edge_count)},
        {"evidence_request_count", static_cast<std::int64_t>(evidence_requests.size())},
        {"schema_version", "rozephine-vrs-hot-memory-source-v1"},
        {"source_address", source_address},
        {"term_count", static_cast<std::int64_t>(term_count)}}));
}

std::string_view VrsHotMemorySource::snapshot_id() const noexcept { return snapshot_id_; }
std::size_t VrsHotMemorySource::episode_count() const noexcept { return episode_count_; }
const std::map<std::string, std::size_t, std::less<>>&
VrsHotMemorySource::outcome_counts() const noexcept { return outcome_counts_; }

const MemoryEpisode& VrsHotMemorySource::retain(std::string key, MemoryEpisode value) const {
    std::lock_guard lock(retained_mutex_);
    const auto found = retained_.find(key);
    if (found != retained_.end()) return *found->second;
    auto item = std::make_shared<const MemoryEpisode>(std::move(value));
    const auto* pointer = item.get();
    retained_.emplace(std::move(key), std::move(item));
    return *pointer;
}

MemoryEpisode VrsHotMemorySource::edge_episode(
    const std::size_t edge, std::string identifier) const {
    if (edge >= edge_source.size()) throw std::out_of_range(identifier);
    const auto source = edge_source[edge], target = edge_target[edge];
    const auto sign = edge_sign[edge];
    const auto strength = vrs_strength[edge];
    const bool eligible = strength >= promotion_threshold;
    auto cues = unique({canonical_vrs_cue(terms[source]), canonical_vrs_cue(terms[target])});
    JsonValue::Object observation{
        {"edge_id", static_cast<std::int64_t>(edge)},
        {"source_term_id", static_cast<std::int64_t>(source)}, {"source_term", terms[source]},
        {"target_term_id", static_cast<std::int64_t>(target)}, {"target_term", terms[target]},
        {"sign", static_cast<std::int64_t>(sign)}, {"vrs_strength", strength},
        {"promotion_threshold", promotion_threshold}, {"promotion_eligible", eligible}};
    auto relation = terms[source] + (sign > 0 ? " supports " : " refutes ") + terms[target];
    return {identifier, std::move(cues),
        {step("experience_occurrence_to_vrs_connection", std::move(observation), {relation},
              sign > 0 ? "reinforced_connection" : "negative_connection",
              sign > 0 ? "success" : "negative", {source_address, identifier})},
        {source_address, identifier}, source_address,
        eligible ? "verified_experience_evidence" : "historical_non_authoritative_vrs"};
}

MemoryEpisode VrsHotMemorySource::term_episode(const std::size_t term) const {
    const auto identifier = "vrs-term:" + std::to_string(term);
    if (term >= terms.size() || (support[term] <= 0 && refute[term] <= 0))
        throw std::out_of_range(identifier);
    JsonValue::Object observation{{"term_id", static_cast<std::int64_t>(term)},
        {"term", terms[term]}, {"score", score[term]}, {"support", support[term]},
        {"refute", refute[term]}};
    std::vector<MemoryStep> steps;
    const auto add = [&](const bool enabled, std::string phase, std::string relation,
                         std::string judgment, std::string outcome, std::string suffix) {
        if (enabled) steps.push_back(step(std::move(phase), observation, {std::move(relation)},
            std::move(judgment), std::move(outcome),
            {source_address, identifier + ":" + suffix}));
    };
    add(support[term] != 0, "preserved_term_evidence", "term-has-supporting-experience",
        "historical_support_present", "success", "support");
    add(refute[term] != 0, "preserved_term_evidence", "term-has-refuting-experience",
        "historical_refutation_present", "failure", "refute");
    add(refute[term] != 0, "preserved_term_negative_evidence", "term-has-negative-experience",
        "historical_negative_evidence_present", "negative", "negative");
    add(support[term] != 0 && refute[term] != 0, "preserved_term_conflict",
        "support-conflicts-with-refute", "unresolved_historical_conflict", "conflict", "conflict");
    return {identifier, {canonical_vrs_cue(terms[term])}, std::move(steps),
            {source_address, identifier}, source_address,
            "historical_non_authoritative_term_state"};
}

MemoryEpisode VrsHotMemorySource::request_episode(const std::size_t request_id) const {
    const auto identifier = "vrs-evidence-request:" + std::to_string(request_id);
    if (request_id >= evidence_requests.size()) throw std::out_of_range(identifier);
    const auto& request = evidence_requests[request_id];
    JsonValue::Object observation{{"term_id", static_cast<std::int64_t>(request.term_id)},
        {"term", terms[request.term_id]}, {"priority", request_value(request, "priority")},
        {"connection_score", request_value(request, "connection_score")},
        {"support", request_value(request, "support")},
        {"refute", request_value(request, "refute")},
        {"requested_evidence", request.requested_evidence}, {"semantic_authority", false}};
    return {identifier, {canonical_vrs_cue(terms[request.term_id])},
        {step("unresolved_evidence_request", observation,
              {"current-vrs-state-requires-more-evidence"},
              "evidence_insufficient_or_conflicting", "uncertain",
              {source_address, identifier}),
         step("pending_re_evidence", std::move(observation),
              {"request-awaits-current-evidence"}, "re_evidence_pending", "pending",
              {source_address, identifier})},
        {source_address, identifier}, source_address,
        "pending_non_authoritative_evidence_request"};
}

const MemoryEpisode& VrsHotMemorySource::episode(const std::string_view identifier) const {
    {
        std::lock_guard lock(retained_mutex_);
        const auto found = retained_.find(identifier);
        if (found != retained_.end()) return *found->second;
    }
    if (identifier.starts_with("vrs-edge:")) {
        const auto id = parse_id(identifier, "vrs-edge:");
        return retain(std::string(identifier), edge_episode(id, std::string(identifier)));
    }
    if (identifier.starts_with("vrs-term:"))
        return retain(std::string(identifier), term_episode(parse_id(identifier, "vrs-term:")));
    if (identifier.starts_with("vrs-evidence-request:"))
        return retain(std::string(identifier), request_episode(
            parse_id(identifier, "vrs-evidence-request:")));
    throw std::out_of_range(std::string(identifier));
}

std::vector<std::string> VrsHotMemorySource::episode_ids_for_cue(
    const std::string_view cue) const {
    std::set<std::string, std::less<>> result;
    for (const auto term : address_index->term_ids(cue)) {
        if (support[term] != 0 || refute[term] != 0)
            result.insert("vrs-term:" + std::to_string(term));
        for (const auto edge : address_index->edge_ids_for_term(term))
            result.insert("vrs-edge:" + std::to_string(edge));
        const auto requests = address_index->request_ids_by_term.find(term);
        if (requests != address_index->request_ids_by_term.end())
            for (const auto id : requests->second)
                result.insert("vrs-evidence-request:" + std::to_string(id));
    }
    return {result.begin(), result.end()};
}

std::vector<std::string> VrsHotMemorySource::iter_episode_ids() const {
    std::vector<std::string> result;
    result.reserve(episode_count_);
    for (std::size_t id = 0; id < edge_source.size(); ++id)
        result.push_back("vrs-edge:" + std::to_string(id));
    for (std::size_t id = 0; id < terms.size(); ++id)
        if (support[id] != 0 || refute[id] != 0)
            result.push_back("vrs-term:" + std::to_string(id));
    for (std::size_t id = 0; id < evidence_requests.size(); ++id)
        result.push_back("vrs-evidence-request:" + std::to_string(id));
    return result;
}

CanonicalVrsHotMemorySource::CanonicalVrsHotMemorySource(
    std::vector<std::string> terms_value, std::vector<double> score_value,
    std::vector<std::int64_t> support_value, std::vector<std::int64_t> refute_value,
    std::vector<std::uint32_t> source_value, std::vector<std::uint32_t> target_value,
    std::vector<std::int8_t> sign_value, std::vector<double> strength_value,
    std::vector<VrsEvidenceRequest> requests_value,
    std::shared_ptr<const VrsHotAddressIndex> index_value,
    std::string address_value, const double threshold,
    std::vector<std::uint32_t> edge_to_group,
    std::vector<std::uint64_t> offsets,
    std::vector<std::uint32_t> member_ids,
    std::string manifest)
    : VrsHotMemorySource(std::move(terms_value), std::move(score_value),
          std::move(support_value), std::move(refute_value), std::move(source_value),
          std::move(target_value), std::move(sign_value), std::move(strength_value),
          std::move(requests_value), std::move(index_value), std::move(address_value), threshold),
      member_edge_to_group(std::move(edge_to_group)),
      group_member_offsets(std::move(offsets)),
      group_member_edge_ids(std::move(member_ids)),
      member_manifest_address(std::move(manifest)) {
    const auto group_count = edge_source.size();
    const auto member_count = member_edge_to_group.size();
    if (member_manifest_address.empty() || group_member_offsets.size() != group_count + 1 ||
        group_member_offsets.front() != 0 || group_member_offsets.back() != member_count ||
        group_member_edge_ids.size() != member_count ||
        !std::ranges::is_sorted(group_member_offsets))
        throw std::invalid_argument("canonical VRS member lineage changed");
    std::vector<bool> seen(member_count);
    for (std::size_t group = 0; group < group_count; ++group)
        for (std::uint64_t at = group_member_offsets[group]; at < group_member_offsets[group + 1]; ++at) {
            const auto member = group_member_edge_ids[at];
            if (member >= member_count || seen[member] || member_edge_to_group[member] != group)
                throw std::invalid_argument("canonical VRS reverse member mapping changed");
            seen[member] = true;
        }
    episode_count_ = member_count + evidence_requests.size();
    for (std::size_t term = 0; term < terms.size(); ++term)
        if (support[term] != 0 || refute[term] != 0) ++episode_count_;
    outcome_counts_["success"] = 0;
    outcome_counts_["negative"] = 0;
    for (std::size_t group = 0; group < group_count; ++group) {
        const auto count = group_member_offsets[group + 1] - group_member_offsets[group];
        (edge_sign[group] > 0 ? outcome_counts_["success"] : outcome_counts_["negative"]) += count;
    }
    architecture::Sha256 hash;
    hash.update(snapshot_id_);
    hash.update(std::as_bytes(std::span(member_edge_to_group)));
    hash.update(std::as_bytes(std::span(group_member_offsets)));
    hash.update(std::as_bytes(std::span(group_member_edge_ids)));
    hash.update(member_manifest_address);
    snapshot_id_ = hex(hash.finish());
}

std::size_t CanonicalVrsHotMemorySource::original_edge_count() const noexcept {
    return member_edge_to_group.size();
}

std::size_t CanonicalVrsHotMemorySource::canonical_group_for_member(
    const std::size_t edge) const {
    if (edge >= member_edge_to_group.size()) throw std::out_of_range("vrs-edge:" + std::to_string(edge));
    return member_edge_to_group[edge];
}

std::span<const std::uint32_t> CanonicalVrsHotMemorySource::member_edge_ids_for_group(
    const std::size_t group) const {
    if (group >= edge_source.size()) throw std::out_of_range("vrs-edge-group:" + std::to_string(group));
    return std::span(group_member_edge_ids).subspan(group_member_offsets[group],
        group_member_offsets[group + 1] - group_member_offsets[group]);
}

MemoryEpisode CanonicalVrsHotMemorySource::edge_episode(
    const std::size_t group, std::string identifier) const {
    if (group >= edge_source.size()) throw std::out_of_range(identifier);
    const auto source = edge_source[group], target = edge_target[group];
    const auto sign = edge_sign[group];
    const auto strength = vrs_strength[group];
    const auto group_ref = "vrs-edge-group:" + std::to_string(group);
    std::optional<std::size_t> member;
    if (identifier.starts_with("vrs-edge:")) member = parse_id(identifier, "vrs-edge:");
    std::vector<std::string> refs{source_address, member_manifest_address, group_ref};
    if (member) refs.push_back("vrs-edge:" + std::to_string(*member));
    JsonValue::Object observation{
        {"canonical_group_id", static_cast<std::int64_t>(group)},
        {"member_edge_id", member ? JsonValue(static_cast<std::int64_t>(*member)) : JsonValue(nullptr)},
        {"member_count", static_cast<std::int64_t>(group_member_offsets[group + 1] - group_member_offsets[group])},
        {"source_term_id", static_cast<std::int64_t>(source)}, {"source_term", terms[source]},
        {"target_term_id", static_cast<std::int64_t>(target)}, {"target_term", terms[target]},
        {"sign", static_cast<std::int64_t>(sign)}, {"deweighted_vrs_strength", strength},
        {"promotion_threshold", promotion_threshold},
        {"promotion_eligible", strength >= promotion_threshold},
        {"all_member_edges_retained", true}, {"member_manifest_address", member_manifest_address}};
    return {identifier, unique({canonical_vrs_cue(terms[source]), canonical_vrs_cue(terms[target])}),
        {step("canonical_experience_occurrence_to_vrs_connection", std::move(observation),
              {terms[source] + (sign > 0 ? " supports " : " refutes ") + terms[target]},
              sign > 0 ? "canonical_reinforced_connection" : "canonical_negative_connection",
              sign > 0 ? "success" : "negative", refs)}, refs, source_address,
        strength >= promotion_threshold ? "verified_experience_evidence" :
                                          "historical_non_authoritative_vrs"};
}

const MemoryEpisode& CanonicalVrsHotMemorySource::episode(
    const std::string_view identifier) const {
    {
        std::lock_guard lock(retained_mutex_);
        const auto found = retained_.find(identifier);
        if (found != retained_.end()) return *found->second;
    }
    if (identifier.starts_with("vrs-edge-group:")) {
        const auto group = parse_id(identifier, "vrs-edge-group:");
        return retain(std::string(identifier), edge_episode(group, std::string(identifier)));
    }
    if (identifier.starts_with("vrs-edge:")) {
        const auto member = parse_id(identifier, "vrs-edge:");
        return retain(std::string(identifier), edge_episode(
            canonical_group_for_member(member), std::string(identifier)));
    }
    return VrsHotMemorySource::episode(identifier);
}

std::vector<std::string> CanonicalVrsHotMemorySource::episode_ids_for_cue(
    const std::string_view cue) const {
    std::set<std::string, std::less<>> result;
    for (const auto term : address_index->term_ids(cue)) {
        if (support[term] != 0 || refute[term] != 0)
            result.insert("vrs-term:" + std::to_string(term));
        for (const auto group : address_index->edge_ids_for_term(term))
            result.insert("vrs-edge-group:" + std::to_string(group));
        const auto requests = address_index->request_ids_by_term.find(term);
        if (requests != address_index->request_ids_by_term.end())
            for (const auto id : requests->second)
                result.insert("vrs-evidence-request:" + std::to_string(id));
    }
    return {result.begin(), result.end()};
}

std::vector<std::string> CanonicalVrsHotMemorySource::iter_episode_ids() const {
    std::vector<std::string> result;
    result.reserve(episode_count_);
    for (std::size_t id = 0; id < original_edge_count(); ++id)
        result.push_back("vrs-edge:" + std::to_string(id));
    for (std::size_t id = 0; id < terms.size(); ++id)
        if (support[id] != 0 || refute[id] != 0)
            result.push_back("vrs-term:" + std::to_string(id));
    for (std::size_t id = 0; id < evidence_requests.size(); ++id)
        result.push_back("vrs-evidence-request:" + std::to_string(id));
    return result;
}

VrsQueryMemoryBundle build_vrs_query_memory_bundle(VrsQueryMemoryInput input) {
    const auto term_count = input.terms.size();
    const auto edge_count = input.edge_source.size();
    auto index = address_index(input.terms, input.edge_source, input.edge_target,
                               input.evidence_requests);
    const bool any_canonical = input.member_edge_to_group || input.group_member_offsets ||
        input.group_member_edge_ids || input.member_manifest_address;
    const bool all_canonical = input.member_edge_to_group && input.group_member_offsets &&
        input.group_member_edge_ids && input.member_manifest_address;
    if (any_canonical != all_canonical)
        throw std::invalid_argument("canonical VRS member lineage must be supplied together");
    std::shared_ptr<const VrsHotMemorySource> source;
    if (all_canonical) {
        source = std::make_shared<const CanonicalVrsHotMemorySource>(
            input.terms, input.score, input.support, input.refute,
            input.edge_source, input.edge_target, input.edge_sign, input.vrs_strength,
            input.evidence_requests, index, input.source_address, input.promotion_threshold,
            *input.member_edge_to_group, *input.group_member_offsets,
            *input.group_member_edge_ids, *input.member_manifest_address);
    } else {
        source = std::make_shared<const VrsHotMemorySource>(
            input.terms, input.score, input.support, input.refute,
            input.edge_source, input.edge_target, input.edge_sign, input.vrs_strength,
            input.evidence_requests, index, input.source_address, input.promotion_threshold);
    }
    std::set<std::string, std::less<>> staged_cues;
    for (const auto& episode : input.additional_episodes)
        staged_cues.insert(episode.cues.begin(), episode.cues.end());
    input.queries = unique(std::move(input.queries));
    if (input.queries.empty() || std::ranges::any_of(input.queries,
            [](const auto& query) { return collapse_unicode_whitespace(query).empty(); }))
        throw std::invalid_argument("VRS query memory requires nonempty queries");
    std::map<std::string, std::vector<std::string>, std::less<>> query_cues;
    for (const auto& query : input.queries) {
        auto& selected = query_cues[query];
        for (auto key : query_keys(query))
            if (index->term_ids_by_cue.contains(key) || staged_cues.contains(key))
                selected.push_back(std::move(key));
    }
    auto combined = append_memory_activation_index(source, input.additional_episodes,
        std::vector<std::string>(memory_outcomes.begin(), memory_outcomes.end()));
    std::vector<std::string> promoted;
    const bool canonical = all_canonical;
    for (std::size_t edge = 0; edge < input.vrs_strength.size(); ++edge)
        if (input.vrs_strength[edge] >= input.promotion_threshold)
            promoted.push_back(std::string(canonical ? "vrs-edge-group:" : "vrs-edge:") +
                               std::to_string(edge));
    return {combined, std::move(query_cues), {0, term_count}, {0, edge_count},
            std::move(promoted), combined->outcome_counts(), input.source_address,
            std::move(source)};
}

}  // namespace swegca::world
