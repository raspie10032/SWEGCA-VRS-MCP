#pragma once

#include "swegca_architecture/content_observation_kernel.hpp"
#include "swegca_architecture/sha256.hpp"
#include "vrs/transfer_budget.hpp"

#include <memory_resource>
#include <sys/stat.h>

namespace swegca::vrs {

struct FileReadObservation {
    struct stat before{}, after{};
    architecture::DigestBytes digest{};
    std::uint64_t bytes_read = 0;
};

struct FileEqualityObservation {
    FileReadObservation left, right;
    architecture::kernel::EvidenceOutcome outcome =
        architecture::kernel::EvidenceOutcome::insufficient;
    bool complete = false, stable = false;
    int io_error = 0;
};

// Measure two regular files already opened by the authorized caller. No path
// resolution, command execution, VRS mutation or input-meaning inference here.
// pread preserves both descriptor positions; the caller retains ownership.
// max_bytes bounds each complete file. Fixed scratch space is charged to the
// caller's resource, all requested reads to its shared transfer budget.
// Allocation/budget errors propagate, I/O failures produce insufficient.
// Stability checks detect observed size/metadata changes, not an atomic pair
// snapshot. Use immutable snapshots if that stronger property is required.
[[nodiscard]] FileEqualityObservation observe_file_equality(
    int left, int right, std::uint64_t max_bytes,
    std::pmr::memory_resource&, TransferBudget&, bool expect_equal = true);

} // namespace swegca::vrs
