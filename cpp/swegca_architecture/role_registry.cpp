#include "swegca_architecture/role_registry.hpp"

#include "swegca_architecture/sha256.hpp"

#include <array>
#include <bit>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

constexpr std::array<std::string_view, 32> initial_role_names{
    "object_0", "object_1", "object_2", "object_3",
    "object_4", "object_5", "object_6", "object_7",
    "relation_0", "relation_1", "relation_2", "relation_3",
    "relation_4", "relation_5", "relation_6", "relation_7",
    "action_0", "action_1", "action_2", "action_3",
    "camera_0", "camera_1", "lighting_0", "lighting_1",
    "environment_0", "environment_1", "audio_event_0", "audio_event_1",
    "narrative", "constraints", "verification", "global",
};

constexpr std::size_t verification_role_index = 30;

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
void require_initial_sizes(RolePartitionSizes sizes) {
    if (sizes.semantic == 0 || sizes.executive == 0 || sizes.scratch == 0 ||
        sizes.semantic > initial_role_names.size() ||
        sizes.executive > initial_role_names.size() ||
        sizes.scratch > initial_role_names.size() ||
        sizes.semantic + sizes.executive + sizes.scratch != initial_role_names.size())
        throw std::invalid_argument("initial_role_partition_size_invalid");
    if (verification_role_index < sizes.semantic + sizes.executive)
        throw std::invalid_argument("verification_role_must_be_in_scratch");
}

// Registry identity is stable across processes and binds every mask to the
// exact ordered role-to-partition map, rather than only its cardinality.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
void update_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
Digest256 registry_digest(std::span<const RoleDefinition> definitions) {
    Sha256 hash;
    hash.update("swegca.role_registry.v1");
    update_u64(hash, definitions.size());
    for (const auto& definition : definitions) {
        update_u64(hash, definition.id.value().size());
        hash.update(definition.id.value());
        const std::array partition{
            static_cast<std::byte>(definition.partition)};
        hash.update(partition);
        update_u64(hash, definition.slot);
    }
    return Digest256(hash.finish());
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
std::size_t required_word_count(std::size_t roles) {
    if (roles == 0)
        throw std::invalid_argument("role_mask_registry_must_not_be_empty");
    if (roles > std::numeric_limits<std::size_t>::max() - 63)
        throw std::overflow_error("role_mask_size_overflow");
    return (roles + 63) / 64;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
std::string_view role_text(const RoleDefinition& definition) noexcept {
    return definition.id.value();
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
std::string_view role_text(const RoleDefinitionInput& definition) noexcept {
    return definition.id;
}

// Both startup and typed successor inputs become identities on Main's account.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
template <class Input>
std::vector<RoleDefinition, AllocationAdapter<RoleDefinition>> owned_definitions(
    const AllocationContext& account, std::span<const Input> definitions) {
    if (definitions.empty())
        throw std::invalid_argument("role_registry_must_not_be_empty");
    std::vector<RoleDefinition, AllocationAdapter<RoleDefinition>> owned(
        account.allocator<RoleDefinition>());
    owned.reserve(definitions.size());
    for (const auto& definition : definitions)
        owned.push_back({RoleId(account, role_text(definition)),
                         definition.partition, definition.slot});
    return owned;
}

}  // namespace

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
RoleRegistry::RoleRegistry(const AllocationContext& account,
                            std::span<const RoleDefinition> definitions)
    : RoleRegistry(account, owned_definitions(account, definitions)) {}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
RoleRegistry::RoleRegistry(const AllocationContext& account,
                           std::span<const RoleDefinitionInput> definitions)
    : RoleRegistry(account, owned_definitions(account, definitions)) {}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
RoleRegistry::RoleRegistry(const AllocationContext& account, Definitions definitions)
    : memory_(account), definitions_(std::move(definitions)),
      by_id_(std::less<>{}, account.allocator<IdEntry>()),
      digest_(registry_digest(definitions_)) {
    if (definitions_.empty())
        throw std::invalid_argument("role_registry_must_not_be_empty");
    using Location = std::pair<TensorPartition, std::uint64_t>;
    std::set<Location, std::less<Location>, AllocationAdapter<Location>> locations(
        std::less<Location>{}, account.allocator<Location>());
    for (std::size_t index = 0; index < definitions_.size(); ++index) {
        const auto& definition = definitions_[index];
        if (definition.partition != TensorPartition::semantic &&
            definition.partition != TensorPartition::executive &&
            definition.partition != TensorPartition::scratch)
            throw std::invalid_argument("role_registry_partition_invalid");
        // The map node and any key character buffer share Main's account.
        // Small-string storage is already covered by the map node allocation.
        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        if (!by_id_.emplace(
                IdText(definition.id.value().data(), definition.id.value().size(),
                       account.allocator<char>()), index).second)
            throw std::invalid_argument("role_registry_duplicate_id");
        if (!locations.emplace(definition.partition, definition.slot).second)
            throw std::invalid_argument("role_registry_duplicate_location");
    }
}

// The accepted profile preserves SLOT_ROLES global order across the three
// concatenated partitions. Further roles are explicit registry extensions;
// they do not create another state object.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
RoleRegistry RoleRegistry::initial_profile(const AllocationContext& account,
                                            RolePartitionSizes sizes) {
    require_initial_sizes(sizes);
    const auto scratch_begin = sizes.semantic + sizes.executive;

    Definitions definitions(account.allocator<RoleDefinition>());
    definitions.reserve(initial_role_names.size());
    for (std::size_t index = 0; index < initial_role_names.size(); ++index) {
        TensorPartition partition = TensorPartition::semantic;
        std::uint64_t local_slot = index;
        if (index >= scratch_begin) {
            partition = TensorPartition::scratch;
            local_slot = index - scratch_begin;
        } else if (index >= sizes.semantic) {
            partition = TensorPartition::executive;
            local_slot = index - sizes.semantic;
        }
        definitions.push_back(RoleDefinition{
            RoleId(account, initial_role_names[index]),
            partition,
            local_slot,
        });
    }
    return RoleRegistry(account, std::move(definitions));
}

// Check the same fixed profile without constructing a second registry.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
bool RoleRegistry::matches_initial_profile(RolePartitionSizes sizes) const {
    require_initial_sizes(sizes);
    if (definitions_.size() != initial_role_names.size()) return false;
    const auto scratch_begin = sizes.semantic + sizes.executive;
    for (std::size_t index = 0; index < definitions_.size(); ++index) {
        const auto partition = index >= scratch_begin ? TensorPartition::scratch
            : index >= sizes.semantic ? TensorPartition::executive : TensorPartition::semantic;
        const auto slot = index >= scratch_begin ? index - scratch_begin
            : index >= sizes.semantic ? index - sizes.semantic : index;
        const auto& role = definitions_[index];
        if (role.id.value() != initial_role_names[index] || role.partition != partition ||
            role.slot != slot) return false;
    }
    return true;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
RoleRegistry RoleRegistry::with_appended(RoleDefinition definition) const {
    auto definitions = definitions_;
    definitions.push_back({RoleId(memory_, definition.id.value()),
                           definition.partition, definition.slot});
    return RoleRegistry(memory_, std::move(definitions));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
const RoleDefinition& RoleRegistry::at(std::size_t index) const {
    if (index >= definitions_.size())
        throw std::out_of_range("role_registry_index_out_of_range");
    return definitions_[index];
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
const RoleDefinition* RoleRegistry::find(std::string_view id) const noexcept {
    const auto found = by_id_.find(id);
    return found == by_id_.end() ? nullptr : &definitions_[found->second];
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
RoleMask::RoleMask(const RoleRegistry& registry,
                   Words words)
    : role_count_(registry.size()), registry_digest_(registry.digest()),
      words_(std::move(words)) {
    if (words_.size() != required_word_count(role_count_))
        throw std::invalid_argument("role_mask_word_count_mismatch");
    if (role_count_ != 0 && role_count_ % 64 != 0) {
        const auto valid_bits = role_count_ % 64;
        const auto outside = ~((std::uint64_t{1} << valid_bits) - 1);
        if ((words_.back() & outside) != 0)
            throw std::invalid_argument("role_mask_outside_registry");
    }
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
RoleMask RoleMask::none(const RoleRegistry& registry) {
    return RoleMask(
        registry,
        Words(required_word_count(registry.size()), std::uint64_t{0},
              registry.memory_.allocator<std::uint64_t>()));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
RoleMask RoleMask::from_indices(
    const RoleRegistry& registry, std::span<const std::size_t> selected) {
    auto mask = none(registry);
    for (const auto index : selected) {
        if (index >= registry.size())
            throw std::out_of_range("role_mask_index_out_of_range");
        mask.words_[index / 64] |= std::uint64_t{1} << (index % 64);
    }
    return mask;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
bool RoleMask::test(std::size_t index) const {
    if (index >= role_count_)
        throw std::out_of_range("role_mask_index_out_of_range");
    if (index / 64 >= words_.size())
        throw std::logic_error("role_mask_not_live");
    return (words_[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::size_t RoleMask::selected_count() const noexcept {
    std::size_t count = 0;
    for (const auto word : words_) count += std::popcount(word);
    return count;
}

}  // namespace swegca::architecture
