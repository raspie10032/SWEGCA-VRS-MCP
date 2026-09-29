#pragma once

#include "world/cognitive_state.hpp"
#include "world/session_content_encoding.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view utterance_receipt_source_sha256 =
    "4209124ddf9ee1d6898b2b856dcf391426e7608eaba0c54a1d50c912e1888b2e";
inline constexpr std::string_view utterance_plan_schema =
    "rozephine-utterance-plan-v1";
inline constexpr std::string_view utterance_receipt_schema =
    "rozephine-utterance-receipt-v1";

extern const std::string model_receipt_guide;

[[nodiscard]] std::size_t validate_delivered_part(
    const JsonValue::Object& receipt,
    std::string_view request_id,
    std::string_view view_id,
    std::string_view pair_snapshot_id,
    std::size_t next_index,
    std::optional<std::size_t> part_count = std::nullopt);

using UtteranceClaimGroup =
    std::optional<std::vector<SessionRecordedClaimAddress>>;

class UtterancePlan final {
public:
    UtterancePlan(std::string request_id, std::string view_id,
                  std::string pair_snapshot_id,
                  std::vector<UtteranceClaimGroup> groups);

    [[nodiscard]] std::string plan_id() const;
    [[nodiscard]] std::string part_id(std::size_t index) const;
    [[nodiscard]] JsonValue::Object prepared(std::size_t index) const;
    [[nodiscard]] JsonValue::Object receipt() const;
    [[nodiscard]] JsonValue::Object delivered(
        std::size_t index, std::size_t delivered_parts,
        std::string delta, bool failed,
        const JsonValue::Object* request_receipt,
        std::int64_t started_ns, std::int64_t finished_ns) const;

    const std::string request_id;
    const std::string view_id;
    const std::string pair_snapshot_id;
    const std::vector<UtteranceClaimGroup> groups;
};

[[nodiscard]] JsonValue::Object cancelled_delivery(
    const JsonValue::Object& receipt);

}  // namespace swegca::world
