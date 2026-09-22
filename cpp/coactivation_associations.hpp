#pragma once

#include "coactivation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:17-20
struct CoactivationWitness {
    std::shared_ptr<const CoactivationEvent> event;
    std::size_t experience_index;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:17-20
    [[nodiscard]] const CoactivatedExperience& experience() const {
        return event->experiences.at(experience_index);
    }
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:23-37
struct ObservedSharedAssociation {
    std::uint32_t origin_region;
    std::uint32_t destination_region;
    SharedExperienceBridge bridge;
    std::vector<CoactivationWitness> witnesses;
    std::optional<double> usefulness;
    std::optional<double> causal_relevance;
    bool grants_authority = false;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:34-37
    [[nodiscard]] std::size_t activation_request_count() const { return witnesses.size(); }
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:40-44
struct RejectedCoactivation {
    std::string request_id;
    std::string episode_id;
    std::string reason;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:47-58
struct CoactivationAssociationsReceipt {
    std::string pair_snapshot_id;
    std::string topology_id;
    std::uint32_t origin_region;
    std::vector<ObservedSharedAssociation> associations;
    std::vector<RejectedCoactivation> rejected;
    std::size_t indexed_event_count;
    std::size_t relevant_event_count;
    std::size_t examined_experience_count;
    bool grants_authority = false;
    bool complete_memory_search = false;
};

// Main owns this derived delta index. A missing index never restricts ordinary
// four-stage memory. The event journal remains the source of observation.
class CoactivationAssociations {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:61-69
    CoactivationAssociations() = default;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:67-69
    [[nodiscard]] std::size_t event_count() const { return events_.size(); }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:71-107
    [[nodiscard]] bool add(std::shared_ptr<const CoactivationEvent> event);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:109-162
    [[nodiscard]] CoactivationAssociationsReceipt query(
        const FullCurrentMemoryVrsSnapshot& pair, const EventVrsInputView& inputs,
        const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions,
        std::uint32_t component, std::uint32_t origin_region) const;

private:
    using PostingKey = std::pair<std::string, std::uint32_t>;
    using RequestPostings = std::map<std::string, std::vector<CoactivationWitness>>;
    std::map<std::string, std::shared_ptr<const CoactivationEvent>> events_;
    std::map<PostingKey, RequestPostings> postings_;
    bool failed_ = false;
};

}  // namespace swegca::vrs
