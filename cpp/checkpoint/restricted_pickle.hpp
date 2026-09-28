#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace swegca::checkpoint {

// This is a symbolic record format, not a deserialized Python object graph.
// No GLOBAL or REDUCE target is called.
enum class SymbolicKind : std::uint8_t {
    none,
    boolean,
    integer,
    real,
    string,
    list,
    tuple,
    dictionary,
    ordered_dictionary,
    global,
    storage,
    tensor,
};

enum class StorageType : std::uint8_t { half, float32, bfloat16, int64 };

struct StorageRecord {
    StorageType type = StorageType::float32;
    std::string key;
    std::string device;
    std::uint64_t elements = 0;
};

struct TensorRecord {
    StorageRecord storage;
    std::uint64_t storage_offset = 0;
    std::vector<std::uint64_t> shape;
    std::vector<std::uint64_t> stride;
    bool requires_grad = false;
};

struct SymbolicValue;
using Symbolic = std::shared_ptr<SymbolicValue>;

struct SymbolicValue {
    SymbolicKind kind = SymbolicKind::none;
    bool boolean = false;
    std::int64_t integer = 0;
    double real = 0;
    std::string text;
    std::vector<Symbolic> sequence;
    std::vector<std::pair<Symbolic, Symbolic>> entries;
    StorageRecord storage;
    TensorRecord tensor;
};

struct RestrictedPickleLimits {
    std::size_t maximum_input_bytes = 16U << 20;
    std::size_t maximum_opcodes = 2'000'000;
    std::size_t maximum_stack_depth = 1'000'000;
    std::size_t maximum_memo_entries = 1'000'000;
    std::size_t maximum_container_items = 1'000'000;
    std::size_t maximum_total_nodes = 4'000'000;
    std::size_t maximum_string_bytes = 1U << 20;
    std::size_t maximum_total_string_bytes = 64U << 20;
    std::size_t maximum_tensor_rank = 32;
};

struct RestrictedPickleResult {
    Symbolic root;
    std::size_t bytes_consumed = 0;
    std::size_t opcode_count = 0;
};

class RestrictedPickleError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Accepts only the exact protocol-2 opcode and GLOBAL/reducer family observed
// in the three audited PyTorch ZIP checkpoints. Persistent storage IDs and
// _rebuild_tensor_v2 become inert records. Every other operation fails closed.
[[nodiscard]] RestrictedPickleResult parse_restricted_pickle(
    std::span<const std::byte> input,
    const RestrictedPickleLimits& limits = {});

}  // namespace swegca::checkpoint
