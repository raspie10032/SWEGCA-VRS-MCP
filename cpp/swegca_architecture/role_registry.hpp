#pragma once

#include "swegca_architecture/strong_types.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::architecture {

enum class TensorPartition : std::uint8_t {
    semantic = 1,
    executive = 2,
    scratch = 3,
};

struct RoleDefinition final {
    RoleId id;
    TensorPartition partition;
    std::uint64_t slot;

    auto operator<=>(const RoleDefinition&) const = default;
};

struct RolePartitionSizes final {
    std::uint64_t semantic;
    std::uint64_t executive;
    std::uint64_t scratch;

    auto operator<=>(const RolePartitionSizes&) const = default;
};

// Immutable string-addressed mapping from cognitive role to one tensor slot.
// Extensions append roles inside the same Cognitive State identity.
// Rule: state subsystem, reconstruction board@7c0b62f:83-93.
class RoleRegistry final {
public:
    explicit RoleRegistry(std::vector<RoleDefinition> definitions);

    [[nodiscard]] static RoleRegistry initial_profile(RolePartitionSizes sizes);
    [[nodiscard]] RoleRegistry with_appended(RoleDefinition definition) const;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] std::size_t size() const noexcept { return definitions_.size(); }
    [[nodiscard]] const RoleDefinition& at(std::size_t index) const;
    [[nodiscard]] const RoleDefinition* find(std::string_view id) const noexcept;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const std::vector<RoleDefinition>& definitions() const noexcept {
        return definitions_;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

private:
    std::vector<RoleDefinition> definitions_;
    std::map<std::string, std::size_t, std::less<>> by_id_;
    Digest256 digest_;
};

// A role mask cannot be confused with numeric tensor storage.
class RoleMask final {
public:
    [[nodiscard]] static RoleMask none(const RoleRegistry& registry);
    [[nodiscard]] static RoleMask from_indices(
        const RoleRegistry& registry, std::span<const std::size_t> selected);

    [[nodiscard]] bool test(std::size_t index) const;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
    [[nodiscard]] std::size_t role_count() const noexcept { return role_count_; }
    [[nodiscard]] std::size_t selected_count() const noexcept;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] bool matches(const RoleRegistry& registry) const noexcept {
        return registry_digest_ == registry.digest();
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const Digest256& registry_digest() const noexcept {
        return registry_digest_;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
    [[nodiscard]] const std::vector<std::uint64_t>& words() const noexcept {
        return words_;
    }

    auto operator<=>(const RoleMask&) const = default;

private:
    RoleMask(const RoleRegistry& registry, std::vector<std::uint64_t> words);

    std::size_t role_count_;
    Digest256 registry_digest_;
    std::vector<std::uint64_t> words_;
};

}  // namespace swegca::architecture
