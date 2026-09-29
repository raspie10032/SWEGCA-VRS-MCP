#pragma once

#include "world/vrs_array_blocks.hpp"
#include "world/vrs_event_signal.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_event_storage_source_sha256 =
    "e1d10843da65ef8938d1ae138c5180a0680e24531a56441b3c37aaebaabc68e4";

class BoundEventSignalStorage;

class StorageBoundEventSignalProposal final {
public:
    StorageBoundEventSignalProposal(const StorageBoundEventSignalProposal&) = default;
    StorageBoundEventSignalProposal(StorageBoundEventSignalProposal&&) noexcept = default;
    StorageBoundEventSignalProposal& operator=(const StorageBoundEventSignalProposal&) = delete;
    StorageBoundEventSignalProposal& operator=(StorageBoundEventSignalProposal&&) = delete;

    const EventSignalProposal signal;
    const std::shared_ptr<const BoundEventSignalStorage> binding;

private:
    friend class BoundEventSignalStorage;
    StorageBoundEventSignalProposal(
        EventSignalProposal signal,
        std::shared_ptr<const BoundEventSignalStorage> binding);
};

struct PreparedEventStorageDiagnostics final {
    VrsArrayPatchReceipt scores;
    VrsArrayPatchReceipt strengths;
};

class PreparedEventSignalStorage final {
public:
    PreparedEventSignalStorage(const PreparedEventSignalStorage&) = default;
    PreparedEventSignalStorage(PreparedEventSignalStorage&&) noexcept = default;
    PreparedEventSignalStorage& operator=(const PreparedEventSignalStorage&) = delete;
    PreparedEventSignalStorage& operator=(PreparedEventSignalStorage&&) = delete;

    const std::shared_ptr<const BoundEventSignalStorage> parent;
    const std::shared_ptr<const StorageBoundEventSignalProposal> proposal;
    const std::shared_ptr<const VrsArrayBlocks> scores;
    const std::shared_ptr<const VrsArrayBlocks> strengths;
    const std::vector<VrsExperiencePromotionDecision> promotions;
    const PreparedEventStorageDiagnostics diagnostics;

    [[nodiscard]] bool authority_granted() const noexcept { return false; }
    [[nodiscard]] bool persistent_state_mutated() const noexcept { return false; }
    [[nodiscard]] bool main_committed() const noexcept { return false; }

private:
    friend class BoundEventSignalStorage;
    PreparedEventSignalStorage(
        std::shared_ptr<const BoundEventSignalStorage> parent,
        std::shared_ptr<const StorageBoundEventSignalProposal> proposal,
        std::shared_ptr<const VrsArrayBlocks> scores,
        std::shared_ptr<const VrsArrayBlocks> strengths,
        std::vector<VrsExperiencePromotionDecision> promotions,
        PreparedEventStorageDiagnostics diagnostics);
};

// This pinned storage boundary binds the already validated base generation.
// Sparse appended EventVrsInputs are a separate prerequisite and are not
// represented as a dense fallback here.
class BoundEventSignalStorage final :
    public std::enable_shared_from_this<BoundEventSignalStorage> {
public:
    [[nodiscard]] static std::shared_ptr<const BoundEventSignalStorage> cold_bind(
        std::shared_ptr<const EventSignalInputs> inputs,
        std::shared_ptr<const VrsArrayBlocks> scores,
        std::shared_ptr<const VrsArrayBlocks> strengths);

    [[nodiscard]] StorageBoundEventSignalProposal settle(
        std::span<const std::size_t> changed_nodes = {},
        std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_updates = {},
        std::string_view connection_namespace = "vrs-edge:",
        const StorageBoundEventSignalProposal* previous = nullptr,
        std::uint64_t maximum_rounds = 1) const;

    [[nodiscard]] PreparedEventSignalStorage prepare(
        const StorageBoundEventSignalProposal& proposal,
        VrsBlockCodec codec = VrsBlockCodec::zlib) const;

    const std::shared_ptr<const EventSignalInputs> inputs;
    const std::shared_ptr<const VrsArrayBlocks> scores;
    const std::shared_ptr<const VrsArrayBlocks> strengths;

private:
    BoundEventSignalStorage(
        std::shared_ptr<const EventSignalInputs> inputs,
        std::shared_ptr<const VrsArrayBlocks> scores,
        std::shared_ptr<const VrsArrayBlocks> strengths);
};

}  // namespace swegca::world
