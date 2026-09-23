#pragma once

#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/allocation.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace swegca::architecture {

namespace detail {

inline constexpr std::size_t identity_text_max_bytes = 4096;

[[nodiscard]] bool is_strict_utf8(std::string_view value) noexcept;

void require_identity_text(std::string_view value, std::string_view field);

// The same rule as `require_identity_text`, judged without allocating or
// throwing (for texts viewed in place, such as decoded journal records).
[[nodiscard]] bool is_identity_text(std::string_view value) noexcept;

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

class Digest256 final {
public:
    static constexpr std::size_t width = digest256_width;
    using Bytes = DigestBytes;

    // Rule: provenance and immutable artifact identity, SWEGCA I03 and I07.
    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-45
    explicit Digest256(Bytes bytes);

    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
    [[nodiscard]] static Digest256 from_hex(std::string_view value);

    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:15-25
    [[nodiscard]] std::string hex() const;

    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
    [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
    auto operator<=>(const Digest256&) const = default;

private:
    Bytes bytes_;
};

class StateGeneration final {
public:
    // The ordinal names a successor; the digest names canonical state
    // content. Bit-exact rollback may restore an earlier digest at a new
    // ordinal (mosaic_bounded_world_write.py@5901a5a:262-283,424-442).
    // Rule: one current Main-owned state, ARCHITECTURE_SPEC.md@5901a5a:103-109.
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:175-188
    StateGeneration(std::uint64_t ordinal, Digest256 digest);

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:175-188
    [[nodiscard]] std::uint64_t ordinal() const noexcept { return ordinal_; }
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:175-188
    [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }
    auto operator<=>(const StateGeneration&) const = default;

private:
    std::uint64_t ordinal_;
    Digest256 digest_;
};

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

}  // namespace swegca::architecture
