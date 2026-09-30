#pragma once

#include "world/compressed_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view resident_observation_tree_source_sha256 =
    "f0af7c4bd7c17d3c98fb9a2f088c4ca1eaa4eb84556fe698d80a0e61fc724316";

using ResidentObservationExternal = std::variant<
    std::shared_ptr<const CompressedText>,
    std::shared_ptr<const PackedFloatTuple>>;

class PackedResidentObservation final {
public:
    PackedResidentObservation(
        std::shared_ptr<const std::vector<std::byte>> blob,
        std::vector<ResidentObservationExternal> externals,
        std::size_t start = 0, std::size_t stop = 0,
        bool indexed = false);

    [[nodiscard]] PackedResidentObservation request_view() const;
    [[nodiscard]] std::vector<std::string> keys() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] JsonValue value(std::string_view key) const;
    [[nodiscard]] JsonValue::Object materialize() const;
    [[nodiscard]] PackedResidentObservation object_view(std::string_view key) const;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] const std::vector<ResidentObservationExternal>& externals() const noexcept;

private:
    std::shared_ptr<const std::vector<std::byte>> blob_;
    std::shared_ptr<const std::vector<ResidentObservationExternal>> externals_;
    std::size_t start_{};
    std::size_t stop_{};
    std::shared_ptr<const std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>>>
        fields_;

    PackedResidentObservation(
        std::shared_ptr<const std::vector<std::byte>> blob,
        std::shared_ptr<const std::vector<ResidentObservationExternal>> externals,
        std::size_t start, std::size_t stop, bool indexed);
};

[[nodiscard]] PackedResidentObservation pack_observation(
    const JsonValue::Object& observation,
    std::optional<CompressionPolicy> policy = std::nullopt);

}  // namespace swegca::world
