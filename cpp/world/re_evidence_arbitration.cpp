#include "world/re_evidence_arbitration.hpp"

#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <future>
#include <limits>
#include <memory_resource>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

using Json = transport::Json;

constexpr std::string_view exact_binding =
    "exact_source_address_and_revision";

std::string hex_digest(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(digest.size() * 2U, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        out[index * 2U] = digits[value >> 4U];
        out[index * 2U + 1U] = digits[value & 15U];
    }
    return out;
}

bool unicode_space(const char32_t value) noexcept {
    return (value >= 0x09U && value <= 0x0dU) ||
           (value >= 0x1cU && value <= 0x20U) || value == 0x85U ||
           value == 0xa0U || value == 0x1680U ||
           (value >= 0x2000U && value <= 0x200aU) || value == 0x2028U ||
           value == 0x2029U || value == 0x202fU || value == 0x205fU ||
           value == 0x3000U;
}

std::pair<char32_t, std::size_t> decode_forward(
    const std::string_view value, const std::size_t offset) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first < 0x80U) return {first, 1U};
    std::size_t width = 0;
    char32_t codepoint = 0;
    if ((first & 0xe0U) == 0xc0U) { width = 2; codepoint = first & 0x1fU; }
    else if ((first & 0xf0U) == 0xe0U) { width = 3; codepoint = first & 0x0fU; }
    else if ((first & 0xf8U) == 0xf0U) { width = 4; codepoint = first & 0x07U; }
    else throw std::invalid_argument("invalid UTF-8 text");
    if (offset + width > value.size())
        throw std::invalid_argument("invalid UTF-8 text");
    for (std::size_t index = 1; index != width; ++index) {
        const auto byte = static_cast<unsigned char>(value[offset + index]);
        if ((byte & 0xc0U) != 0x80U)
            throw std::invalid_argument("invalid UTF-8 text");
        codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }
    return {codepoint, width};
}

std::string text_value(const Json* value, const std::string_view label) {
    if (value == nullptr || value->kind != Json::Kind::string)
        throw std::invalid_argument(std::string(label) +
                                    " must be nonempty text");
    const std::string_view source(value->scalar);
    std::size_t begin = 0;
    while (begin < source.size()) {
        const auto [point, width] = decode_forward(source, begin);
        if (!unicode_space(point)) break;
        begin += width;
    }
    std::size_t scan = begin;
    std::size_t last_nonspace_end = begin;
    while (scan < source.size()) {
        const auto [point, width] = decode_forward(source, scan);
        scan += width;
        if (!unicode_space(point)) last_nonspace_end = scan;
    }
    if (last_nonspace_end == begin)
        throw std::invalid_argument(std::string(label) +
                                    " must be nonempty text");
    return std::string(source.substr(begin, last_nonspace_end - begin));
}

void validate_text_only(const std::string& value, const std::string_view label) {
    Json temporary(std::pmr::get_default_resource());
    temporary.kind = Json::Kind::string;
    temporary.scalar = value;
    (void)text_value(&temporary, label);
}

const Json* member(const Json& object, const std::string_view key) noexcept {
    return object.find(key);
}

std::optional<std::string> optional_text(const Json* value,
                                         const std::string_view label) {
    if (value == nullptr || value->kind == Json::Kind::null) return std::nullopt;
    return text_value(value, label);
}

std::vector<std::string> unique_text(const Json* value,
                                     const std::string_view label) {
    if (value == nullptr || value->kind != Json::Kind::array)
        throw std::invalid_argument(std::string(label) + " must be a sequence");
    std::vector<std::string> result;
    result.reserve(value->values.size());
    std::unordered_set<std::string> seen;
    for (const auto& item : value->values) {
        auto normalized = text_value(&item, label);
        if (!seen.insert(normalized).second)
            throw std::invalid_argument(std::string(label) + " must be unique");
        result.push_back(std::move(normalized));
    }
    return result;
}

void append_stable_unique(std::vector<std::string>& destination,
                          std::unordered_set<std::string>& seen,
                          std::vector<std::string> values) {
    for (auto& value : values)
        if (seen.insert(value).second) destination.push_back(std::move(value));
}

std::optional<std::int64_t> optional_nonnegative_int(
    const Json* value, const std::string_view label) {
    if (value == nullptr || value->kind == Json::Kind::null) return std::nullopt;
    if (value->kind != Json::Kind::number ||
        value->scalar.find_first_of(".eE") != std::string_view::npos)
        throw std::invalid_argument(std::string(label) +
                                    " must be a nonnegative integer");
    std::int64_t parsed = 0;
    const char* const begin = value->scalar.data();
    const char* const end = begin + value->scalar.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 0)
        throw std::invalid_argument(std::string(label) +
                                    " must be a nonnegative integer");
    return parsed;
}

std::optional<double> optional_confidence(const Json* value) {
    if (value == nullptr || value->kind == Json::Kind::null) return std::nullopt;
    if (value->kind != Json::Kind::number)
        throw std::invalid_argument("current producer confidence must be numeric");
    double parsed = 0.0;
    const char* const begin = value->scalar.data();
    const char* const end = begin + value->scalar.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end ||
        !std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0)
        throw std::invalid_argument(
            "current producer confidence must be finite within [0, 1]");
    return parsed;
}

bool python_truthy(const Json& value) {
    switch (value.kind) {
    case Json::Kind::null: return false;
    case Json::Kind::boolean: return value.scalar == "true";
    case Json::Kind::string: return !value.scalar.empty();
    case Json::Kind::array:
    case Json::Kind::object: return !value.values.empty();
    case Json::Kind::number: {
        double parsed = 0.0;
        const char* const begin = value.scalar.data();
        const char* const end = begin + value.scalar.size();
        const auto result = std::from_chars(begin, end, parsed);
        return result.ec != std::errc{} || result.ptr != end || parsed != 0.0;
    }
    }
    return false;
}

bool contains(const std::set<std::string_view>& values,
              const std::string_view value) {
    return values.find(value) != values.end();
}

std::optional<std::string> target_episode(
    const std::vector<ReEvidenceCandidate>& candidates,
    const std::vector<ReEvidenceCurrentEvidence>& evidence) {
    if (evidence.empty() ||
        !std::all_of(evidence.begin(), evidence.end(), [](const auto& item) {
            return item.transaction_ready();
        })) return std::nullopt;
    std::optional<std::string> bound;
    for (const auto& candidate : candidates) {
        if (!candidate.revision) continue;
        const bool matches = std::all_of(
            evidence.begin(), evidence.end(), [&](const auto& item) {
                return item.source_address && item.source_revision &&
                       std::find(candidate.source_addresses.begin(),
                                 candidate.source_addresses.end(),
                                 *item.source_address) !=
                           candidate.source_addresses.end() &&
                       *item.source_revision == *candidate.revision;
            });
        if (!matches) continue;
        if (bound) return std::nullopt;
        bound = candidate.episode_id;
    }
    return bound;
}

}  // namespace

ReEvidenceCandidate::ReEvidenceCandidate(
    std::string episode_id_value, std::vector<std::string> matched_cues_value,
    std::vector<std::string> propositions_value,
    std::vector<std::string> historical_outcomes_value,
    std::vector<std::string> evidence_refs_value,
    std::vector<std::string> source_addresses_value,
    std::string verification_state_value,
    std::optional<std::string> revision_value)
    : episode_id(std::move(episode_id_value)),
      matched_cues(std::move(matched_cues_value)),
      propositions(std::move(propositions_value)),
      historical_outcomes(std::move(historical_outcomes_value)),
      evidence_refs(std::move(evidence_refs_value)),
      source_addresses(std::move(source_addresses_value)),
      verification_state(std::move(verification_state_value)),
      revision(std::move(revision_value)) {}

ReEvidenceCurrentEvidence::ReEvidenceCurrentEvidence(
    std::string evidence_ref_value, std::string observation_value,
    std::string source_kind_value,
    std::optional<std::string> source_address_value,
    std::optional<std::string> source_revision_value,
    std::optional<std::string> source_family_value,
    std::optional<std::string> context_hash_value,
    std::optional<std::string> axis_value,
    std::optional<std::string> verification_outcome_value,
    std::optional<std::int64_t> observed_at_value,
    std::optional<std::int64_t> expires_at_value,
    std::optional<std::string> producer_id_value,
    std::optional<double> producer_confidence_value)
    : evidence_ref(std::move(evidence_ref_value)),
      observation(std::move(observation_value)),
      source_kind(std::move(source_kind_value)),
      source_address(std::move(source_address_value)),
      source_revision(std::move(source_revision_value)),
      source_family(std::move(source_family_value)),
      context_hash(std::move(context_hash_value)), axis(std::move(axis_value)),
      verification_outcome(std::move(verification_outcome_value)),
      observed_at(observed_at_value), expires_at(expires_at_value),
      producer_id(std::move(producer_id_value)),
      producer_confidence(producer_confidence_value) {}

bool ReEvidenceCurrentEvidence::transaction_ready() const noexcept {
    return source_address && source_revision && source_family && context_hash &&
           axis && verification_outcome && observed_at && producer_id &&
           producer_confidence;
}

PreparedReEvidenceRequest::PreparedReEvidenceRequest(
    std::string request_sha256_value, std::string user_query_value,
    std::vector<ReEvidenceCandidate> candidates_value,
    std::vector<ReEvidenceCurrentEvidence> current_evidence_value,
    std::optional<std::string> target_value)
    : request_sha256(std::move(request_sha256_value)),
      user_query(std::move(user_query_value)),
      candidates(std::move(candidates_value)),
      current_evidence(std::move(current_evidence_value)),
      current_evidence_target_episode_id(std::move(target_value)) {}

const ReEvidenceCandidate* PreparedReEvidenceRequest::candidate(
    const std::string_view episode_id_value) const noexcept {
    const auto found = std::find_if(candidates.begin(), candidates.end(),
        [&](const auto& item) { return item.episode_id == episode_id_value; });
    return found == candidates.end() ? nullptr : &*found;
}

std::vector<std::string> PreparedReEvidenceRequest::current_evidence_refs() const {
    std::vector<std::string> result;
    result.reserve(current_evidence.size());
    for (const auto& item : current_evidence) result.push_back(item.evidence_ref);
    return result;
}

std::vector<std::string> PreparedReEvidenceRequest::current_observations() const {
    std::vector<std::string> result;
    result.reserve(current_evidence.size());
    for (const auto& item : current_evidence) result.push_back(item.observation);
    return result;
}

std::vector<std::string> PreparedReEvidenceRequest::current_source_kinds() const {
    std::vector<std::string> result;
    result.reserve(current_evidence.size());
    for (const auto& item : current_evidence) result.push_back(item.source_kind);
    return result;
}

std::vector<std::string> PreparedReEvidenceRequest::current_source_addresses() const {
    std::vector<std::string> result;
    for (const auto& item : current_evidence)
        if (item.source_address) result.push_back(*item.source_address);
    return result;
}

ReEvidenceProposal::ReEvidenceProposal(
    std::string source_value, std::string source_address_value,
    std::string request_sha256_value,
    std::optional<std::string> selected_episode_id_value,
    std::string proposition_value, std::string verdict_value,
    std::string rationale_value, const bool action_authorized_value,
    const bool persistent_write_authorized_value,
    const bool semantic_promotion_authorized_value)
    : source(std::move(source_value)),
      source_address(std::move(source_address_value)),
      request_sha256(std::move(request_sha256_value)),
      selected_episode_id(std::move(selected_episode_id_value)),
      proposition(std::move(proposition_value)), verdict(std::move(verdict_value)),
      rationale(std::move(rationale_value)),
      action_authorized(action_authorized_value),
      persistent_write_authorized(persistent_write_authorized_value),
      semantic_promotion_authorized(semantic_promotion_authorized_value) {
    validate_text_only(source, "proposal source");
    validate_text_only(source_address, "proposal source address");
    validate_text_only(request_sha256, "proposal request SHA-256");
    if (selected_episode_id)
        validate_text_only(*selected_episode_id, "selected episode_id");
    validate_text_only(proposition, "proposal proposition");
    static const std::set<std::string_view> verdicts{
        "support", "refute", "insufficient", "conflict"};
    if (!contains(verdicts, verdict))
        throw std::invalid_argument("unsupported Re-evidence proposal verdict");
    if (!selected_episode_id && verdict != "insufficient")
        throw std::invalid_argument(
            "only insufficient may omit a selected episode");
    validate_text_only(rationale, "proposal rationale");
    if (action_authorized || persistent_write_authorized ||
        semantic_promotion_authorized)
        throw std::invalid_argument(
            "Re-evidence proposal grants forbidden authority");
}

PreparedReEvidenceRequest prepare_re_evidence_request(
    std::string serialized_input, std::string expected_sha256) {
    const std::string normalized_sha = [&] {
        Json value(std::pmr::get_default_resource());
        value.kind = Json::Kind::string;
        value.scalar = expected_sha256;
        return text_value(&value, "request SHA-256");
    }();
    architecture::Sha256 digest;
    digest.update(std::string_view(serialized_input));
    if (hex_digest(digest.finish()) != normalized_sha)
        throw std::invalid_argument("serialized Re-evidence request changed");

    std::pmr::monotonic_buffer_resource memory;
    const Json payload = transport::parse_json(serialized_input, memory);
    if (payload.kind != Json::Kind::object)
        throw std::invalid_argument("Re-evidence request must be a JSON object");
    const auto exact_string = [&](const std::string_view key,
                                  const std::string_view expected) {
        const auto* value = member(payload, key);
        return value != nullptr && value->kind == Json::Kind::string &&
               value->scalar == expected;
    };
    if (!exact_string("user_query_role", "retrieval_cue_only"))
        throw std::invalid_argument("user query role changed");
    if (!exact_string("historical_memory_role",
                      "replayed_candidate_not_current_truth"))
        throw std::invalid_argument("historical memory role changed");

    const Json* const authority = member(payload, "authority");
    bool authority_valid = authority != nullptr &&
                           authority->kind == Json::Kind::object &&
                           member(*authority, "action") != nullptr &&
                           member(*authority, "persistent_write") != nullptr &&
                           member(*authority, "semantic_promotion") != nullptr;
    if (authority_valid)
        authority_valid = std::none_of(authority->values.begin(),
                                       authority->values.end(), python_truthy);
    if (!authority_valid)
        throw std::invalid_argument("Re-evidence request grants forbidden authority");

    const Json* const raw_candidates = member(payload, "candidates");
    if (raw_candidates == nullptr || raw_candidates->kind != Json::Kind::array ||
        raw_candidates->values.empty())
        throw std::invalid_argument("Re-evidence request requires candidates");
    static const std::set<std::string_view> historical_outcomes{
        "success", "failure", "uncertain", "conflict", "pending"};
    std::vector<ReEvidenceCandidate> candidates;
    candidates.reserve(raw_candidates->values.size());
    for (const auto& raw_candidate : raw_candidates->values) {
        if (raw_candidate.kind != Json::Kind::object)
            throw std::invalid_argument("Re-evidence candidate must be an object");
        const Json* const raw_steps = member(raw_candidate, "steps");
        if (raw_steps == nullptr || raw_steps->kind != Json::Kind::array ||
            raw_steps->values.empty())
            throw std::invalid_argument("Re-evidence candidate requires replay steps");
        std::vector<std::string> propositions;
        std::vector<std::string> outcomes;
        std::vector<std::string> evidence_refs;
        std::unordered_set<std::string> proposition_set;
        std::unordered_set<std::string> evidence_set;
        for (const auto& step : raw_steps->values) {
            if (step.kind != Json::Kind::object)
                throw std::invalid_argument("replay step must be an object");
            append_stable_unique(propositions, proposition_set,
                unique_text(member(step, "relations"), "replay relation"));
            auto outcome = text_value(member(step, "outcome"),
                                      "historical outcome");
            if (!contains(historical_outcomes, outcome))
                throw std::invalid_argument("unsupported historical outcome");
            outcomes.push_back(std::move(outcome));
            append_stable_unique(evidence_refs, evidence_set,
                unique_text(member(step, "evidence_refs"),
                            "replay evidence address"));
        }
        const auto revision = optional_text(member(raw_candidate, "revision"),
                                            "candidate revision");
        const Json* const raw_addresses = member(raw_candidate, "source_addresses");
        auto addresses = raw_addresses == nullptr ||
                                 raw_addresses->kind == Json::Kind::null
            ? std::vector<std::string>{}
            : unique_text(raw_addresses, "candidate source address");
        candidates.emplace_back(
            text_value(member(raw_candidate, "episode_id"), "episode_id"),
            unique_text(member(raw_candidate, "matched_cues"), "matched cue"),
            std::move(propositions), std::move(outcomes),
            std::move(evidence_refs), std::move(addresses),
            text_value(member(raw_candidate, "verification_state"),
                       "verification state"),
            revision);
    }
    std::unordered_set<std::string> candidate_ids;
    for (const auto& candidate : candidates)
        if (!candidate_ids.insert(candidate.episode_id).second)
            throw std::invalid_argument("candidate episode IDs must be unique");

    const Json* const raw_current = member(payload, "current_observation_evidence");
    if (raw_current == nullptr || raw_current->kind != Json::Kind::array)
        throw std::invalid_argument("current observation evidence must be a list");
    static const std::set<std::string_view> current_outcomes{
        "support", "refute", "insufficient"};
    std::vector<ReEvidenceCurrentEvidence> current;
    current.reserve(raw_current->values.size());
    for (const auto& evidence : raw_current->values) {
        if (evidence.kind != Json::Kind::object)
            throw std::invalid_argument("current evidence must be an object");
        auto axis = optional_text(member(evidence, "axis"),
                                  "current evidence axis");
        auto outcome = optional_text(member(evidence, "verification_outcome"),
                                     "current verification outcome");
        if (outcome && !contains(current_outcomes, *outcome))
            throw std::invalid_argument("unsupported current verification outcome");
        auto observed = optional_nonnegative_int(member(evidence, "observed_at"),
                                                 "current observed_at");
        auto expires = optional_nonnegative_int(member(evidence, "expires_at"),
                                                "current expires_at");
        if (observed && expires && *expires < *observed)
            throw std::invalid_argument("current expires_at precedes observed_at");
        current.emplace_back(
            text_value(member(evidence, "evidence_ref"), "current evidence ref"),
            text_value(member(evidence, "observation"), "current observation"),
            text_value(member(evidence, "source_kind"), "current source kind"),
            optional_text(member(evidence, "source_address"),
                          "current source address"),
            optional_text(member(evidence, "source_revision"),
                          "current source revision"),
            optional_text(member(evidence, "source_family"),
                          "current source family"),
            optional_text(member(evidence, "context_hash"),
                          "current context hash"),
            std::move(axis), std::move(outcome), observed, expires,
            optional_text(member(evidence, "producer_id"),
                          "current producer ID"),
            optional_confidence(member(evidence, "producer_confidence")));
        const auto& added = current.back();
        const bool any_extended = added.source_address || added.source_revision ||
            added.source_family || added.context_hash || added.axis ||
            added.verification_outcome || added.observed_at || added.producer_id ||
            added.producer_confidence;
        if (any_extended && !added.transaction_ready())
            throw std::invalid_argument(
                "current evidence provenance is partially specified");
    }
    std::unordered_set<std::string> current_refs;
    for (const auto& evidence : current)
        if (!current_refs.insert(evidence.evidence_ref).second)
            throw std::invalid_argument("current evidence refs must be unique");
    bool has_ready = false;
    bool has_incomplete = false;
    for (const auto& evidence : current)
        (evidence.transaction_ready() ? has_ready : has_incomplete) = true;
    if (has_ready && has_incomplete)
        throw std::invalid_argument(
            "current evidence provenance completeness must be uniform");
    const Json* const presence = member(payload,
        "current_observation_evidence_present");
    if (presence == nullptr || presence->kind != Json::Kind::boolean ||
        (presence->scalar == "true") != !current.empty())
        throw std::invalid_argument("current evidence presence flag changed");

    const Json* const binding_value = member(payload,
        "current_observation_candidate_binding");
    std::optional<std::string> binding;
    if (binding_value != nullptr && binding_value->kind != Json::Kind::null) {
        if (binding_value->kind != Json::Kind::string ||
            binding_value->scalar != exact_binding)
            throw std::invalid_argument(
                "current observation candidate binding changed");
        binding = std::string(exact_binding);
    }
    auto target = target_episode(candidates, current);
    if (binding && !target)
        throw std::invalid_argument(
            "exact current observation binding requires one unique candidate");
    const Json* const declared_value = member(payload,
        "current_observation_target_episode_id");
    if (declared_value != nullptr && declared_value->kind != Json::Kind::null) {
        const auto declared = text_value(
            declared_value, "current observation target episode_id");
        if (!binding)
            throw std::invalid_argument(
                "declared current observation target lacks binding rule");
        if (!target || declared != *target)
            throw std::invalid_argument(
                "declared current observation target does not match provenance");
    }

    return PreparedReEvidenceRequest(
        std::move(expected_sha256),
        text_value(member(payload, "user_query"), "user query"),
        std::move(candidates), std::move(current), std::move(target));
}

namespace {

std::optional<std::string> proposal_rejection(
    const PreparedReEvidenceRequest& request,
    const ReEvidenceProposal& proposal) {
    if (proposal.request_sha256 != request.request_sha256)
        return "request_address_mismatch";
    const auto current_refs = request.current_evidence_refs();
    const auto current_sources = request.current_source_addresses();
    if (std::find(current_refs.begin(), current_refs.end(), proposal.source_address) !=
            current_refs.end() ||
        std::find(current_sources.begin(), current_sources.end(),
                  proposal.source_address) != current_sources.end())
        return "proposal_output_cannot_be_current_evidence";
    const bool transaction_ready = !request.current_evidence.empty() &&
        std::all_of(request.current_evidence.begin(), request.current_evidence.end(),
                    [](const auto& item) { return item.transaction_ready(); });
    if (transaction_ready) {
        if (!request.current_evidence_target_episode_id)
            return "current_evidence_candidate_binding_unresolved";
        if (proposal.selected_episode_id !=
            request.current_evidence_target_episode_id)
            return "selected_candidate_not_bound_to_current_evidence";
    }
    if (!proposal.selected_episode_id)
        return proposal.verdict == "insufficient"
            ? std::nullopt
            : std::optional<std::string>("missing_candidate");
    const auto* candidate = request.candidate(*proposal.selected_episode_id);
    if (candidate == nullptr) return "candidate_not_in_request";
    if (transaction_ready) return std::nullopt;

    std::set<std::string> allowed(candidate->propositions.begin(),
                                  candidate->propositions.end());
    if (proposal.verdict == "insufficient") allowed.insert(request.user_query);
    if (!allowed.contains(proposal.proposition))
        return "proposition_not_in_selected_replay";
    if ((proposal.verdict == "support" || proposal.verdict == "refute") &&
        current_refs.empty())
        return "current_evidence_absent";
    if (proposal.verdict == "conflict" && current_refs.empty() &&
        std::find(candidate->historical_outcomes.begin(),
                  candidate->historical_outcomes.end(), "conflict") ==
            candidate->historical_outcomes.end())
        return "conflict_has_no_current_or_historical_basis";
    return std::nullopt;
}

std::unique_ptr<WorldState> transient_snapshot(const WorldState& state) {
    return std::make_unique<WorldState>(
        state.semantic_slots().clone(), state.active_mask().clone(),
        state.dirty_mask().clone(), std::string(state.source()),
        std::vector<SurfaceResidualRef>(state.surface_refs().begin(),
                                        state.surface_refs().end()));
}

std::unique_ptr<CognitiveState> transient_snapshot(const CognitiveState& state) {
    return std::make_unique<CognitiveState>(state.clone());
}

struct WorldPersistentSnapshot final {
    Tensor semantic_slots;
    BooleanMask active_mask;
    BooleanMask dirty_mask;
};

struct CognitivePersistentSnapshot final {
    Tensor semantic_slots;
    Tensor executive_slots;
    Tensor scratch_slots;
};

WorldPersistentSnapshot persistent_snapshot(const WorldState& state) {
    return {state.semantic_slots().clone(), state.active_mask().clone(),
            state.dirty_mask().clone()};
}

CognitivePersistentSnapshot persistent_snapshot(const CognitiveState& state) {
    return {state.semantic_slots().clone(), state.executive_slots().clone(),
            state.scratch_slots().clone()};
}

bool persistent_unchanged(const WorldState& state,
                          const WorldPersistentSnapshot& before) {
    return state.semantic_slots().exact_equal(before.semantic_slots) &&
        state.active_mask() == before.active_mask &&
        state.dirty_mask() == before.dirty_mask;
}

bool persistent_unchanged(const CognitiveState& state,
                          const CognitivePersistentSnapshot& before) {
    return state.semantic_slots().exact_equal(before.semantic_slots) &&
        state.executive_slots().exact_equal(before.executive_slots) &&
        state.scratch_slots().exact_equal(before.scratch_slots);
}

std::string state_owner(const WorldState& state) {
    return std::string(state.source());
}

std::string state_owner(const CognitiveState& state) {
    return std::string(state.owner_id());
}

template <typename State>
MainReEvidenceResult<State> arbitrate_re_evidence(
    std::shared_ptr<const State> state,
    const PreparedReEvidenceRequest& request,
    const std::span<const ReEvidenceProposal> proposals,
    std::vector<std::string> executed_cores, const bool fanout_used,
    const std::int64_t elapsed_ns) {
    std::vector<ProposalDisposition> dispositions;
    std::vector<const ReEvidenceProposal*> accepted;
    dispositions.reserve(proposals.size());
    accepted.reserve(proposals.size());
    for (const auto& proposal : proposals) {
        auto rejection = proposal_rejection(request, proposal);
        dispositions.emplace_back(proposal.source, proposal.source_address,
                                  !rejection.has_value(), rejection);
        if (!rejection) accepted.push_back(&proposal);
    }

    std::set<std::optional<std::string>> selected_ids;
    std::set<std::string> propositions;
    std::set<std::string> verdicts;
    for (const auto* proposal : accepted) {
        selected_ids.insert(proposal->selected_episode_id);
        propositions.insert(proposal->proposition);
        verdicts.insert(proposal->verdict);
    }
    const bool structural_conflict =
        selected_ids.size() > 1 || propositions.size() > 1;
    const bool transaction_ready = !request.current_evidence.empty() &&
        std::all_of(request.current_evidence.begin(), request.current_evidence.end(),
                    [](const auto& item) { return item.transaction_ready(); });
    const auto* target_candidate = request.current_evidence_target_episode_id
        ? request.candidate(*request.current_evidence_target_episode_id)
        : nullptr;
    const std::vector<std::string> target_propositions = target_candidate == nullptr
        ? std::vector<std::string>{}
        : target_candidate->propositions;
    std::set<std::string> current_outcomes;
    for (const auto& item : request.current_evidence)
        if (item.verification_outcome &&
            (*item.verification_outcome == "support" ||
             *item.verification_outcome == "refute"))
            current_outcomes.insert(*item.verification_outcome);

    std::string verdict;
    std::string rationale;
    if (!accepted.empty() && transaction_ready && target_propositions.size() != 1) {
        verdict = "insufficient";
        rationale = "Current evidence does not identify one bound replay proposition.";
    } else if (!accepted.empty() && transaction_ready &&
               current_outcomes == std::set<std::string>{"support"}) {
        verdict = "support";
        rationale = "Provenance-complete current evidence supports the bound replay.";
    } else if (!accepted.empty() && transaction_ready &&
               current_outcomes == std::set<std::string>{"refute"}) {
        verdict = "refute";
        rationale = "Provenance-complete current evidence refutes the bound replay.";
    } else if (!accepted.empty() && transaction_ready &&
               current_outcomes == std::set<std::string>{"refute", "support"}) {
        verdict = "conflict";
        rationale = "Provenance-complete current evidence remains contradictory.";
    } else if (!accepted.empty() && transaction_ready) {
        verdict = "insufficient";
        rationale = "Provenance-complete current evidence is not decisive.";
    } else if (structural_conflict || verdicts.contains("conflict") ||
               (verdicts.contains("support") && verdicts.contains("refute"))) {
        verdict = "conflict";
        rationale = "Independent accepted proposals remain semantically unresolved.";
    } else if (accepted.empty() ||
               verdicts == std::set<std::string>{"insufficient"}) {
        verdict = "insufficient";
        rationale = accepted.empty()
            ? "No structurally valid proposal survived main arbitration."
            : "Accepted proposals found no sufficient current evidence.";
    } else {
        const auto substantive = std::find_if(
            accepted.begin(), accepted.end(), [](const auto* proposal) {
                return proposal->verdict == "support" ||
                    proposal->verdict == "refute";
            });
        if (substantive == accepted.end()) {
            verdict = "insufficient";
            rationale = "Accepted proposals found no sufficient current evidence.";
        } else {
            verdict = (*substantive)->verdict;
            rationale = (*substantive)->rationale;
        }
    }

    std::optional<std::string> common_selected;
    std::optional<std::string> common_proposition;
    if (!accepted.empty() && transaction_ready && target_candidate != nullptr) {
        common_selected = target_candidate->episode_id;
        if (target_propositions.size() == 1)
            common_proposition = target_propositions.front();
    } else {
        if (selected_ids.size() == 1) common_selected = *selected_ids.begin();
        if (propositions.size() == 1) common_proposition = *propositions.begin();
    }
    const auto* selected_candidate = common_selected
        ? request.candidate(*common_selected)
        : nullptr;

    std::vector<CandidateDisposition> candidate_receipts;
    candidate_receipts.reserve(request.candidates.size());
    for (const auto& candidate : request.candidates) {
        const bool selected = common_selected &&
            candidate.episode_id == *common_selected;
        candidate_receipts.emplace_back(
            candidate.episode_id, selected, candidate.evidence_refs,
            candidate.source_addresses,
            candidate.source_addresses.empty()
                ? "unavailable_in_frozen_input" : "present",
            candidate.verification_state, candidate.revision,
            candidate.revision ? "present" : "unavailable_in_frozen_input",
            selected ? std::nullopt
                     : std::optional<std::string>("not_selected_by_main"));
    }
    std::vector<CurrentEvidenceDisposition> evidence_receipts;
    evidence_receipts.reserve(request.current_evidence.size());
    for (const auto& item : request.current_evidence) {
        evidence_receipts.emplace_back(
            item.evidence_ref, item.observation, item.source_kind,
            item.source_address, item.source_revision, item.source_family,
            item.context_hash, item.axis, item.verification_outcome,
            item.observed_at, item.expires_at, item.producer_id,
            item.producer_confidence, item.transaction_ready(),
            item.source_revision ? "present" : "unavailable_in_frozen_input");
    }
    const auto current_refs = request.current_evidence_refs();
    const std::vector<std::string> replay_refs = selected_candidate == nullptr
        ? std::vector<std::string>{}
        : selected_candidate->evidence_refs;
    MainReEvidenceReceipt receipt(
        "rozephine-main-re-evidence-receipt-v2", state_owner(*state),
        request.request_sha256, request.user_query, common_selected,
        common_proposition, verdict, rationale, current_refs,
        std::move(evidence_receipts), replay_refs,
        std::move(candidate_receipts), std::move(dispositions),
        std::move(executed_cores), fanout_used, elapsed_ns,
        verdict == "conflict", verdict == "insufficient",
        verdict == "conflict" || verdict == "insufficient",
        verdict == "support" || verdict == "refute");
    return MainReEvidenceResult<State>{std::move(state), std::move(receipt)};
}

template <typename State, typename CoreMap>
MainReEvidenceResult<State> run_manager(
    std::shared_ptr<const State> state,
    const PreparedReEvidenceRequest& request,
    const CoreMap& resident_cores,
    const std::span<const std::string> selected_cores) {
    std::set<std::string> selected_set(selected_cores.begin(), selected_cores.end());
    if (selected_cores.empty() || selected_set.size() != selected_cores.size())
        throw std::invalid_argument(
            "selected Re-evidence cores must be unique and nonempty");
    for (const auto& core : selected_set)
        if (!resident_cores.contains(core)) throw std::out_of_range(core);
    if (!state) throw std::invalid_argument("Re-evidence state must not be null");

    const auto before = persistent_snapshot(*state);
    const auto started = std::chrono::steady_clock::now();
    const auto execute = [&](const std::string& core_name) {
        auto detached = transient_snapshot(*state);
        auto proposal = resident_cores.at(core_name)(*detached, request);
        if (proposal.source != core_name)
            throw std::invalid_argument("Re-evidence core source identity changed");
        return proposal;
    };

    std::vector<ReEvidenceProposal> proposals;
    proposals.reserve(selected_cores.size());
    bool fanout_used = false;
    if (selected_cores.size() == 1) {
        proposals.push_back(execute(selected_cores.front()));
    } else {
        fanout_used = true;
        std::vector<std::future<ReEvidenceProposal>> futures;
        futures.reserve(selected_cores.size());
        for (const auto& core : selected_cores)
            futures.push_back(std::async(std::launch::async, execute, core));
        for (auto& future : futures) proposals.push_back(future.get());
    }
    if (!persistent_unchanged(*state, before))
        throw std::runtime_error(
            "Re-evidence worker mutated sole main persistent state");
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed < 0 ||
        static_cast<std::uint64_t>(elapsed) >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("Re-evidence elapsed time overflow");
    return arbitrate_re_evidence(
        std::move(state), request, proposals,
        std::vector<std::string>(selected_cores.begin(), selected_cores.end()),
        fanout_used, static_cast<std::int64_t>(elapsed));
}

}  // namespace

MainReEvidenceResult<WorldState> run_re_evidence_manager(
    std::shared_ptr<const WorldState> state,
    const PreparedReEvidenceRequest& request,
    const WorldReEvidenceCores& resident_cores,
    const std::span<const std::string> selected_cores) {
    return run_manager(std::move(state), request, resident_cores, selected_cores);
}

MainReEvidenceResult<CognitiveState> run_re_evidence_manager(
    std::shared_ptr<const CognitiveState> state,
    const PreparedReEvidenceRequest& request,
    const CognitiveReEvidenceCores& resident_cores,
    const std::span<const std::string> selected_cores) {
    return run_manager(std::move(state), request, resident_cores, selected_cores);
}

}  // namespace swegca::world
