#pragma once

#include "event_vrs_inputs.hpp"
#include "native_endpoint_index.hpp"
#include "native_graph_numeric_append.hpp"
#include "native_graph_page_file.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace swegca::vrs {

// One immutable numerical Graph read generation. Page maps, page files and
// endpoint segments carry the same original-journal generation. The endpoint
// index is constructed against this exact object before the validated wrapper
// can escape, preserving the author's identity-bound dependency check.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
class NativeGraphNumericView final : public EventVrsInputView {
public:
    // Cold open validates every numerical value and endpoint through
    // ValidatedEventVrsInputs. It is a recovery/publication boundary, never a
    // user-input hot operation.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] static std::shared_ptr<const ValidatedEventVrsInputs>
    open_validated(
        NativeGraphNumericPageState state,
        std::shared_ptr<const NativeGraphPageFile> node_file,
        std::shared_ptr<const NativeGraphPageFile> edge_file,
        std::filesystem::path endpoint_directory,
        std::uint64_t endpoint_base_edge_count,
        std::vector<NativeEndpointSegment> endpoint_segments,
        OwnerLock* writer = nullptr);

    // Rebase a just-prepared EventDelta successor onto its immutable pages
    // and native endpoint segments. This prevents the sparse delta chain from
    // becoming the resident Main representation.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:170-180
    [[nodiscard]] static std::shared_ptr<const ValidatedEventVrsInputs>
    rebase_validated_successor(
        NativeGraphNumericPageState state,
        std::shared_ptr<const NativeGraphPageFile> node_file,
        std::shared_ptr<const NativeGraphPageFile> edge_file,
        const ValidatedEventVrsInputs& prepared_successor,
        OwnerLock* writer = nullptr);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] std::string snapshot_id() const override {
        return state_.graph_snapshot_id;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] std::uint64_t node_count() const override {
        return state_.node_count;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] std::uint64_t edge_count() const override {
        return state_.edge_count;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] float direct(std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] float score(std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] bool unresolved(std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] EventEdge edge(std::uint32_t address) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] float strength(std::uint32_t address) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:66-76
    void require_immutable_binding() const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] const EndpointDependencyIndex& dependencies() const override;

private:
    NativeGraphNumericView(
        NativeGraphNumericPageState state,
        std::shared_ptr<const NativeGraphPageFile> node_file,
        std::shared_ptr<const NativeGraphPageFile> edge_file);

    [[nodiscard]] NativeGraphNodePage node_page(std::uint32_t node) const;
    [[nodiscard]] NativeGraphEdgePage edge_page(std::uint32_t edge) const;

    NativeGraphNumericPageState state_;
    std::shared_ptr<const NativeGraphPageFile> node_file_;
    std::shared_ptr<const NativeGraphPageFile> edge_file_;
    std::shared_ptr<const NativeEndpointDependencyIndex> dependencies_;
};

}  // namespace swegca::vrs
