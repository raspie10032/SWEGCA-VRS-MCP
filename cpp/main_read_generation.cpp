#include "main_read_generation.hpp"

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
    PortalPolicy policy, std::vector<PortalRevocation> revocations,
    std::shared_ptr<const NativeJournalReadView> journal,
    std::shared_ptr<const ExactJournalDirectory> original_addresses,
    std::shared_ptr<const NativeCueDirectory> cue_addresses,
    std::shared_ptr<const NativeCueDirectory> proposition_addresses,
    std::shared_ptr<const NativeCueDirectory> successor_addresses,
    std::int64_t published_row_limit)
    // SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
    : pair_(std::move(pair)), inputs_(std::move(inputs)),
      nodes_(std::move(nodes)), regions_(std::move(regions)),
      associations_(std::move(associations)), policy_(std::move(policy)),
      revocations_(std::move(revocations)), journal_(std::move(journal)),
      original_addresses_(std::move(original_addresses)),
      cue_addresses_(std::move(cue_addresses)),
      proposition_addresses_(std::move(proposition_addresses)),
      successor_addresses_(std::move(successor_addresses)),
      published_row_limit_(published_row_limit) {
    if (!pair_ || !inputs_ || !nodes_ || !regions_ || !associations_ ||
        !journal_ || !original_addresses_ || !cue_addresses_ ||
        !proposition_addresses_ || !successor_addresses_)
        throw std::runtime_error("complete Main read generation required");
    if (std::filesystem::equivalent(cue_addresses_->directory(),
                                    proposition_addresses_->directory()) ||
        std::filesystem::equivalent(cue_addresses_->directory(),
                                    successor_addresses_->directory()) ||
        std::filesystem::equivalent(proposition_addresses_->directory(),
                                    successor_addresses_->directory()))
        throw std::runtime_error("Main posting directories alias");
    const auto& source = inputs_->require_validated_immutable();
    if (pair_->vrs_snapshot_id() != source.snapshot_id())
        throw std::runtime_error("read generation belongs to another VRS snapshot");
    nodes_->require_source(source);
    regions_->require_source(source);
    regions_->require_memory_source(pair_->memory());
    const auto publication = original_addresses_->publication();
    const auto cue_publication = cue_addresses_->publication();
    const auto proposition_publication = proposition_addresses_->publication();
    const auto successor_publication = successor_addresses_->publication();
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
}

// SWEGCA: user@2026-09-22:13-21
PinnedReadLayer MainReadGeneration::layer() const {
    return PinnedReadLayer{*pair_, inputs_->require_validated_immutable(), *nodes_,
                           *regions_, *associations_, policy_, revocations_,
                           *journal_, *original_addresses_, *cue_addresses_,
                           *proposition_addresses_, *successor_addresses_,
                           published_row_limit_};
}

}  // namespace swegca::vrs
