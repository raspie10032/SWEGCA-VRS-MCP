#include "main_read_generation.hpp"

#include "native_published_hot_index.hpp"
#include "native_graph_node_directory.hpp"

#include <array>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
// SWEGCA: user@2026-09-22:13-21
MainReadGeneration::MainReadGeneration(
    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> pair,
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    std::shared_ptr<const GraphNodeDirectory> nodes,
    std::shared_ptr<const GraphRegionDirectory> regions,
    std::shared_ptr<const CoactivationAssociations> associations,
    std::shared_ptr<const GraphAuxiliaryState> auxiliary,
    PortalPolicy policy, std::vector<PortalRevocation> revocations,
    std::shared_ptr<const NativeJournalReadView> journal,
    std::shared_ptr<const ExactJournalDirectory> original_addresses,
    std::shared_ptr<const NativeCueDirectory> cue_addresses,
    std::shared_ptr<const NativeCueDirectory> proposition_addresses,
    std::shared_ptr<const NativeCueDirectory> successor_addresses,
    std::shared_ptr<const NativeCueDirectory> source_addresses,
    std::shared_ptr<const NativeOperationDirectory> operations,
    std::int64_t published_row_limit)
    // SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
    : pair_(std::move(pair)), inputs_(std::move(inputs)),
      nodes_(std::move(nodes)), regions_(std::move(regions)),
      associations_(std::move(associations)), auxiliary_(std::move(auxiliary)),
      policy_(std::move(policy)),
      revocations_(std::move(revocations)), journal_(std::move(journal)),
      original_addresses_(std::move(original_addresses)),
      cue_addresses_(std::move(cue_addresses)),
      proposition_addresses_(std::move(proposition_addresses)),
      successor_addresses_(std::move(successor_addresses)),
      source_addresses_(std::move(source_addresses)),
      operations_(std::move(operations)),
      // SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
      published_row_limit_(published_row_limit) {
    if (!pair_ || !inputs_ || !nodes_ || !regions_ || !associations_ ||
        !auxiliary_ ||
        !journal_ || !original_addresses_ || !cue_addresses_ ||
        !proposition_addresses_ || !successor_addresses_ ||
        !source_addresses_ || !operations_)
        throw std::runtime_error("complete Main read generation required");
    const std::array<const NativeCueDirectory*, 4> directories{
        cue_addresses_.get(), proposition_addresses_.get(),
        successor_addresses_.get(), source_addresses_.get()};
    for (std::size_t left = 0; left < directories.size(); ++left)
        for (std::size_t right = left + 1; right < directories.size(); ++right)
            if (std::filesystem::equivalent(directories[left]->directory(),
                                             directories[right]->directory()))
                throw std::runtime_error("Main posting directories alias");
    const auto& source = inputs_->require_validated_immutable();
    const auto* native_memory =
        dynamic_cast<const NativePublishedHotIndex*>(&pair_->memory());
    if (!native_memory ||
        native_memory->pair_snapshot_id() != pair_->snapshot_id() ||
        native_memory->published_row_limit() != published_row_limit_ ||
        &native_memory->originals() != original_addresses_.get() ||
        &native_memory->cues() != cue_addresses_.get() ||
        &native_memory->propositions() != proposition_addresses_.get() ||
        &native_memory->successors() != successor_addresses_.get() ||
        &native_memory->sources() != source_addresses_.get())
        throw std::runtime_error("Main HotIndex read generation changed");
    if (pair_->vrs_snapshot_id() != source.snapshot_id())
        throw std::runtime_error("read generation belongs to another VRS snapshot");
    if (auxiliary_->snapshot_id != pair_->vrs_snapshot_id())
        throw std::runtime_error("Main auxiliary Graph generation changed");
    nodes_->require_source(source);
    const auto* native_nodes =
        dynamic_cast<const NativeGraphNodeDirectory*>(nodes_.get());
    std::optional<GraphNodePublication> node_publication;
    if (native_nodes) node_publication = native_nodes->publication();
    if (!native_nodes || !native_nodes->published_reader() ||
        !node_publication ||
        node_publication->journal_generation != journal_->generation() ||
        node_publication->graph_snapshot_id != pair_->vrs_snapshot_id() ||
        node_publication->pair_snapshot_id != pair_->snapshot_id() ||
        node_publication->published_rows != published_row_limit_ ||
        node_publication->node_count != source.node_count())
        throw std::runtime_error("Main Graph node read generation changed");
    if (std::filesystem::equivalent(native_nodes->directory(),
                                    original_addresses_->directory()) ||
        std::filesystem::equivalent(native_nodes->directory(),
                                    cue_addresses_->directory()) ||
        std::filesystem::equivalent(native_nodes->directory(),
                                    proposition_addresses_->directory()) ||
        std::filesystem::equivalent(native_nodes->directory(),
                                    successor_addresses_->directory()) ||
        std::filesystem::equivalent(native_nodes->directory(),
                                    source_addresses_->directory()) ||
        std::filesystem::equivalent(native_nodes->directory(),
                                    operations_->directory()))
        throw std::runtime_error("Main Graph node directory aliases memory");
    regions_->require_source(source);
    regions_->require_memory_source(pair_->memory());
    const auto publication = original_addresses_->publication();
    const auto cue_publication = cue_addresses_->publication();
    const auto proposition_publication = proposition_addresses_->publication();
    const auto successor_publication = successor_addresses_->publication();
    const auto source_publication = source_addresses_->publication();
    const auto operation_publication = operations_->publication();
    if (!original_addresses_->published_reader() || published_row_limit_ < 0 ||
        static_cast<std::uint64_t>(published_row_limit_) > journal_->row_count() ||
        journal_->generation() != original_addresses_->journal_generation() ||
        !publication || publication->published_rows != published_row_limit_ ||
        publication->journal_generation != journal_->generation() ||
        publication->pair_snapshot_id != pair_->snapshot_id())
        throw std::runtime_error("Main original read generation changed");
    if (!cue_addresses_->published_reader() || !cue_publication ||
        cue_addresses_->journal_generation() != journal_->generation() ||
        cue_publication->journal_generation != journal_->generation() ||
        cue_publication->published_rows != published_row_limit_ ||
        cue_publication->pair_snapshot_id != pair_->snapshot_id())
        throw std::runtime_error("Main cue read generation changed");
    if (!proposition_addresses_->published_reader() ||
        !proposition_publication ||
        proposition_addresses_->journal_generation() != journal_->generation() ||
        proposition_publication->journal_generation != journal_->generation() ||
        proposition_publication->published_rows != published_row_limit_ ||
        proposition_publication->pair_snapshot_id != pair_->snapshot_id())
        throw std::runtime_error("Main proposition read generation changed");
    if (!successor_addresses_->published_reader() || !successor_publication ||
        successor_addresses_->journal_generation() != journal_->generation() ||
        successor_publication->journal_generation != journal_->generation() ||
        successor_publication->published_rows != published_row_limit_ ||
        successor_publication->pair_snapshot_id != pair_->snapshot_id())
        throw std::runtime_error("Main successor read generation changed");
    if (!source_addresses_->published_reader() || !source_publication ||
        source_addresses_->journal_generation() != journal_->generation() ||
        source_publication->journal_generation != journal_->generation() ||
        source_publication->published_rows != published_row_limit_ ||
        source_publication->pair_snapshot_id != pair_->snapshot_id())
        throw std::runtime_error("Main source read generation changed");
    if (!operations_->published_reader() || !operation_publication ||
        operations_->journal_generation() != journal_->generation() ||
        operation_publication->journal_generation != journal_->generation() ||
        operation_publication->published_rows != published_row_limit_ ||
        operation_publication->pair_snapshot_id != pair_->snapshot_id())
        throw std::runtime_error("Main operation read generation changed");
}

// SWEGCA: user@2026-09-22:13-21
PinnedReadLayer MainReadGeneration::layer() const {
    return PinnedReadLayer{*pair_, inputs_->require_validated_immutable(), *nodes_,
                           *regions_, *associations_, *auxiliary_, policy_,
                           revocations_,
                           *journal_, *original_addresses_, *cue_addresses_,
                           *proposition_addresses_, *successor_addresses_,
                           *source_addresses_,
                           published_row_limit_};
}

}  // namespace swegca::vrs
