#include "checkpoint/restricted_pickle.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace swegca::checkpoint {
namespace {

constexpr std::string_view rebuild_tensor = "torch._utils _rebuild_tensor_v2";
constexpr std::string_view ordered_dict = "collections OrderedDict";
constexpr std::string_view half_storage = "torch HalfStorage";
constexpr std::string_view float_storage = "torch FloatStorage";
constexpr std::string_view bfloat_storage = "torch BFloat16Storage";
constexpr std::string_view long_storage = "torch LongStorage";

class Parser final {
public:
    Parser(std::span<const std::byte> input, const RestrictedPickleLimits& limits)
        : input_(input), limits_(limits) {
        if (input_.empty() || input_.size() > limits_.maximum_input_bytes)
            fail("pickle input size is outside the configured bound");
        if (!limits_.maximum_opcodes || !limits_.maximum_stack_depth ||
            !limits_.maximum_memo_entries || !limits_.maximum_container_items ||
            !limits_.maximum_total_nodes || !limits_.maximum_string_bytes ||
            !limits_.maximum_tensor_rank)
            fail("pickle limits must be nonzero");
    }

    RestrictedPickleResult parse() {
        bool protocol_seen = false;
        while (offset_ < input_.size()) {
            if (++opcode_count_ > limits_.maximum_opcodes) fail("pickle opcode limit exceeded");
            const auto opcode = byte();
            switch (opcode) {
            case 0x80:  // PROTO
                if (protocol_seen || offset_ != 1 || byte() != 2) fail("only pickle protocol 2 is accepted");
                protocol_seen = true;
                break;
            case '.': {  // STOP
                if (!protocol_seen) fail("pickle protocol header is missing");
                if (offset_ != input_.size()) fail("trailing data follows pickle STOP");
                if (stack_.size() != 1 || is_mark(stack_.back())) fail("pickle STOP has an invalid stack");
                return {stack_.back(), offset_, opcode_count_};
            }
            case 'N': push(make(SymbolicKind::none)); break;
            case 0x88: push_boolean(true); break;   // NEWTRUE
            case 0x89: push_boolean(false); break; // NEWFALSE
            case 'J': push_integer(signed32()); break; // BININT
            case 'K': push_integer(byte()); break;     // BININT1
            case 'M': push_integer(unsigned16()); break; // BININT2
            case 'G': push_real(binary_float()); break; // BINFLOAT
            case 'X': push_string(string32()); break;   // BINUNICODE
            case ']': push(make(SymbolicKind::list)); break; // EMPTY_LIST
            case '}': push(make(SymbolicKind::dictionary)); break; // EMPTY_DICT
            case ')': push(make(SymbolicKind::tuple)); break; // EMPTY_TUPLE
            case '(': push(mark_); break; // MARK
            case 't': tuple_from_mark(); break; // TUPLE
            case 0x85: fixed_tuple(1); break; // TUPLE1
            case 0x86: fixed_tuple(2); break; // TUPLE2
            case 0x87: fixed_tuple(3); break; // TUPLE3
            case 'a': append_one(); break; // APPEND
            case 'e': append_many(); break; // APPENDS
            case 'u': setitems(); break; // SETITEMS
            case 'q': memo_put(byte()); break; // BINPUT
            case 'r': memo_put(unsigned32()); break; // LONG_BINPUT
            case 'h': memo_get(byte()); break; // BINGET
            case 'j': memo_get(unsigned32()); break; // LONG_BINGET
            case 'c': push_global(global_name()); break; // GLOBAL
            case 'Q': persistent_id(); break; // BINPERSID
            case 'R': reduce(); break; // REDUCE
            default: fail("unknown or forbidden pickle opcode");
            }
        }
        fail("pickle ended before STOP");
    }

private:
    [[noreturn]] static void fail(std::string_view reason) {
        throw RestrictedPickleError(std::string(reason));
    }

    std::uint8_t byte() {
        if (offset_ == input_.size()) fail("truncated pickle operand");
        return std::to_integer<std::uint8_t>(input_[offset_++]);
    }

    std::uint16_t unsigned16() {
        const auto a = byte(), b = byte();
        return static_cast<std::uint16_t>(a | (std::uint16_t{b} << 8));
    }

    std::uint32_t unsigned32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8) value |= std::uint32_t{byte()} << shift;
        return value;
    }

    std::int32_t signed32() { return std::bit_cast<std::int32_t>(unsigned32()); }

    double binary_float() {
        std::uint64_t bits = 0;
        for (unsigned index = 0; index != 8; ++index) bits = (bits << 8) | byte();
        const auto value = std::bit_cast<double>(bits);
        if (!std::isfinite(value)) fail("non-finite BINFLOAT is rejected");
        return value;
    }

    std::string line() {
        const auto start = offset_;
        while (offset_ < input_.size() && input_[offset_] != std::byte{'\n'}) ++offset_;
        if (offset_ == input_.size()) fail("unterminated GLOBAL name");
        const auto count = offset_++ - start;
        if (!count || count > limits_.maximum_string_bytes) fail("GLOBAL name length is invalid");
        std::string result(count, '\0');
        std::memcpy(result.data(), input_.data() + start, count);
        if (result.find('\0') != std::string::npos || result.find('\r') != std::string::npos)
            fail("GLOBAL name contains a forbidden byte");
        charge_string(result.size());
        return result;
    }

    std::string string32() {
        const auto count = static_cast<std::size_t>(unsigned32());
        if (count > limits_.maximum_string_bytes || count > input_.size() - offset_)
            fail("BINUNICODE length is invalid");
        std::string result(count, '\0');
        if (count) std::memcpy(result.data(), input_.data() + offset_, count);
        offset_ += count;
        charge_string(count);
        return result;
    }

    void charge_string(std::size_t count) {
        if (count > limits_.maximum_total_string_bytes - total_string_bytes_)
            fail("pickle total string-byte limit exceeded");
        total_string_bytes_ += count;
    }

    Symbolic make(SymbolicKind kind) {
        if (++node_count_ > limits_.maximum_total_nodes) fail("pickle node limit exceeded");
        auto result = std::make_shared<SymbolicValue>();
        result->kind = kind;
        return result;
    }

    void push(Symbolic value) {
        if (stack_.size() == limits_.maximum_stack_depth) fail("pickle stack limit exceeded");
        stack_.push_back(std::move(value));
    }

    Symbolic pop() {
        if (stack_.empty()) fail("pickle stack underflow");
        auto value = std::move(stack_.back());
        stack_.pop_back();
        if (is_mark(value)) fail("pickle operand unexpectedly crossed MARK");
        return value;
    }

    void push_boolean(bool value) { auto out = make(SymbolicKind::boolean); out->boolean = value; push(out); }
    void push_integer(std::int64_t value) { auto out = make(SymbolicKind::integer); out->integer = value; push(out); }
    void push_real(double value) { auto out = make(SymbolicKind::real); out->real = value; push(out); }
    void push_string(std::string value) { auto out = make(SymbolicKind::string); out->text = std::move(value); push(out); }

    static bool is_mark(const Symbolic& value) { return value.get() == mark_identity(); }
    static SymbolicValue* mark_identity() {
        static SymbolicValue identity;
        return &identity;
    }

    std::size_t mark_position() const {
        for (std::size_t index = stack_.size(); index != 0; --index)
            if (is_mark(stack_[index - 1])) return index - 1;
        fail("pickle MARK is missing");
    }

    void check_container_size(std::size_t size) const {
        if (size > limits_.maximum_container_items) fail("pickle container item limit exceeded");
    }

    void tuple_from_mark() {
        const auto mark = mark_position();
        const auto count = stack_.size() - mark - 1;
        check_container_size(count);
        auto result = make(SymbolicKind::tuple);
        result->sequence.assign(stack_.begin() + static_cast<std::ptrdiff_t>(mark + 1), stack_.end());
        stack_.resize(mark);
        push(result);
    }

    void fixed_tuple(std::size_t count) {
        if (stack_.size() < count) fail("fixed tuple stack underflow");
        for (std::size_t i = stack_.size() - count; i < stack_.size(); ++i)
            if (is_mark(stack_[i])) fail("fixed tuple crossed MARK");
        auto result = make(SymbolicKind::tuple);
        result->sequence.assign(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
        stack_.resize(stack_.size() - count);
        push(result);
    }

    void append_one() {
        auto value = pop();
        if (stack_.empty() || stack_.back()->kind != SymbolicKind::list) fail("APPEND target is not a list");
        check_container_size(stack_.back()->sequence.size() + 1);
        stack_.back()->sequence.push_back(std::move(value));
    }

    void append_many() {
        const auto mark = mark_position();
        if (mark == 0 || stack_[mark - 1]->kind != SymbolicKind::list) fail("APPENDS target is not a list");
        auto& target = stack_[mark - 1]->sequence;
        const auto count = stack_.size() - mark - 1;
        if (count > limits_.maximum_container_items - target.size()) fail("pickle list item limit exceeded");
        target.insert(target.end(), stack_.begin() + static_cast<std::ptrdiff_t>(mark + 1), stack_.end());
        stack_.resize(mark);
    }

    void setitems() {
        const auto mark = mark_position();
        if (mark == 0) fail("SETITEMS target is missing");
        auto& dictionary = stack_[mark - 1];
        if (dictionary->kind != SymbolicKind::dictionary && dictionary->kind != SymbolicKind::ordered_dictionary)
            fail("SETITEMS target is not a dictionary");
        const auto count = stack_.size() - mark - 1;
        if (count % 2) fail("SETITEMS has an unmatched key");
        const auto pairs = count / 2;
        if (pairs > limits_.maximum_container_items - dictionary->entries.size())
            fail("pickle dictionary item limit exceeded");
        for (std::size_t index = mark + 1; index < stack_.size(); index += 2)
            dictionary->entries.emplace_back(stack_[index], stack_[index + 1]);
        stack_.resize(mark);
    }

    void memo_put(std::uint32_t index) {
        if (stack_.empty() || is_mark(stack_.back())) fail("memo write has no value");
        if (index >= limits_.maximum_memo_entries) fail("pickle memo index exceeds limit");
        if (memo_.contains(index)) fail("pickle memo index was overwritten");
        memo_.emplace(index, stack_.back());
    }

    void memo_get(std::uint32_t index) {
        const auto found = memo_.find(index);
        if (found == memo_.end()) fail("pickle memo reference is missing");
        push(found->second);
    }

    std::string global_name() {
        auto module = line();
        auto name = line();
        module.push_back(' ');
        module += name;
        if (module != rebuild_tensor && module != ordered_dict && module != half_storage &&
            module != float_storage && module != bfloat_storage && module != long_storage)
            fail("pickle GLOBAL is not whitelisted");
        return module;
    }

    void push_global(std::string name) {
        auto result = make(SymbolicKind::global);
        result->text = std::move(name);
        push(result);
    }

    static bool integer(const Symbolic& value, std::uint64_t& out) {
        if (value->kind != SymbolicKind::integer || value->integer < 0) return false;
        out = static_cast<std::uint64_t>(value->integer);
        return true;
    }

    static StorageType storage_type(std::string_view name) {
        if (name == half_storage) return StorageType::half;
        if (name == float_storage) return StorageType::float32;
        if (name == bfloat_storage) return StorageType::bfloat16;
        if (name == long_storage) return StorageType::int64;
        fail("persistent ID has an invalid storage class");
    }

    void persistent_id() {
        auto id = pop();
        if (id->kind != SymbolicKind::tuple || id->sequence.size() != 5) fail("persistent ID form is invalid");
        const auto& fields = id->sequence;
        if (fields[0]->kind != SymbolicKind::string || fields[0]->text != "storage" ||
            fields[1]->kind != SymbolicKind::global ||
            fields[2]->kind != SymbolicKind::string || fields[2]->text.empty() ||
            fields[3]->kind != SymbolicKind::string || fields[3]->text != "cpu")
            fail("persistent storage ID type mismatch");
        std::uint64_t elements = 0;
        if (!integer(fields[4], elements)) fail("persistent storage size is invalid");
        auto result = make(SymbolicKind::storage);
        result->storage = {storage_type(fields[1]->text), fields[2]->text, fields[3]->text, elements};
        push(result);
    }

    std::vector<std::uint64_t> dimensions(const Symbolic& value, std::string_view what) const {
        if (value->kind != SymbolicKind::tuple || value->sequence.size() > limits_.maximum_tensor_rank)
            fail(std::string(what) + " is not a bounded tuple");
        std::vector<std::uint64_t> result;
        result.reserve(value->sequence.size());
        for (const auto& item : value->sequence) {
            std::uint64_t number = 0;
            if (!integer(item, number)) fail(std::string(what) + " contains a nonnegative-integer mismatch");
            result.push_back(number);
        }
        return result;
    }

    static void validate_tensor_span(const TensorRecord& tensor) {
        if (tensor.shape.size() != tensor.stride.size()) fail("tensor shape and stride ranks differ");
        if (tensor.storage_offset > tensor.storage.elements) fail("tensor storage offset is out of bounds");
        bool empty = false;
        std::uint64_t maximum = tensor.storage_offset;
        for (std::size_t axis = 0; axis < tensor.shape.size(); ++axis) {
            if (tensor.shape[axis] == 0) { empty = true; continue; }
            const auto extent = tensor.shape[axis] - 1;
            if (extent && tensor.stride[axis] > (std::numeric_limits<std::uint64_t>::max() - maximum) / extent)
                fail("tensor span arithmetic overflow");
            maximum += extent * tensor.stride[axis];
        }
        if (!empty && maximum >= tensor.storage.elements) fail("tensor span exceeds storage");
    }

    void reduce() {
        auto args = pop();
        auto callable = pop();
        if (callable->kind != SymbolicKind::global || args->kind != SymbolicKind::tuple)
            fail("REDUCE operand type mismatch");
        if (callable->text == ordered_dict) {
            if (!args->sequence.empty()) fail("OrderedDict reducer arguments are not empty");
            push(make(SymbolicKind::ordered_dictionary));
            return;
        }
        if (callable->text != rebuild_tensor || args->sequence.size() != 6)
            fail("REDUCE target or arity is forbidden");
        const auto& fields = args->sequence;
        if (fields[0]->kind != SymbolicKind::storage || fields[4]->kind != SymbolicKind::boolean ||
            fields[5]->kind != SymbolicKind::ordered_dictionary || !fields[5]->entries.empty())
            fail("_rebuild_tensor_v2 argument type mismatch");
        std::uint64_t offset = 0;
        if (!integer(fields[1], offset)) fail("tensor storage offset is invalid");
        TensorRecord tensor;
        tensor.storage = fields[0]->storage;
        tensor.storage_offset = offset;
        tensor.shape = dimensions(fields[2], "tensor shape");
        tensor.stride = dimensions(fields[3], "tensor stride");
        tensor.requires_grad = fields[4]->boolean;
        validate_tensor_span(tensor);
        auto result = make(SymbolicKind::tensor);
        result->tensor = std::move(tensor);
        push(result);
    }

    std::span<const std::byte> input_;
    const RestrictedPickleLimits& limits_;
    std::size_t offset_ = 0;
    std::size_t opcode_count_ = 0;
    std::size_t node_count_ = 0;
    std::size_t total_string_bytes_ = 0;
    std::vector<Symbolic> stack_;
    std::unordered_map<std::uint32_t, Symbolic> memo_;
    Symbolic mark_{mark_identity(), [](SymbolicValue*) {}};
};

}  // namespace

RestrictedPickleResult parse_restricted_pickle(std::span<const std::byte> input,
                                                const RestrictedPickleLimits& limits) {
    return Parser(input, limits).parse();
}

}  // namespace swegca::checkpoint
