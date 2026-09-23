#pragma once

#include "swegca_vrs/core_digest.hpp"

#include "swegca_architecture/digest_bytes.hpp"

#include <compare>
#include <cstdint>
#include <optional>

// The exact position of one published journal record, on its own so that
// shells which only cite records (evidence admission) need not see the store.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:118 (evidence is an addressable record).
namespace swegca::vrs::journal {

// Exact position of one published record plus what it must still carry.
struct RecordPosition {
    std::uint64_t segment_ordinal = 0;
    std::uint64_t byte_offset = 0;
    std::uint64_t sequence = 0;
    DigestBytes record_digest{};

    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:196-205
    auto operator<=>(const RecordPosition&) const = default;
};

// A journal manifest's state fields are data, not a Main publication
// capability. The all-zero/absent pair is the fresh journal before genesis.
// Main verifies a present pair against its selected marker and kind-7 record
// before constructing a PublishedStateId.
struct StateHeadReference {
    DigestBytes content_digest{};
    std::optional<RecordPosition> publication;

    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:196-205
    auto operator<=>(const StateHeadReference&) const = default;
};

}  // namespace swegca::vrs::journal
