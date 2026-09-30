#pragma once

#include "world/vrs_event_hot_publication.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view vrs_event_durable_source_sha256 =
    "602bc109a06d07aa7db8daa7d8101e3ad19f7bad1725a64f9cbd073343d477bf";

class DurableEventBinding;

struct PreparedDurableEvent final {
    PreparedEventHotGeneration hot;
    std::shared_ptr<const DurableEventBinding> next_binding;
    std::string report_sha256;
    std::filesystem::path report_path;
    std::size_t report_bytes{};
    std::optional<VrsArrayBundleSaveReceipt> storage_receipt;
    bool cold_restored{};
    bool durable_report_verified{true};
    bool main_pair_committed{};
    bool current_pointer_written{};
    bool authority_granted{};
};

class DurableEventBinding final {
public:
    [[nodiscard]] static std::shared_ptr<const DurableEventBinding> bootstrap(
        std::shared_ptr<const BoundEventHotSource> hot,
        std::shared_ptr<VrsGenerationBlockStore> store);
    [[nodiscard]] static std::shared_ptr<const DurableEventBinding> cold_bind(
        std::shared_ptr<const BoundEventHotSource> hot,
        std::shared_ptr<VrsGenerationBlockStore> store,
        std::string bundle_sha256,
        std::size_t maximum_raw_bytes);
    [[nodiscard]] PreparedDurableEvent prepare(
        const PreparedEventSignalStorage& candidate,
        std::size_t maximum_report_bytes = 16U * 1024U * 1024U) const;
    [[nodiscard]] PreparedDurableEvent restore(
        std::string report_sha256,
        std::size_t maximum_raw_bytes,
        std::size_t maximum_report_bytes = 16U * 1024U * 1024U,
        std::size_t maximum_manifest_bytes = 16U * 1024U * 1024U) const;

    const std::shared_ptr<const BoundEventHotSource> hot;
    const std::shared_ptr<VrsGenerationBlockStore> store;
    const std::string bundle_sha256;

private:
    DurableEventBinding(std::shared_ptr<const BoundEventHotSource> hot,
        std::shared_ptr<VrsGenerationBlockStore> store,
        std::string bundle_sha256);
};

}  // namespace swegca::world
