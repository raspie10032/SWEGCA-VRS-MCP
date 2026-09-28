#include "world/cognitive_event.hpp"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void rejects(const std::function<void()>& operation, const std::string_view expected) {
    try {
        operation();
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()).find(expected) != std::string_view::npos);
        return;
    }
    CHECK(false);
}

CognitiveEvent event() {
    return CognitiveEvent(
        "evt_0001", "observation",
        EventSource("image", "vision_bridge_v0", "image://input/001.png"),
        {EvidenceClaim("has_color", "red", 0.97, "umbrella_01"),
         EvidenceClaim("is_open", true, 1.0)},
        {"image://input/001.png"}, EvidenceKind::observed_evidence,
        {{"nested", JsonValue::Object{{"frame", 3}, {"valid", true}}}});
}

void test_exact_enum_strings() {
    const std::pair<EvidenceKind, std::string_view> cases[]{
        {EvidenceKind::learned_prediction, "learned_prediction"},
        {EvidenceKind::observed_evidence, "observed_evidence"},
        {EvidenceKind::deterministic_simulation, "deterministic_simulation"},
        {EvidenceKind::user_claim, "user_claim"},
        {EvidenceKind::external_model_claim, "external_model_claim"},
    };
    for (const auto& [kind, name] : cases) {
        CHECK(evidence_kind_name(kind) == name);
        CHECK(evidence_kind_from_name(name) == kind);
    }
    rejects([] { static_cast<void>(evidence_kind_from_name("observed")); },
            "unsupported evidence kind");
}

void test_event_round_trip_and_exact_fields() {
    const auto original = event();
    const auto wire = original.to_dict();
    const auto& object = wire.as_object();
    CHECK(object.size() == 7);
    for (const auto key : {"event_id", "event_type", "source", "claims", "evidence_refs",
                           "evidence_kind", "metadata"}) {
        CHECK(object.contains(key));
    }
    CHECK(wire.at("source").as_object().size() == 3);
    CHECK(wire.at("claims").as_array().size() == 2);
    CHECK(wire.at("claims").as_array()[0].as_object().size() == 4);
    CHECK(wire.at("claims").as_array()[1].as_object().contains("subject"));
    CHECK(std::holds_alternative<std::nullptr_t>(
        wire.at("claims").as_array()[1].at("subject").storage()));
    CHECK(wire.at("evidence_kind").as_string() == "observed_evidence");

    const auto restored = CognitiveEvent::from_dict(wire);
    CHECK(restored == original);
    CHECK(restored.event_id() == "evt_0001");
    CHECK(restored.event_type() == "observation");
    CHECK(restored.source().source_ref == "image://input/001.png");
    CHECK(restored.evidence_refs().size() == 1);
    CHECK(restored.claims().size() == 2);
    CHECK(restored.evidence_kind() == EvidenceKind::observed_evidence);
}

void test_source_and_claim_validation() {
    rejects([] { EventSource invalid(" ", "adapter", "ref"); },
            "representation must not be empty");
    rejects([] { EventSource invalid("image", "\t", "ref"); }, "adapter must not be empty");
    rejects([] { EventSource invalid("image", "adapter", "\n"); },
            "source_ref must not be empty");
    rejects([] { EvidenceClaim invalid(" ", true, 0.5); }, "predicate must not be empty");
    rejects([] { EvidenceClaim invalid("p", true, 0.5, "\t"); },
            "subject must not be empty");
    for (const auto confidence : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN()}) {
        rejects([&] { EvidenceClaim invalid("p", true, confidence); },
                "confidence must be finite and within [0, 1]");
    }
    CHECK(EvidenceClaim("p", true, 0.0).confidence == 0.0);
    CHECK(EvidenceClaim("p", true, 1.0).confidence == 1.0);

    auto source = EventSource("image", "adapter", "ref").to_dict().as_object();
    source["extra"] = true;
    rejects([&] { static_cast<void>(EventSource::from_dict(source)); }, "unexpected object field");
    auto claim = EvidenceClaim("p", true, 0.5).to_dict().as_object();
    claim["extra"] = true;
    rejects([&] { static_cast<void>(EvidenceClaim::from_dict(claim)); },
            "unexpected object field");
}

void test_event_provenance_validation() {
    const EventSource source("image", "adapter", "ref");
    rejects(
        [&] {
            CognitiveEvent invalid(" ", "observation", source, {}, {"ref"},
                                   EvidenceKind::observed_evidence);
        },
        "event_id must not be empty");
    rejects(
        [&] {
            CognitiveEvent invalid("event", "\t", source, {}, {"ref"},
                                   EvidenceKind::observed_evidence);
        },
        "event_type must not be empty");
    rejects(
        [&] {
            CognitiveEvent invalid("event", "observation", source, {}, {},
                                   EvidenceKind::observed_evidence);
        },
        "at least one provenance address");
    for (const auto reference : {"", " ", "\t\n"}) {
        rejects(
            [&] {
                CognitiveEvent invalid("event", "observation", source, {}, {reference},
                                       EvidenceKind::observed_evidence);
            },
            "empty addresses");
    }
}

void test_deep_owned_claim_value_and_metadata() {
    JsonValue claim_value = JsonValue::Object{
        {"labels", JsonValue::Array{"red", "open"}}, {"score", 7}};
    JsonValue::Object metadata{{"source", JsonValue::Object{{"frame", 1}}}};
    CognitiveEvent owned("event", "observation", EventSource("image", "adapter", "ref"),
                         {EvidenceClaim("describes", claim_value, 0.8)}, {"ref"},
                         EvidenceKind::learned_prediction, metadata);
    const auto before = owned.to_dict();

    claim_value = "changed";
    metadata["source"] = "changed";
    CHECK(owned.to_dict() == before);
    const auto copied = owned;
    CHECK(copied == owned);
    CHECK(CognitiveEvent::from_dict(before) == owned);
    CHECK(owned.to_dict() == before);
}

void test_from_dict_defaults() {
    JsonValue::Object payload{{"event_id", "event"},
                              {"event_type", "observation"},
                              {"source", EventSource("image", "adapter", "ref").to_dict()},
                              {"evidence_refs", JsonValue::Array{"ref"}},
                              {"evidence_kind", "user_claim"}};
    const auto restored = CognitiveEvent::from_dict(payload);
    CHECK(restored.claims().empty());
    CHECK(restored.metadata().empty());
}

}  // namespace

int main() {
    test_exact_enum_strings();
    test_event_round_trip_and_exact_fields();
    test_source_and_claim_validation();
    test_event_provenance_validation();
    test_deep_owned_claim_value_and_metadata();
    test_from_dict_defaults();
    std::cout << "cognitive event tests passed\n";
}
