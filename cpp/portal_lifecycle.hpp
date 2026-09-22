#pragma once

#include "coactivation_associations.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

// The source's nonnegative Fraction with explicit finite C++ storage. The
// observed nanosecond ratio fits this range; an oversized policy is rejected.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:15-27
struct PortalFraction {
    std::uint64_t numerator;
    std::uint64_t denominator;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:140-148
    PortalFraction(std::uint64_t numerator, std::uint64_t denominator);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:140-148
    [[nodiscard]] bool operator==(const PortalFraction&) const = default;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:15-27
struct PortalPolicy {
    std::string version;
    std::int64_t half_life_ns;
    std::int64_t maximum_age_ns;
    PortalFraction minimum_mass;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:22-27
    PortalPolicy(std::string version, std::int64_t half_life_ns,
                 std::int64_t maximum_age_ns, PortalFraction minimum_mass);
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:30-45
struct PortalKey {
    std::string pair_snapshot_id;
    std::string topology_id;
    std::uint32_t origin_region;
    std::uint32_t destination_region;
    std::string episode_id;
    std::string revision;
    std::vector<std::string> source_addresses;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:30-45
    [[nodiscard]] bool operator<(const PortalKey& other) const;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:30-45
    [[nodiscard]] bool operator==(const PortalKey&) const = default;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:41-45
[[nodiscard]] PortalKey portal_key(const ObservedSharedAssociation& association);

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:48-58
struct PortalRevocation {
    PortalKey key;
    std::int64_t observed_at_ns;
    std::string reason;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:54-58
    PortalRevocation(PortalKey key, std::int64_t observed_at_ns, std::string reason);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:48-58
    [[nodiscard]] bool operator==(const PortalRevocation&) const = default;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:61-72
struct PortalCandidate {
    PortalKey key;
    ObservedSharedAssociation association;
    double decayed_coactivation_mass;
    std::vector<std::pair<std::string, PortalFraction>> request_contributions;
    std::int64_t age_ns;
    bool eligible;
    std::vector<std::string> rejection_reasons;
    std::vector<PortalRevocation> revocations;
    std::optional<double> measured_retrieval_usefulness;
    bool grants_authority = false;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:75-86
struct PortalPlan {
    CoactivationAssociationsReceipt source_receipt;
    PortalPolicy policy;
    std::int64_t observed_at_ns;
    std::vector<PortalCandidate> candidates;
    std::optional<PortalCandidate> selected;
    std::vector<PortalRevocation> supplied_revocations;
    std::vector<PortalRevocation> retained_other_generation_revocations;
    bool ordinary_recall_available = true;
    bool complete_memory_search = false;
    bool grants_authority = false;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:89-156
[[nodiscard]] PortalPlan plan_portals(
    const CoactivationAssociationsReceipt& receipt, const PortalPolicy& policy,
    std::int64_t observed_at_ns,
    const std::vector<PortalRevocation>& revocations = {});

}  // namespace swegca::vrs
