#pragma once

#include "checkpoint/restricted_pickle.hpp"

#include <cstddef>
#include <string>

namespace swegca::checkpoint {

struct CanonicalSymbolicJsonLimits {
    std::size_t maximum_depth = 256;
    std::size_t maximum_output_bytes = 64U << 20;
};

// Serializes only the JSON-compatible subset of SymbolicValue. The byte
// contract is Python json.dumps(value, ensure_ascii=False, sort_keys=True,
// separators=(',', ':')) for valid UTF-8 strings and finite binary64 values.
// Pickle-only values and cyclic graphs fail closed.
[[nodiscard]] std::string canonical_symbolic_json(
    const Symbolic& value,
    const CanonicalSymbolicJsonLimits& limits = {});

}  // namespace swegca::checkpoint
