#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view packed_memberships_source_sha256 =
    "889ed37bfd27d508968b64f5e4576b14546000ba38e35798db944a8e245b17c4";

using MembershipIdentifier = std::variant<std::uint64_t, JsonInteger>;

struct MembershipValue final {
    MembershipIdentifier group;
    double weight{};
    friend bool operator==(const MembershipValue&, const MembershipValue&) = default;
};

class PackedMemberships final {
public:
    PackedMemberships(std::vector<std::uint64_t> groups,
                      std::vector<double> weights);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] MembershipValue operator[](std::size_t index) const;
    [[nodiscard]] std::vector<MembershipValue> values() const;

private:
    std::vector<std::uint64_t> groups_;
    std::vector<double> weights_;
};

using PackedMembershipResult =
    std::variant<PackedMemberships, std::vector<MembershipValue>>;

[[nodiscard]] PackedMembershipResult pack_validated_memberships(
    const std::vector<MembershipValue>& values);

}  // namespace swegca::world
