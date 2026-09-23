#include "memory_receipt.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:558-593
MemoryActivationReceipt::MemoryActivationReceipt(
    std::string new_schema_version, std::string new_snapshot_id, DejaVuSignal new_deja_vu,
    RecallResult new_recall, ReplayResult new_replay, ReEvidenceResult new_re_evidence,
    std::array<std::string, 4> new_stage_order, bool new_action_authorized,
    bool new_persistent_write_authorized)
    : schema_version(std::move(new_schema_version)), snapshot_id(std::move(new_snapshot_id)),
      deja_vu(std::move(new_deja_vu)), recall(std::move(new_recall)), replay(std::move(new_replay)),
      re_evidence(std::move(new_re_evidence)), stage_order(std::move(new_stage_order)),
      action_authorized(new_action_authorized), persistent_write_authorized(new_persistent_write_authorized) {
    if (schema_version != "rozephine-memory-activation-v1")
        throw std::runtime_error("memory activation receipt schema changed");
    if (stage_order != std::array<std::string, 4>{
            "deja_vu", "recall", "replay", "re_evidence"})
        throw std::runtime_error("memory activation stage order changed");
    if (snapshot_id != recall.snapshot_id)
        throw std::runtime_error("memory activation snapshot changed between stages");
    if (snapshot_id != deja_vu.snapshot_id)
        throw std::runtime_error("déjà vu snapshot changed between stages");
    if (std::set<std::string>{deja_vu.query, recall.query, replay.query,
                              re_evidence.query}.size() != 1)
        throw std::runtime_error("memory activation query changed between stages");
    if (action_authorized || persistent_write_authorized)
        throw std::runtime_error("memory activation receipt grants no authority");
    validate_opened_identity(*this);
}

// SWEGCA: src/swegca_vrs2/native_context.py@7536139:172-193
void validate_opened_identity(const MemoryActivationReceipt& receipt) {
    const auto size = receipt.recall.candidates.size();
    if (size != receipt.replay.episodes.size() ||
        size != receipt.re_evidence.judgments.size())
        throw std::runtime_error("memory_context_cardinality_changed");
    for (std::size_t index = 0; index < size; ++index) {
        if (receipt.recall.candidates[index].episode_id != receipt.replay.episodes[index].episode_id ||
            receipt.recall.candidates[index].episode_id != receipt.re_evidence.judgments[index].episode_id)
            throw std::runtime_error("memory_context_episode_identity_changed");
    }
}

}  // namespace swegca::vrs
