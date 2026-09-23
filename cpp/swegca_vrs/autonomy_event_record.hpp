#pragma once

#include "swegca_vrs/autonomy_event_codec.hpp"
#include "swegca_vrs/experience.hpp"

#include <cstdint>

// An event is read from one addressed experience, not accepted as an
// unbound caller-supplied view. The Main route still decides which blob
// carries the producer's event, whether the experience is original or
// derived, and how its source and context relate to the event. This adapter
// grants no evidence or action authority.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:107-131
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
namespace swegca::vrs {

enum class AutonomyEventPlacement : std::uint8_t { raw = 1, structured = 2 };

// The exact published record and blob from which the parsed event came.
// This owns the address so a later Main receipt can name it after the
// ExperienceRecord object is gone. These fields are data, not a capability.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
struct AutonomyEventRecordBinding final {
    ExperienceAddress address;
    journal::RecordPosition position;
    DigestBytes record_digest{};
    std::uint16_t kind = 0;
    AutonomyEventPlacement placement;
    DigestBytes blob_digest{};
};

struct AutonomyEventRead final {
    ParsedAutonomyEvent event;
    AutonomyEventRecordBinding binding;
};

// `record` must have been decoded by Main on a pinned journal read snapshot;
// this adapter refuses an unpinned record. Main still verifies that the
// pinned generation is the one selected by its committed receipt.
// It always visits the chosen blob to completion, allowing the experience
// reader to verify its whole digest before the event parser is finished.
// No placement default is provided: Main must choose it from the actual
// producer record format, rather than treating a different blob as evidence.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:107-131
[[nodiscard]] AutonomyEventRead read_autonomy_event(
    const ExperienceRecord& record, AutonomyEventPlacement placement,
    const AllocationContext& memory);

}  // namespace swegca::vrs
