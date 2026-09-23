#include "event_vrs_inputs.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:54-55
bool sha256_id(std::string_view identifier) {
    return identifier.size() == 64 &&
        std::all_of(identifier.begin(), identifier.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:53-73
ValidatedEventVrsInputs::ValidatedEventVrsInputs(
    std::shared_ptr<const EventVrsInputView> source)
    : source_(std::move(source)), snapshot_id_(source_ ? source_->snapshot_id() : std::string()),
      node_count_(source_ ? source_->node_count() : 0),
      edge_count_(source_ ? source_->edge_count() : 0) {
    if (!source_ || !sha256_id(snapshot_id_))
        throw std::runtime_error("event inputs need a generation digest");
    if (node_count_ > std::uint64_t{0x100000000ULL} ||
        edge_count_ > std::uint64_t{std::numeric_limits<std::uint32_t>::max()})
        throw std::runtime_error("event input shape or dtype changed");
    source_->require_immutable_binding();
    for (std::uint64_t node = 0; node < node_count_; ++node) {
        const auto address = static_cast<std::uint32_t>(node);
        (void)source_->unresolved(address);
        if (!std::isfinite(source_->direct(address)) || !std::isfinite(source_->score(address)))
            throw std::runtime_error("event input contains nonfinite values");
    }
    for (std::uint64_t address = 0; address < edge_count_; ++address) {
        const auto index = static_cast<std::uint32_t>(address);
        const auto edge = source_->edge(index);
        const auto strength = source_->strength(index);
        if (!std::isfinite(edge.vrs_strength) || !std::isfinite(strength) ||
            edge.vrs_strength < 0 || strength < 0 ||
            (edge.sign != -1 && edge.sign != 0 && edge.sign != 1))
            throw std::runtime_error("event edges or strength changed");
        if (edge.source >= node_count_ || edge.target >= node_count_)
            throw std::runtime_error("event endpoint is outside the node directory");
    }
    source_->dependencies().require_source(*source_);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:170-180
ValidatedEventVrsInputs::ValidatedEventVrsInputs(
    std::shared_ptr<const EventVrsInputView> source,
    const ValidatedEventVrsInputs& parent,
    std::uint64_t appended_nodes, std::uint64_t appended_edges)
    : source_(std::move(source)), snapshot_id_(source_ ? source_->snapshot_id() : std::string()),
      node_count_(source_ ? source_->node_count() : 0),
      edge_count_(source_ ? source_->edge_count() : 0) {
    (void)parent.require_validated_immutable();
    if (!source_ || !sha256_id(snapshot_id_) ||
        node_count_ > std::uint64_t{0x100000000ULL} ||
        edge_count_ > std::uint64_t{std::numeric_limits<std::uint32_t>::max()} ||
        node_count_ != parent.node_count_ + appended_nodes ||
        edge_count_ != parent.edge_count_ + appended_edges)
        throw std::runtime_error("event delta shape or generation changed");
    source_->require_immutable_binding();
    source_->dependencies().require_source(*source_);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:80-86
const EventVrsInputView& ValidatedEventVrsInputs::require_validated_immutable() const {
    source_->require_immutable_binding();
    if (source_->snapshot_id() != snapshot_id_ ||
        source_->node_count() != node_count_ || source_->edge_count() != edge_count_)
        throw std::runtime_error("event validated array metadata changed");
    source_->dependencies().require_source(*source_);
    return *source_;
}

}  // namespace swegca::vrs
