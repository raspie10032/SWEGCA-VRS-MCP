#include "swegca_architecture/sha256.hpp"
#include "world/re_evidence_arbitration.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {

using swegca::world::ReEvidenceProposal;
using swegca::world::prepare_re_evidence_request;
using namespace swegca::world;

#define CHECK(expression) assert(expression)

std::string hex_digest(const swegca::architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2U] = digits[value >> 4U];
        result[index * 2U + 1U] = digits[value & 15U];
    }
    return result;
}

std::string digest(const std::string& text) {
    swegca::architecture::Sha256 sha;
    sha.update(text);
    return hex_digest(sha.finish());
}

const std::string legacy_request =
    R"({"user_query":" alpha beta ","user_query_role":"retrieval_cue_only","current_observation_evidence":[],"current_observation_evidence_present":false,"candidates":[{"episode_id":" episode:direct ","matched_cues":["alpha","beta"],"source_addresses":["memory:1","frame:before"],"revision":"r1","verification_state":"historical_only","steps":[{"evidence_refs":["memory:1","frame:before"],"outcome":"success","relations":["alpha supports beta"]},{"evidence_refs":["memory:1"],"outcome":"pending","relations":["alpha supports beta"]}]}],"historical_memory_role":"replayed_candidate_not_current_truth","authority":{"action":false,"persistent_write":0,"semantic_promotion":null,"extra":[]}})";

std::string complete_request(const bool include_binding = false,
                             const bool add_legacy = false) {
    std::string evidence =
        R"({"evidence_ref":"current:1","observation":"current observation","source_kind":"sensor_observation","source_address":"memory:1","source_revision":"r1","source_family":"sensor_visual","context_hash":"context:1","axis":"observational","verification_outcome":"support","observed_at":10,"producer_id":"capture:1","producer_confidence":1.0})";
    if (add_legacy) {
        evidence +=
            R"(,{"evidence_ref":"legacy:1","observation":"legacy observation","source_kind":"sensor_observation"})";
    }
    return std::string(
        R"({"user_query":"alpha beta","user_query_role":"retrieval_cue_only","current_observation_evidence":[)") +
        evidence +
        R"JSON(],"current_observation_evidence_present":true,"candidates":[{"episode_id":"episode:direct","matched_cues":["alpha"],"source_addresses":["memory:1"],"revision":"r1","verification_state":"historical_only","steps":[{"evidence_refs":["memory:1"],"outcome":"success","relations":["alpha supports beta"]}]},{"episode_id":"episode:other","matched_cues":["beta"],"source_addresses":["memory:2"],"revision":"r2","verification_state":"historical_only","steps":[{"evidence_refs":["memory:2"],"outcome":"failure","relations":["beta refutes alpha"]}]}],"historical_memory_role":"replayed_candidate_not_current_truth","authority":{"action":false,"persistent_write":false,"semantic_promotion":false})JSON" +
        (include_binding
             ? R"(,"current_observation_candidate_binding":"exact_source_address_and_revision","current_observation_target_episode_id":"episode:direct")"
             : "") +
        "}";
}

template<class Function>
void rejects(Function&& function, const std::string& expected) {
    try {
        function();
        CHECK(false);
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()).find(expected) != std::string::npos);
    }
}

void test_exact_byte_sha_and_legacy_request() {
    // This fixed oracle was produced by Python hashlib.sha256 over the exact
    // UTF-8 bytes above, matching the pinned implementation's cold boundary.
    CHECK(digest(legacy_request) ==
          "e0a92b94a1cafae2ec296dbb34d39349d87d74e7e243f4f0029e6e59ae27f7a8");
    const auto expected = digest(legacy_request);
    const auto request = prepare_re_evidence_request(
        legacy_request, "  " + expected + "\t");
    CHECK(request.request_sha256 == "  " + expected + "\t");
    CHECK(request.user_query == "alpha beta");
    CHECK(request.candidates.size() == 1U);
    CHECK(request.candidates[0].episode_id == "episode:direct");
    CHECK(request.candidates[0].propositions ==
          std::vector<std::string>{"alpha supports beta"});
    CHECK(request.candidates[0].historical_outcomes ==
          (std::vector<std::string>{"success", "pending"}));
    CHECK(request.candidates[0].evidence_refs ==
          (std::vector<std::string>{"memory:1", "frame:before"}));
    CHECK(request.candidate("episode:direct") != nullptr);
    CHECK(request.candidate("missing") == nullptr);
    CHECK(request.current_evidence_refs().empty());
    CHECK(!request.current_evidence_target_episode_id);

    std::string tampered = legacy_request;
    tampered.push_back(' ');
    rejects([&] { (void)prepare_re_evidence_request(tampered, expected); },
            "serialized Re-evidence request changed");
}

void test_complete_evidence_and_exact_binding() {
    const auto wire = complete_request(true);
    const auto request = prepare_re_evidence_request(wire, digest(wire));
    CHECK(request.current_evidence.size() == 1U);
    CHECK(request.current_evidence[0].transaction_ready());
    CHECK(request.current_evidence_target_episode_id ==
          std::optional<std::string>("episode:direct"));
    CHECK(request.current_evidence_refs() ==
          std::vector<std::string>{"current:1"});
    CHECK(request.current_observations() ==
          std::vector<std::string>{"current observation"});
    CHECK(request.current_source_kinds() ==
          std::vector<std::string>{"sensor_observation"});
    CHECK(request.current_source_addresses() ==
          std::vector<std::string>{"memory:1"});
}

void test_incomplete_and_mixed_provenance_rejected() {
    const auto mixed = complete_request(false, true);
    rejects([&] { (void)prepare_re_evidence_request(mixed, digest(mixed)); },
            "completeness must be uniform");

    auto partial = complete_request();
    const auto field = std::string(R"(,"source_revision":"r1")");
    const auto position = partial.find(field);
    CHECK(position != std::string::npos);
    partial.erase(position, field.size());
    rejects([&] { (void)prepare_re_evidence_request(partial, digest(partial)); },
            "partially specified");
}

void test_authority_and_presence_are_exact() {
    auto authority = legacy_request;
    const auto needle = std::string(R"("extra":[])");
    const auto position = authority.find(needle);
    CHECK(position != std::string::npos);
    authority.replace(position, needle.size(), R"("extra":[0])");
    rejects([&] {
        (void)prepare_re_evidence_request(authority, digest(authority));
    }, "grants forbidden authority");

    auto presence = legacy_request;
    const auto flag = std::string(
        R"("current_observation_evidence_present":false)");
    const auto flag_position = presence.find(flag);
    CHECK(flag_position != std::string::npos);
    presence.replace(flag_position, flag.size(),
                     R"("current_observation_evidence_present":0)");
    rejects([&] {
        (void)prepare_re_evidence_request(presence, digest(presence));
    }, "presence flag changed");
}

void test_validation_order_keeps_query_last() {
    auto wire = legacy_request;
    const auto query = std::string(R"("user_query":" alpha beta ")");
    const auto query_position = wire.find(query);
    CHECK(query_position != std::string::npos);
    wire.replace(query_position, query.size(), R"("user_query":" ")");
    const auto outcome = std::string(R"("outcome":"success")");
    const auto outcome_position = wire.find(outcome);
    CHECK(outcome_position != std::string::npos);
    wire.replace(outcome_position, outcome.size(), R"("outcome":"unknown")");
    rejects([&] { (void)prepare_re_evidence_request(wire, digest(wire)); },
            "unsupported historical outcome");
}

void test_proposal_contract_and_immutable_values() {
    static_assert(!std::is_copy_assignable_v<ReEvidenceProposal>);
    static_assert(!std::is_move_assignable_v<ReEvidenceProposal>);
    const ReEvidenceProposal proposal(
        " e2b ", " output:e2b ", " digest ", " episode:direct ",
        " alpha supports beta ", "support", " rationale ");
    // Python's frozen dataclass validates with _text but retains original values.
    CHECK(proposal.source == " e2b ");
    CHECK(proposal.proposition == " alpha supports beta ");

    rejects([] {
        (void)ReEvidenceProposal("e2b", "output:e2b", "digest", std::nullopt,
                                "alpha", "support", "reason");
    }, "only insufficient");
    rejects([] {
        (void)ReEvidenceProposal("e2b", "output:e2b", "digest", "episode:1",
                                "alpha", "unknown", "reason");
    }, "unsupported Re-evidence proposal verdict");
    rejects([] {
        (void)ReEvidenceProposal("e2b", "output:e2b", "digest", "episode:1",
                                "alpha", "support", "reason", false, true);
    }, "grants forbidden authority");
}

Tensor zeros(const std::vector<std::uint64_t>& shape) {
    std::size_t count = 1;
    for (const auto value : shape) count *= static_cast<std::size_t>(value);
    return Tensor(TensorDType::float32, shape,
                  std::vector<double>(count, 0.0));
}

std::shared_ptr<const WorldState> world_state() {
    return std::make_shared<const WorldState>(
        zeros({1, 32, 4}), BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 0)),
        BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 0)), "sole-main-test");
}

ReEvidenceProposal proposal(
    const PreparedReEvidenceRequest& request, std::string source = "e2b",
    std::string verdict = "insufficient",
    std::optional<std::string> selected = "episode:direct",
    std::string proposition = "alpha supports beta") {
    return ReEvidenceProposal(
        std::move(source), "output:worker", request.request_sha256,
        std::move(selected), std::move(proposition), std::move(verdict),
        "worker rationale");
}

void test_single_manager_and_main_semantic_authority() {
    const auto complete_wire = complete_request(true);
    const auto complete = prepare_re_evidence_request(
        complete_wire, digest(complete_wire));
    const auto state = world_state();
    const WorldReEvidenceCores cores{{
        "e2b", [&](WorldState&, const PreparedReEvidenceRequest&) {
            return proposal(complete, "e2b", "refute", "episode:direct",
                            "worker invented relation");
        }}};
    const std::vector<std::string> selected{"e2b"};
    const auto result = run_re_evidence_manager(state, complete, cores, selected);
    CHECK(result.state == state);
    CHECK(result.receipt.state_owner == "sole-main-test");
    CHECK(result.receipt.executed_cores == selected);
    CHECK(!result.receipt.fanout_used);
    CHECK(result.receipt.proposals.size() == 1);
    CHECK(result.receipt.proposals[0].accepted);
    CHECK(result.receipt.verdict == "support");
    CHECK(result.receipt.proposition ==
          std::optional<std::string>("alpha supports beta"));
    CHECK(result.receipt.semantic_judgment_formed);
    CHECK(!result.receipt.should_abstain);
    CHECK(result.receipt.current_evidence[0].transaction_ready);
    CHECK(result.receipt.replay_evidence_refs ==
          std::vector<std::string>{"memory:1"});

    const auto wrong = ReEvidenceProposal(
        "e2b", "output:worker", complete.request_sha256, "episode:other",
        "anything", "support", "wrong subject");
    const WorldReEvidenceCores wrong_core{{
        "e2b", [&](WorldState&, const PreparedReEvidenceRequest&) { return wrong; }}};
    const auto rejected = run_re_evidence_manager(state, complete, wrong_core, selected);
    CHECK(rejected.receipt.verdict == "insufficient");
    CHECK(rejected.receipt.proposals[0].rejection_reason ==
          std::optional<std::string>(
              "selected_candidate_not_bound_to_current_evidence"));
}

void test_absent_current_abstention_and_parallel_order() {
    const auto legacy = prepare_re_evidence_request(
        legacy_request, digest(legacy_request));
    const auto state = world_state();
    const std::vector<std::string> one{"e2b"};
    const WorldReEvidenceCores single{{
        "e2b", [&](WorldState& detached, const PreparedReEvidenceRequest&) {
            detached = WorldState(
                Tensor(TensorDType::float32, {1, 32, 4},
                       std::vector<double>(128, 1.0)),
                BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 1)),
                BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 1)),
                "changed-snapshot");
            return proposal(legacy);
        }}};
    const auto abstained = run_re_evidence_manager(state, legacy, single, one);
    CHECK(abstained.state == state);
    CHECK(abstained.receipt.verdict == "insufficient");
    CHECK(abstained.receipt.should_abstain);
    CHECK(std::all_of(state->semantic_slots().values().begin(),
                      state->semantic_slots().values().end(),
                      [](const double value) { return value == 0.0; }));

    const PreparedReEvidenceRequest current(
        "digest", "alpha beta",
        {ReEvidenceCandidate(
            "episode:direct", {"alpha"}, {"alpha supports beta"}, {"success"},
            {"memory:1"}, {}, "historical_only", std::nullopt)},
        {ReEvidenceCurrentEvidence(
            "current:1", "current observation", "sensor_observation")},
        std::nullopt);
    std::latch both_started(2);
    const WorldReEvidenceCores parallel{
        {"positive", [&](WorldState&, const PreparedReEvidenceRequest&) {
             both_started.count_down(); both_started.wait();
             std::this_thread::sleep_for(std::chrono::milliseconds(10));
             return proposal(current, "positive", "support");
         }},
        {"negative", [&](WorldState&, const PreparedReEvidenceRequest&) {
             both_started.count_down(); both_started.wait();
             return proposal(current, "negative", "refute");
         }}};
    const std::vector<std::string> order{"positive", "negative"};
    const auto conflict = run_re_evidence_manager(state, current, parallel, order);
    CHECK(conflict.receipt.fanout_used);
    CHECK(conflict.receipt.executed_cores == order);
    CHECK(conflict.receipt.proposals[0].source == "positive");
    CHECK(conflict.receipt.proposals[1].source == "negative");
    CHECK(conflict.receipt.verdict == "conflict");
    CHECK(conflict.receipt.unresolved_conflict);

    auto mutable_main = std::make_shared<WorldState>(
        zeros({1, 32, 4}), BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 0)),
        BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 0)), "mutable-main");
    const std::shared_ptr<const WorldState> guarded_main = mutable_main;
    const WorldReEvidenceCores alias_mutator{{
        "e2b", [&](WorldState&, const PreparedReEvidenceRequest&) {
            *mutable_main = WorldState(
                Tensor(TensorDType::float32, {1, 32, 4},
                       std::vector<double>(128, 1.0)),
                BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 0)),
                BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 0)),
                "mutated-main");
            return proposal(legacy);
        }}};
    try {
        (void)run_re_evidence_manager(guarded_main, legacy, alias_mutator, one);
        CHECK(false);
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()).find("mutated sole main") !=
              std::string::npos);
    }
}

void test_manager_execution_gates_and_cognitive_overload() {
    const auto request = prepare_re_evidence_request(
        legacy_request, digest(legacy_request));
    const auto state = world_state();
    const WorldReEvidenceCores cores{{
        "e2b", [&](WorldState&, const PreparedReEvidenceRequest&) {
            return proposal(request, "wrong-source");
        }}};
    rejects([&] {
        const std::vector<std::string> selected{"e2b"};
        (void)run_re_evidence_manager(state, request, cores, selected);
    }, "source identity changed");
    try {
        const std::vector<std::string> selected{"missing"};
        (void)run_re_evidence_manager(state, request, cores, selected);
        CHECK(false);
    } catch (const std::out_of_range& error) {
        CHECK(std::string(error.what()).find("missing") != std::string::npos);
    }
    rejects([&] {
        const std::vector<std::string> selected{"e2b", "e2b"};
        (void)run_re_evidence_manager(state, request, cores, selected);
    }, "unique and nonempty");

    const auto cognitive = std::make_shared<const CognitiveState>(
        zeros({1, 20, 4}), zeros({1, 6, 4}), zeros({1, 6, 4}));
    const CognitiveReEvidenceCores cognitive_cores{{
        "e2b", [&](CognitiveState&, const PreparedReEvidenceRequest&) {
            return proposal(request);
        }}};
    const std::vector<std::string> selected{"e2b"};
    const auto result = run_re_evidence_manager(
        cognitive, request, cognitive_cores, selected);
    CHECK(result.state == cognitive);
    CHECK(result.receipt.state_owner == cognitive->owner_id());
}

}  // namespace

int main() {
    test_exact_byte_sha_and_legacy_request();
    test_complete_evidence_and_exact_binding();
    test_incomplete_and_mixed_provenance_rejected();
    test_authority_and_presence_are_exact();
    test_validation_order_keeps_query_last();
    test_proposal_contract_and_immutable_values();
    test_single_manager_and_main_semantic_authority();
    test_absent_current_abstention_and_parallel_order();
    test_manager_execution_gates_and_cognitive_overload();
    std::cout << "re_evidence_arbitration_tests: ok\n";
}
