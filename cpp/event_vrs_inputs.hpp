#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace swegca::vrs {

class EventDeltaView;

// Logical edge value; the native on-disk layout is encoded explicitly and
// never inferred from sizeof(EventEdge).
struct EventEdge {
    std::uint32_t source;
    std::uint32_t target;
    std::int8_t sign;
    float vrs_strength;
};

enum class EndpointDirection { outgoing, incoming };

class EventVrsInputView;

// Source-bound endpoint addresses. A physical implementation can keep sorted
// segments on disk while preserving the complete directed dependency set.
class EndpointDependencyIndex {
public:
    virtual ~EndpointDependencyIndex() = default;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:51-53
    virtual void require_source(const EventVrsInputView& source) const = 0;
    // Iteration order is the author's segment-concatenation order. The event
    // kernel's fsum observes that order and must not receive sorted-by-ID
    // replacements from a physical index.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
    virtual void visit_edges(std::uint32_t node, EndpointDirection direction,
                             const std::function<void(std::uint32_t)>& visit) const = 0;
};

// Published storage supplies immutable numeric reads. Exact file/mapping
// binding checks belong to that storage representation, never to a model.
class EventVrsInputView {
public:
    virtual ~EventVrsInputView() = default;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual std::string snapshot_id() const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual std::uint64_t node_count() const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual std::uint64_t edge_count() const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual float direct(std::uint32_t node) const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual float score(std::uint32_t node) const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual bool unresolved(std::uint32_t node) const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual EventEdge edge(std::uint32_t address) const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual float strength(std::uint32_t address) const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:66-76
    virtual void require_immutable_binding() const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] virtual const EndpointDependencyIndex& dependencies() const = 0;
};

// Only this cold-validated wrapper may enter numerical event advancement.
class ValidatedEventVrsInputs {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-65
    explicit ValidatedEventVrsInputs(std::shared_ptr<const EventVrsInputView> source);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:68-76
    [[nodiscard]] const EventVrsInputView& require_validated_immutable() const;

private:
    // The author's prepare_event_delta validates only edits and appended
    // values, then records bindings on a successor sharing the cold parent.
    // Only EventDeltaView can enter this path after verifying the prefix.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
    ValidatedEventVrsInputs(std::shared_ptr<const EventVrsInputView> source,
                            const ValidatedEventVrsInputs& parent,
                            std::uint64_t appended_nodes, std::uint64_t appended_edges);

    std::shared_ptr<const EventVrsInputView> source_;
    std::string snapshot_id_;
    std::uint64_t node_count_;
    std::uint64_t edge_count_;
    friend class EventDeltaView;
};

}  // namespace swegca::vrs
