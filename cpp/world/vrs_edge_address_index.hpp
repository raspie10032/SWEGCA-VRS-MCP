#pragma once

#include "world/vrs_event_signal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_edge_address_index_source_sha256 =
    "7edceca917194419828845d7350f7b19a2747b19fbddd1a7ccfbf07a2be6cf56";

// Immutable endpoint/sign directory. Strength is deliberately excluded from
// the address so re-evidence can change a numeric value without changing the
// canonical connection identity.
class CanonicalEdgeAddressIndex final {
public:
    struct Segment;

    [[nodiscard]] static std::shared_ptr<const CanonicalEdgeAddressIndex> build(
        const PersistentEventVector<EventSignalEdge>& source);

    [[nodiscard]] std::shared_ptr<const CanonicalEdgeAddressIndex> advance(
        const PersistentEventVector<EventSignalEdge>& successor) const;

    [[nodiscard]] std::shared_ptr<const CanonicalEdgeAddressIndex> advance_event_delta(
        const EventSignalInputs& parent,
        const EventSignalInputs& successor) const;

    void require_source(const PersistentEventVector<EventSignalEdge>& source) const;

    [[nodiscard]] std::optional<std::size_t> lookup(
        std::uint64_t source, std::uint64_t target, int sign) const;
    [[nodiscard]] std::size_t segment_count() const noexcept;
    [[nodiscard]] std::size_t index_bytes() const noexcept;

private:
    CanonicalEdgeAddressIndex(
        PersistentEventVector<EventSignalEdge> source,
        std::vector<std::shared_ptr<const Segment>> segments);

    const PersistentEventVector<EventSignalEdge> source_;
    const std::vector<std::shared_ptr<const Segment>> segments_;
};

}  // namespace swegca::world
