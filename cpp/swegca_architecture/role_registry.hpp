#pragma once

#include "swegca_architecture/allocation.hpp"
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

// Borrowed startup fields; Main constructs the owned RoleId on its account.
struct RoleDefinitionInput final {
    std::string_view id;
    TensorPartition partition;
    std::uint64_t slot;
};

struct RolePartitionSizes final {
    std::uint64_t semantic;
    std::uint64_t executive;
    std::uint64_t scratch;

    auto operator<=>(const RolePartitionSizes&) const = default;
};

// Immutable string-addressed mapping from cognitive role to one tensor slot.
// The original CognitiveSlotTopology defines a fixed 32-role compatibility
// profile. Its audit explicitly reports compatible extensions as missing
// (mosaic_cognitive_slot_topology.py:49-73,107-118). Appended roles and the
// digest-bound map are additional C++ successor infrastructure.
class RoleRegistry final {
public:
    RoleRegistry(const AllocationContext& account,
                 std::span<const RoleDefinition> definitions);
    RoleRegistry(const AllocationContext& account,
                 std::span<const RoleDefinitionInput> definitions);

    [[nodiscard]] static RoleRegistry initial_profile(
        const AllocationContext& account, RolePartitionSizes sizes);
    [[nodiscard]] bool matches_initial_profile(RolePartitionSizes sizes) const;
    [[nodiscard]] RoleRegistry with_appended(RoleDefinition definition) const;

    // SWEGCA: src/swegca/mosaic_cognitive_slot_topology.py@5901a5a:12-73
    [[nodiscard]] std::size_t size() const noexcept { return definitions_.size(); }
    [[nodiscard]] const RoleDefinition& at(std::size_t index) const;
    [[nodiscard]] const RoleDefinition* find(std::string_view id) const noexcept;
    // SWEGCA: src/swegca/mosaic_cognitive_slot_topology.py@5901a5a:12-73
    [[nodiscard]] std::span<const RoleDefinition> definitions() const noexcept {
        return definitions_;
    }
    // SWEGCA: src/swegca/mosaic_cognitive_slot_topology.py@5901a5a:12-73
    [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

private:
    friend class RoleMask;
    using Definitions = std::vector<RoleDefinition, AllocationAdapter<RoleDefinition>>;
    using IdText = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;
    using IdEntry = std::pair<const IdText, std::size_t>;
    RoleRegistry(const AllocationContext& account, Definitions definitions);

    AllocationContext memory_;
    Definitions definitions_;
    std::map<IdText, std::size_t, std::less<>, AllocationAdapter<IdEntry>> by_id_;
    Digest256 digest_;
};

// A role mask cannot be confused with numeric tensor storage. It represents
// the author's boolean target_slot_mask with C++ packed words; the original
// does not have a RoleMask class or registry digest.
class RoleMask final {
public:
    [[nodiscard]] static RoleMask none(const RoleRegistry& registry);
    [[nodiscard]] static RoleMask from_indices(
        const RoleRegistry& registry, std::span<const std::size_t> selected);

    [[nodiscard]] bool test(std::size_t index) const;
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-75
    [[nodiscard]] std::size_t role_count() const noexcept { return role_count_; }
    [[nodiscard]] std::size_t selected_count() const noexcept;
    // SWEGCA: src/swegca/mosaic_cognitive_slot_topology.py@5901a5a:12-73
    [[nodiscard]] bool matches(const RoleRegistry& registry) const noexcept {
        // A moved-from word buffer must not keep a usable registry binding.
        return role_count_ == registry.size() &&
               words_.size() == role_count_ / 64 + (role_count_ % 64 != 0) &&
               registry_digest_ == registry.digest();
    }
    // SWEGCA: src/swegca/mosaic_cognitive_slot_topology.py@5901a5a:12-73
    [[nodiscard]] const Digest256& registry_digest() const noexcept {
        return registry_digest_;
    }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-75
    [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
        return words_;
    }

    auto operator<=>(const RoleMask&) const = default;

private:
    using Words = std::vector<std::uint64_t, AllocationAdapter<std::uint64_t>>;
    RoleMask(const RoleRegistry& registry, Words words);

    std::size_t role_count_;
    Digest256 registry_digest_;
    Words words_;
};

}  // namespace swegca::architecture
