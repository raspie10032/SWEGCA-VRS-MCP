#include "world/cognitive_state.hpp"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

Tensor tensor(const TensorDType dtype, const std::uint64_t batch, const std::uint64_t slots,
              const std::uint64_t hidden, const double offset = 0,
              std::string device = "cpu") {
    std::vector<double> values(static_cast<std::size_t>(batch * slots * hidden));
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = offset + static_cast<double>(index);
    }
    return Tensor(dtype, {batch, slots, hidden}, std::move(values), std::move(device));
}

StructuredWorldGraph graph() {
    return StructuredWorldGraph(
        {WorldEntity("actor_01", "character", {{"hair", "blonde_twintail"}}, {},
                     {"mesh://actor_01.glb"}),
         WorldEntity("umbrella_01", "prop", {{"state", "open"}})},
        {WorldRelation("actor_01", "holds", "umbrella_01")});
}

CognitiveState state(const TensorDType dtype = TensorDType::float32) {
    return CognitiveState(
        tensor(dtype, 1, 4, 8), tensor(dtype, 1, 2, 8, 100),
        tensor(dtype, 1, 2, 8, 200), graph(), {"image://input/001.png"},
        {{"active_goal", "track_actor"}}, {{"consistency", 0.9}},
        {{"identity", "rozephine"}});
}

void test_one_state_and_round_trip() {
    auto original = state();
    original.validate(CognitiveKernelConfig{4, 2, 2, 8});
    CHECK(original.persistent_state_count() == 1);
    CHECK(original.owner_id() == "rozephine_cognitive_core_v0_2");

    const auto wire = original.to_dict();
    const auto& object = wire.as_object();
    CHECK(object.size() == 9);
    for (const auto key : {"semantic_slots", "executive_slots", "scratch_slots",
                           "structured_world_graph", "evidence_refs", "goal_state",
                           "value_state", "self_state", "owner_id"}) {
        CHECK(object.contains(key));
    }
    CHECK(wire.at("semantic_slots").as_object().size() == 2);
    CHECK(wire.at("semantic_slots").at("dtype").as_string() == "float32");
    CHECK(wire.at("semantic_slots").as_object().contains("values"));
    CHECK(!wire.at("semantic_slots").as_object().contains("device"));
    CHECK(!wire.at("semantic_slots").as_object().contains("stride"));

    auto restored = CognitiveState::from_dict(wire);
    restored.validate(CognitiveKernelConfig{4, 2, 2, 8});
    CHECK(restored.exact_equal(original));
    CHECK(restored.structured_world_graph() == original.structured_world_graph());
    CHECK(restored.goal_state() == original.goal_state());
    CHECK(restored.value_state() == original.value_state());
    CHECK(restored.self_state() == original.self_state());
}

void test_all_supported_dtypes() {
    for (const auto dtype : {TensorDType::bfloat16, TensorDType::float16,
                             TensorDType::float32, TensorDType::float64}) {
        const auto original = state(dtype);
        const auto restored = CognitiveState::from_dict(original.to_dict());
        CHECK(restored.exact_equal(original));
    }
    auto payload = state().to_dict();
    auto object = payload.as_object();
    auto semantic = object.at("semantic_slots").as_object();
    semantic["dtype"] = "int64";
    object["semantic_slots"] = std::move(semantic);
    rejects([&] { static_cast<void>(CognitiveState::from_dict(object)); },
            "unsupported cognitive state dtype");
}

double stored(const TensorDType dtype, const double value) {
    return Tensor(dtype, {1}, {value}).values()[0];
}

void test_tensor_dtype_storage_canonicalization() {
    for (const auto dtype : {TensorDType::bfloat16, TensorDType::float16,
                             TensorDType::float32, TensorDType::float64}) {
        const double positive_zero = stored(dtype, 0.0);
        const double negative_zero = stored(dtype, -0.0);
        const double positive_infinity =
            stored(dtype, std::numeric_limits<double>::infinity());
        const double negative_infinity =
            stored(dtype, -std::numeric_limits<double>::infinity());
        const double positive_nan =
            stored(dtype, std::numeric_limits<double>::quiet_NaN());
        const double negative_nan =
            stored(dtype, -std::numeric_limits<double>::quiet_NaN());
        CHECK(positive_zero == 0.0 && !std::signbit(positive_zero));
        CHECK(negative_zero == 0.0 && std::signbit(negative_zero));
        CHECK(std::isinf(positive_infinity) && !std::signbit(positive_infinity));
        CHECK(std::isinf(negative_infinity) && std::signbit(negative_infinity));
        CHECK(std::isnan(positive_nan) && !std::signbit(positive_nan));
        CHECK(std::isnan(negative_nan) && std::signbit(negative_nan));
    }

    CHECK(stored(TensorDType::float64, 16'777'217.0) == 16'777'217.0);
    CHECK(stored(TensorDType::float64, std::numeric_limits<double>::denorm_min()) ==
          std::numeric_limits<double>::denorm_min());
    CHECK(stored(TensorDType::float64, std::numeric_limits<double>::max()) ==
          std::numeric_limits<double>::max());
    CHECK(stored(TensorDType::float32, 16'777'217.0) == 16'777'216.0);
    CHECK(stored(TensorDType::float32, 16'777'219.0) == 16'777'220.0);
    const double float_minimum_subnormal =
        static_cast<double>(std::numeric_limits<float>::denorm_min());
    CHECK(stored(TensorDType::float32, float_minimum_subnormal) ==
          float_minimum_subnormal);
    CHECK(stored(TensorDType::float32, std::ldexp(1.0, -150)) == 0.0);
    CHECK(std::signbit(stored(TensorDType::float32, -std::ldexp(1.0, -150))));
    CHECK(stored(TensorDType::float32,
                 static_cast<double>(std::numeric_limits<float>::max())) ==
          static_cast<double>(std::numeric_limits<float>::max()));

    CHECK(stored(TensorDType::float16, 1.0 + std::ldexp(1.0, -11)) == 1.0);
    CHECK(stored(TensorDType::float16, 1.0 + 3.0 * std::ldexp(1.0, -11)) ==
          1.0 + std::ldexp(1.0, -9));
    CHECK(stored(TensorDType::bfloat16, 1.0 + std::ldexp(1.0, -8)) == 1.0);
    CHECK(stored(TensorDType::bfloat16, 1.0 + 3.0 * std::ldexp(1.0, -8)) ==
          1.0 + std::ldexp(1.0, -6));

    const double half_minimum_subnormal = std::ldexp(1.0, -24);
    CHECK(stored(TensorDType::float16, half_minimum_subnormal) ==
          half_minimum_subnormal);
    CHECK(stored(TensorDType::float16, std::ldexp(1.0, -25)) == 0.0);
    CHECK(stored(TensorDType::float16,
                 std::nextafter(std::ldexp(1.0, -25), 1.0)) ==
          half_minimum_subnormal);

    const double bfloat_minimum_subnormal = std::ldexp(1.0, -133);
    CHECK(stored(TensorDType::bfloat16, bfloat_minimum_subnormal) ==
          bfloat_minimum_subnormal);
    CHECK(stored(TensorDType::bfloat16, std::ldexp(1.0, -134)) == 0.0);
    CHECK(stored(TensorDType::bfloat16,
                 std::nextafter(std::ldexp(1.0, -134), 1.0)) ==
          bfloat_minimum_subnormal);

    CHECK(stored(TensorDType::float16, 65'504.0) == 65'504.0);
    CHECK(std::isinf(stored(TensorDType::float16, 65'520.0)));
    const double bfloat_maximum = std::ldexp(2.0 - std::ldexp(1.0, -7), 127);
    CHECK(stored(TensorDType::bfloat16, bfloat_maximum) == bfloat_maximum);
    CHECK(std::isinf(stored(TensorDType::bfloat16,
                            bfloat_maximum + std::ldexp(1.0, 119))));

    const Tensor original(
        TensorDType::float32, {1, 4},
        {16'777'217.0, -0.0, std::numeric_limits<double>::infinity(),
         std::numeric_limits<double>::quiet_NaN()});
    const Tensor restored = Tensor::from_json_payload(original.to_json_payload());
    CHECK(restored.exact_equal(original));
    CHECK(original.values()[0] == 16'777'216.0);
}

void test_shape_dtype_device_and_owner_validation() {
    rejects(
        [] {
            CognitiveState invalid(Tensor(TensorDType::float32, {1, 4}, std::vector<double>(4)),
                                   tensor(TensorDType::float32, 1, 2, 2),
                                   tensor(TensorDType::float32, 1, 2, 2));
        },
        "shape [batch, slots, dim]");
    rejects(
        [] {
            CognitiveState invalid(tensor(TensorDType::float32, 1, 4, 8),
                                   tensor(TensorDType::float32, 2, 2, 8),
                                   tensor(TensorDType::float32, 1, 2, 8));
        },
        "share batch and hidden dims");
    rejects(
        [] {
            CognitiveState invalid(tensor(TensorDType::float32, 1, 4, 8),
                                   tensor(TensorDType::float16, 1, 2, 8),
                                   tensor(TensorDType::float32, 1, 2, 8));
        },
        "share dtype");
    rejects(
        [] {
            CognitiveState invalid(tensor(TensorDType::float32, 1, 4, 8, 0, "cpu"),
                                   tensor(TensorDType::float32, 1, 2, 8, 0, "cuda:0"),
                                   tensor(TensorDType::float32, 1, 2, 8, 0, "cpu"));
        },
        "share device");
    rejects([] { state().validate(CognitiveKernelConfig{}); }, "unexpected cognitive slot shapes");
    rejects(
        [] {
            CognitiveState invalid(tensor(TensorDType::float32, 1, 4, 8),
                                   tensor(TensorDType::float32, 1, 2, 8),
                                   tensor(TensorDType::float32, 1, 2, 8), {}, {}, {}, {}, {},
                                   "   ");
        },
        "owner_id must not be empty");
}

void test_graph_integrity_and_round_trip() {
    const auto original = graph();
    CHECK(StructuredWorldGraph::from_json(original.to_json()) == original);
    rejects(
        [] {
            StructuredWorldGraph duplicate(
                {WorldEntity("actor", "character"), WorldEntity("actor", "prop")});
        },
        "unique");
    rejects(
        [] {
            StructuredWorldGraph unknown({WorldEntity("actor", "character")},
                                         {WorldRelation("actor", "holds", "missing")});
        },
        "unknown entities");
    rejects([] { WorldEntity invalid(" ", "character"); }, "entity_id must not be empty");
    rejects([] { WorldEntity invalid("actor", "character", {}, {}, {""}); },
            "empty addresses");
}

void test_deep_ownership_clone_and_input_nonmutation() {
    std::vector<double> semantic_values(32, 1.0);
    JsonValue::Object goal{{"nested", JsonValue::Object{{"value", 7}}}};
    std::vector<std::string> refs{"ref:a", ""};
    CognitiveState owned(Tensor(TensorDType::float32, {1, 4, 8}, semantic_values),
                         tensor(TensorDType::float32, 1, 2, 8),
                         tensor(TensorDType::float32, 1, 2, 8), {}, refs, goal);
    const auto before = owned.to_dict();

    semantic_values[0] = 99;
    goal["nested"] = "changed";
    refs[0] = "changed";
    CHECK(owned.to_dict() == before);
    CHECK(owned.evidence_refs().size() == 2);
    CHECK(owned.evidence_refs()[1].empty());  // CognitiveState itself does not reject this.

    const auto copied = owned;
    const auto cloned = owned.clone();
    CHECK(copied.exact_equal(owned));
    CHECK(cloned.exact_equal(owned));
    CHECK(owned.to_dict() == before);

    const auto roundtrip = CognitiveState::from_dict(before);
    CHECK(roundtrip.exact_equal(owned));
    CHECK(owned.to_dict() == before);
}

}  // namespace

int main() {
    test_one_state_and_round_trip();
    test_all_supported_dtypes();
    test_tensor_dtype_storage_canonicalization();
    test_shape_dtype_device_and_owner_validation();
    test_graph_integrity_and_round_trip();
    test_deep_ownership_clone_and_input_nonmutation();
    std::cout << "cognitive state tests passed\n";
}
