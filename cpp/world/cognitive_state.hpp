#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

struct JsonInteger final {
    std::string value;
    friend bool operator==(const JsonInteger&, const JsonInteger&) = default;
};

class JsonValue final {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Storage =
        std::variant<std::nullptr_t, bool, std::int64_t, JsonInteger,
                     double, std::string, Array, Object>;

    JsonValue() noexcept;
    JsonValue(std::nullptr_t) noexcept;
    JsonValue(bool value) noexcept;
    JsonValue(std::int64_t value) noexcept;
    JsonValue(JsonInteger value);
    JsonValue(int value) noexcept;
    JsonValue(double value) noexcept;
    JsonValue(std::string value);
    JsonValue(std::string_view value);
    JsonValue(const char* value);
    JsonValue(Array value);
    JsonValue(Object value);

    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] std::string_view as_string() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const JsonValue& at(std::string_view key) const;
    [[nodiscard]] const Storage& storage() const noexcept;

    friend bool operator==(const JsonValue&, const JsonValue&) = default;

private:
    Storage storage_;
};

enum class TensorDType : std::uint8_t {
    bfloat16,
    float16,
    float32,
    float64,
};

class Tensor final {
public:
    Tensor(TensorDType dtype, std::vector<std::uint64_t> shape,
           std::vector<double> values, std::string device = "cpu");

    [[nodiscard]] TensorDType dtype() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> shape() const noexcept;
    [[nodiscard]] std::span<const double> values() const noexcept;
    [[nodiscard]] std::string_view device() const noexcept;
    [[nodiscard]] std::size_t rank() const noexcept;
    [[nodiscard]] const void* storage_identity() const noexcept;
    [[nodiscard]] Tensor clone() const;
    [[nodiscard]] bool exact_equal(const Tensor& other) const noexcept;
    [[nodiscard]] JsonValue to_json_payload() const;
    [[nodiscard]] static Tensor from_json_payload(const JsonValue& payload);

private:
    TensorDType dtype_;
    std::vector<std::uint64_t> shape_;
    std::shared_ptr<std::vector<double>> values_;
    std::string device_;
};

struct WorldEntity final {
    std::string entity_id;
    std::string entity_type;
    JsonValue::Object properties;
    JsonValue::Object spatial;
    std::vector<std::string> evidence_refs;

    WorldEntity(std::string entity_id, std::string entity_type,
                JsonValue::Object properties = {}, JsonValue::Object spatial = {},
                std::vector<std::string> evidence_refs = {});
    [[nodiscard]] JsonValue to_json() const;
    [[nodiscard]] static WorldEntity from_json(const JsonValue& payload);
    friend bool operator==(const WorldEntity&, const WorldEntity&) = default;
};

struct WorldRelation final {
    std::string subject;
    std::string predicate;
    std::string object;
    JsonValue::Object properties;

    WorldRelation(std::string subject, std::string predicate, std::string object,
                  JsonValue::Object properties = {});
    [[nodiscard]] JsonValue to_json() const;
    [[nodiscard]] static WorldRelation from_json(const JsonValue& payload);
    friend bool operator==(const WorldRelation&, const WorldRelation&) = default;
};

class StructuredWorldGraph final {
public:
    StructuredWorldGraph(std::vector<WorldEntity> entities = {},
                         std::vector<WorldRelation> relations = {});

    [[nodiscard]] std::span<const WorldEntity> entities() const noexcept;
    [[nodiscard]] std::span<const WorldRelation> relations() const noexcept;
    [[nodiscard]] JsonValue to_json() const;
    [[nodiscard]] static StructuredWorldGraph from_json(const JsonValue& payload);
    friend bool operator==(const StructuredWorldGraph&, const StructuredWorldGraph&) = default;

private:
    std::vector<WorldEntity> entities_;
    std::vector<WorldRelation> relations_;
};

struct CognitiveKernelConfig final {
    std::uint64_t semantic_slots{256};
    std::uint64_t executive_slots{32};
    std::uint64_t scratch_slots{32};
    std::uint64_t hidden_dim{2048};

    void validate() const;
};

class CognitiveState final {
public:
    static constexpr std::string_view default_owner = "rozephine_cognitive_core_v0_2";

    CognitiveState(Tensor semantic_slots, Tensor executive_slots, Tensor scratch_slots,
                   StructuredWorldGraph structured_world_graph = {},
                   std::vector<std::string> evidence_refs = {},
                   JsonValue::Object goal_state = {}, JsonValue::Object value_state = {},
                   JsonValue::Object self_state = {},
                   std::string owner_id = std::string(default_owner));

    [[nodiscard]] const Tensor& semantic_slots() const noexcept;
    [[nodiscard]] const Tensor& executive_slots() const noexcept;
    [[nodiscard]] const Tensor& scratch_slots() const noexcept;
    [[nodiscard]] const StructuredWorldGraph& structured_world_graph() const noexcept;
    [[nodiscard]] std::span<const std::string> evidence_refs() const noexcept;
    [[nodiscard]] const JsonValue::Object& goal_state() const noexcept;
    [[nodiscard]] const JsonValue::Object& value_state() const noexcept;
    [[nodiscard]] const JsonValue::Object& self_state() const noexcept;
    [[nodiscard]] std::string_view owner_id() const noexcept;
    [[nodiscard]] constexpr std::size_t persistent_state_count() const noexcept { return 1; }

    void validate(const CognitiveKernelConfig& config) const;
    [[nodiscard]] CognitiveState with_metadata(JsonValue::Object goal_state,
                                               JsonValue::Object self_state) const;
    [[nodiscard]] CognitiveState clone() const;
    [[nodiscard]] bool exact_equal(const CognitiveState& other) const noexcept;
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static CognitiveState from_dict(const JsonValue& payload);

private:
    Tensor semantic_slots_;
    Tensor executive_slots_;
    Tensor scratch_slots_;
    StructuredWorldGraph structured_world_graph_;
    std::vector<std::string> evidence_refs_;
    JsonValue::Object goal_state_;
    JsonValue::Object value_state_;
    JsonValue::Object self_state_;
    std::string owner_id_;
};

}  // namespace swegca::world
