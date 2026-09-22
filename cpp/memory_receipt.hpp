#pragma once

#include "deja_vu.hpp"
#include "memory_evidence.hpp"
#include "memory_recall.hpp"

#include <array>
#include <string>

namespace swegca::vrs {

class MemoryActivationReceipt {
public:
    const std::string schema_version;
    const std::string snapshot_id;
    const DejaVuSignal deja_vu;
    const RecallResult recall;
    const ReplayResult replay;
    const ReEvidenceResult re_evidence;
    const std::array<std::string, 4> stage_order;
    const bool action_authorized;
    const bool persistent_write_authorized;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:558-593
    MemoryActivationReceipt(
        std::string schema_version, std::string snapshot_id,
        DejaVuSignal deja_vu, RecallResult recall, ReplayResult replay,
        ReEvidenceResult re_evidence,
        std::array<std::string, 4> stage_order = {
            "deja_vu", "recall", "replay", "re_evidence"},
        bool action_authorized = false, bool persistent_write_authorized = false);
};

// Product source guarded every returned opened row against all three stage
// identities. The C++ receipt checks all opened rows before publication.
// SWEGCA: src/swegca_vrs2/native_context.py@c06092a:177-224
void validate_opened_identity(const MemoryActivationReceipt& receipt);

}  // namespace swegca::vrs
