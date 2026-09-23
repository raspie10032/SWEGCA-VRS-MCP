#include "swegca_vrs/autonomy_event_record.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// The blob visitor never stops early: for a parted blob, its final visit
// checks the complete digest. Only then can finish() publish a parsed view.
// This is a native adapter over the author's addressed observation/read and
// autonomous event; it does not classify lineage or authorize a transition.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:107-131
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
AutonomyEventRead read_autonomy_event(
    const ExperienceRecord& record, AutonomyEventPlacement placement,
    const AllocationContext& memory) {
    if (placement != AutonomyEventPlacement::raw &&
        placement != AutonomyEventPlacement::structured)
        throw std::invalid_argument("autonomy_event_placement_invalid");
    if (!record.has_pinned_read_snapshot())
        throw std::invalid_argument("autonomy_event_unpinned_record");

    AutonomyEventParser parser(memory);
    const auto feed = [&parser](std::span<const std::byte> chunk) {
        parser.feed(chunk);
        return true;
    };
    const ExperienceBlob* blob = nullptr;
    if (placement == AutonomyEventPlacement::raw) {
        blob = &record.raw_blob();
        record.for_each_raw_chunk(feed);
    } else {
        blob = &record.structured_blob();
        record.for_each_structured_chunk(feed);
    }

    auto event = parser.finish();
    const auto& published = record.record();
    AutonomyEventRecordBinding binding{
        ExperienceAddress(memory, published.address), record.position(),
        published.record_digest, published.kind, placement, blob->digest};
    return {std::move(event), std::move(binding)};
}

}  // namespace swegca::vrs
