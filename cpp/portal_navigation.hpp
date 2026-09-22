#pragma once

#include "portal_lifecycle.hpp"
#include "region_navigation.hpp"
#include "region_preactivation.hpp"
#include "memory_recall.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace swegca::vrs {

// Disconnected Graph components keep separate topology identities. Component
// order follows the first matched cue; each component retains source region
// weight order without comparing coefficients from unrelated topologies.
struct RegionOrigin {
    std::uint32_t component;
    std::string topology_id;
    std::uint32_t region;
    double weight;
};

struct PortalNavigationFailure {
    std::optional<std::uint32_t> component;
    std::optional<std::uint32_t> origin_region;
    std::string reason;
};

// A completed pre-Replay read. Recall retains all author-selected addresses;
// page work only adds navigation cues and cannot act as an allowlist.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_activation.py@c06092a:50-68
struct PortalNavigationRecall {
    DejaVuSignal signal;
    std::vector<RegionPreactivation> preactivations;
    std::vector<PortalPlan> plans;
    std::optional<PortalCandidate> selected;
    std::vector<RegionOrigin> deferred_regions;
    std::vector<std::string> navigation_cues;
    std::vector<PortalNavigationFailure> navigation_failures;
    std::optional<RegionNavigationPage> navigation_page;
    RecallResult recall;
    bool complete_transitive_search = false;
    bool action_authorized = false;
    bool persistent_write_authorized = false;
};

// The caller obtained signal directly from user input before any other memory
// operation. This ports only the author's navigation and Recall prefix;
// selected-original Replay and Re-evidence follow in Main's four-stage path.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_activation.py@c06092a:81-122
[[nodiscard]] PortalNavigationRecall recall_after_deja_vu_navigation(
    const FullCurrentMemoryVrsSnapshot& pair, const DejaVuSignal& signal,
    const EventVrsInputView& inputs, const GraphNodeDirectory& nodes,
    const GraphRegionDirectory& regions,
    const CoactivationAssociations& associations, const PortalPolicy& policy,
    std::int64_t observed_at_ns, std::uint64_t navigation_term_budget,
    const std::vector<PortalRevocation>& revocations = {});

}  // namespace swegca::vrs
