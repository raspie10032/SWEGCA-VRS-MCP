#include "world/hypothesis_proposer.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace swegca::world;

namespace {

CognitiveEvent event(std::string id, std::string representation,
                     JsonValue value, const double confidence,
                     std::string predicate = "표식_색상",
                     std::optional<std::string> subject = "표식-1") {
    const auto reference = "ref:" + id;
    return CognitiveEvent(
        id, "한국어_관측",
        EventSource(representation, representation + "_adapter", reference),
        {EvidenceClaim(std::move(predicate), std::move(value), confidence,
                       std::move(subject))},
        {reference}, EvidenceKind::observed_evidence);
}

}  // namespace

int main() {
    {
        const std::vector<CognitiveEvent> events{
            event("ocr", "screen_ocr", "청람빛", 0.92),
            event("text", "korean_text", "청람빛", 0.88),
            event("distractor", "screen_ocr", "삼각형", 0.99, "표식_모양")};
        const auto candidates = propose_hypotheses(events);
        assert(candidates.size() == 2);
        const auto& top = candidates.front();
        assert(top.hypothesis_id == "hypothesis:adae4d45bb711df38127");
        assert(top.value == JsonValue("청람빛"));
        assert(std::abs(top.confidence - 0.9) < 1e-15);
        assert((top.source_families ==
                std::vector<std::string>{"screen_ocr", "korean_text"}));
        assert((top.evidence_refs == std::vector<std::string>{"ref:ocr", "ref:text"}));
        assert(!top.has_conflict());
        const EvidenceAccumulatorConfig accumulator_config;
        const HypothesisProposerConfig proposer_config;
        const auto plan = plan_evidence_request(
            top, nullptr, accumulator_config, proposer_config);
        assert((plan.requested_axes ==
                std::vector<std::string>{"observational", "counterfactual"}));
        assert(plan.reason == "fill_weakest_evidence_axes");
    }
    {
        const std::vector<CognitiveEvent> events{
            event("ocr", "screen_ocr", "청람빛", 0.9),
            event("text", "korean_text", "붉은빛", 0.9)};
        const auto candidates = propose_hypotheses(events);
        assert(candidates.size() == 2);
        assert(candidates.front().hypothesis_id ==
               "hypothesis:76995b10f88449702ec6");
        assert(candidates.front().has_conflict());
        assert(candidates.front().conflicting_values ==
               std::vector<JsonValue>{JsonValue("청람빛")});
        const EvidenceAccumulatorConfig accumulator_config;
        const HypothesisProposerConfig proposer_config;
        const auto plan = plan_evidence_request(
            candidates.front(), nullptr, accumulator_config, proposer_config);
        assert(plan.requested_axes.front() == "counterfactual");
        assert(plan.reason == "resolve_conflicting_values");
        const auto external = plan_evidence_request(
            candidates.front(), nullptr, accumulator_config,
            HypothesisProposerConfig(0.5, 2, 0.5, 8, 1), true);
        assert(external.requested_axes ==
               std::vector<std::string>{"counterfactual"});
        assert(external.reason == "resolve_external_refutation");
    }
    {
        const std::vector<CognitiveEvent> events{
            event("text", "korean_text", "청람빛", 0.9)};
        const auto candidate = propose_hypotheses(events).front();
        const EvidenceAccumulatorConfig accumulator_config(
            0.2, 0.25, 0.9, 1.0, 1.0, 2);
        auto state = EvidenceAccumulatorState::empty(
            candidate.hypothesis_id, accumulator_config);
        for (int index = 0; index != 2; ++index) {
            EvidenceObservation observation(
                candidate.hypothesis_id,
                "counterfactual:" + std::to_string(index),
                "source:" + std::to_string(index),
                "context:" + std::to_string(index),
                "counterfactual", "refute", index, std::nullopt,
                "counterfactual-producer:" + std::to_string(index));
            state = update_accumulator(
                state, observation, accumulator_config, index).state;
        }
        const auto plan = plan_evidence_request(
            candidate, state.get(), accumulator_config,
            HypothesisProposerConfig(), true);
        assert((plan.requested_axes ==
                std::vector<std::string>{"observational", "intervention"}));
    }
    {
        const std::vector<CognitiveEvent> ignored{
            event("low", "text", "x", 0.49),
            event("ignored", "text", "x", 1.0, "candidate_entities")};
        assert(propose_hypotheses(ignored).empty());
    }
    {
        const std::vector<std::pair<JsonValue, std::string>> values{
            {JsonValue(1), "hypothesis:c41b7f37496ca2a58026"},
            {JsonValue(1.0), "hypothesis:43efcdb96eea2daa4295"},
            {JsonValue(-0.0), "hypothesis:0b385cbb091a6ddd05a5"},
            {JsonValue(1.0e-7), "hypothesis:518bdcfdaba9c6f88e05"},
            {JsonValue::Object{{"b", true},
                               {"a", JsonValue::Array{"x\n", nullptr, 2.5}}},
             "hypothesis:1622597b0ad2755e2553"}};
        for (std::size_t index = 0; index != values.size(); ++index) {
            const std::vector<CognitiveEvent> events{
                event(std::to_string(index), "r", values[index].first, 1.0,
                      "p", std::nullopt)};
            const auto observed = propose_hypotheses(events).front().hypothesis_id;
            if (observed != values[index].second) {
                std::cerr << "canonical JSON fixture mismatch index=" << index
                          << " observed=" << observed
                          << " expected=" << values[index].second << '\n';
            }
            assert(observed == values[index].second);
        }
    }
    assert(hypothesis_proposer_source_sha256() ==
           "5b4d0bd0c5f086ea30240d9d70dc868f076c40bd68377ddbfa340df6261c199d");
    std::cout << "hypothesis proposer tests passed\n";
}
