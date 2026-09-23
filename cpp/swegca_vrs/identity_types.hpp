#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace swegca::vrs {

using architecture::Digest256;

namespace detail {

inline constexpr std::size_t identity_text_max_bytes = 4096;

// Native byte representation of Python text that may contain lone surrogate
// code points. `push_generalized_utf8` reports one completed code point while
// retaining a partial sequence across input chunks.
// Lineage: native mechanism — event/control text transport.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
struct GeneralizedUtf8State {
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    std::uint8_t pending = 0;
};

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
[[nodiscard]] bool push_generalized_utf8(GeneralizedUtf8State& state, std::byte value,
                                         bool& complete) noexcept;

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
[[nodiscard]] bool is_generalized_utf8(std::string_view value) noexcept;

[[nodiscard]] bool is_strict_utf8(std::string_view value) noexcept;

void require_identity_text(std::string_view value, std::string_view field);

// The same rule as `require_identity_text`, judged without allocating or
// throwing (for texts viewed in place, such as decoded journal records).
[[nodiscard]] bool is_identity_text(std::string_view value) noexcept;

// Whether Python's str.strip removes `code_point`: the blank set the identity
// rule uses (Unicode 16.0, as measured on Python 3.14.7).
[[nodiscard]] bool is_python_strip_space(std::uint32_t code_point) noexcept;

}  // namespace detail

// A distinct Tag creates a non-convertible identity type. The source files are
// rule evidence only; this is a native C++ ownership boundary.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:35-71; TERMINOLOGY.md@5901a5a:16-41.
template <class Tag>
class TextIdentity final {
public:
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:19-21
    TextIdentity(const AllocationContext& account, std::string_view value)
        : value_(account.allocator<char>()) {
        detail::require_identity_text(value, Tag::name);
        value_.assign(value.data(), value.size());
    }

    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:40-49
    [[nodiscard]] std::string_view value() const noexcept { return value_; }

    auto operator<=>(const TextIdentity&) const = default;

private:
    std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>> value_;
};

struct OwnerIdTag { static constexpr std::string_view name = "owner_id"; };
struct ProducerIdTag { static constexpr std::string_view name = "producer_id"; };
struct ClaimIdTag { static constexpr std::string_view name = "claim_id"; };
struct ExperienceAddressTag {
    static constexpr std::string_view name = "experience_address";
};
struct RoleIdTag { static constexpr std::string_view name = "role_id"; };
struct EntityIdTag { static constexpr std::string_view name = "entity_id"; };
struct EntityKindTag { static constexpr std::string_view name = "entity_kind"; };
struct RelationPredicateTag { static constexpr std::string_view name = "relation_predicate"; };
struct TransactionIdTag {
    static constexpr std::string_view name = "transaction_id";
};
struct PolicyVersionTag {
    static constexpr std::string_view name = "policy_version";
};

using OwnerId = TextIdentity<OwnerIdTag>;
using ProducerId = TextIdentity<ProducerIdTag>;
using ClaimId = TextIdentity<ClaimIdTag>;
using ExperienceAddress = TextIdentity<ExperienceAddressTag>;
using RoleId = TextIdentity<RoleIdTag>;
using EntityId = TextIdentity<EntityIdTag>;
using EntityKind = TextIdentity<EntityKindTag>;
using RelationPredicate = TextIdentity<RelationPredicateTag>;
using TransactionId = TextIdentity<TransactionIdTag>;
using PolicyVersion = TextIdentity<PolicyVersionTag>;

class ClaimRevision final {
public:
    // Rule: claim-relative evidence, ARCHITECTURE_SPEC.md@5901a5a:117-135.
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:97-135
    ClaimRevision(ClaimId claim, std::uint64_t revision);

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:97-135
    [[nodiscard]] const ClaimId& claim() const noexcept { return claim_; }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:97-135
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    auto operator<=>(const ClaimRevision&) const = default;

private:
    ClaimId claim_;
    std::uint64_t revision_;
};

struct NoAuthority final {};

}  // namespace swegca::vrs
