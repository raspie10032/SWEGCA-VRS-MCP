#pragma once

#include "swegca_architecture/digest_bytes.hpp"

#include <cstdint>

// The exact position of one published journal record, on its own so that
// shells which only cite records (evidence admission) need not see the store.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:118 (evidence is an addressable record).
namespace swegca::architecture::journal {

// Exact position of one published record plus what it must still carry.
struct RecordPosition {
    std::uint64_t segment_ordinal = 0;
    std::uint64_t byte_offset = 0;
    std::uint64_t sequence = 0;
    DigestBytes record_digest{};
};

}  // namespace swegca::architecture::journal
