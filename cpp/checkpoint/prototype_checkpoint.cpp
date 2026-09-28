#include "checkpoint/prototype_checkpoint.hpp"

#include "checkpoint/restricted_pickle.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

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
            case 's': setitem(); break; // SETITEM
            case 'u': setitems(); break; // SETITEMS
            case 'b': build(); break; // BUILD
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

    void setitem() {
        auto value = pop();
        auto key = pop();
        if (stack_.empty()) fail("SETITEM target is missing");
        auto& dictionary = stack_.back();
        if (dictionary->kind != SymbolicKind::dictionary && dictionary->kind != SymbolicKind::ordered_dictionary)
            fail("SETITEM target is not a dictionary");
        check_container_size(dictionary->entries.size() + 1);
        dictionary->entries.emplace_back(std::move(key), std::move(value));
    }

    void build() {
        auto state = pop();
        if (stack_.empty()) fail("BUILD target is missing");
        auto& target = stack_.back();
        if (target->kind != SymbolicKind::ordered_dictionary || state->kind != SymbolicKind::dictionary ||
            state->entries.size() != 1 || !state->entries.front().first ||
            state->entries.front().first->kind != SymbolicKind::string ||
            state->entries.front().first->text != "_metadata")
            fail("Prototype0 BUILD state mismatch");
        target->entries.push_back(state->entries.front());
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
            fields[3]->kind != SymbolicKind::string || fields[3]->text != "cuda:0")
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

static RestrictedPickleResult parse_prototype_pickle(std::span<const std::byte> input,
                                                const RestrictedPickleLimits& limits) {
    return Parser(input, limits).parse();
}

}  // namespace swegca::checkpoint


namespace swegca::checkpoint {
namespace {

constexpr std::array<std::uint64_t, 2> prototype_shape_0{2048, 8};
constexpr std::array<std::int64_t, 2> prototype_stride_0{8, 1};
constexpr std::array<std::uint64_t, 2> prototype_shape_1{2048, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_1{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_2{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_2{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_3{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_3{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_4{2048, 17};
constexpr std::array<std::int64_t, 2> prototype_stride_4{17, 1};
constexpr std::array<std::uint64_t, 2> prototype_shape_5{2048, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_5{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_6{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_6{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_7{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_7{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_8{2048, 11};
constexpr std::array<std::int64_t, 2> prototype_stride_8{11, 1};
constexpr std::array<std::uint64_t, 2> prototype_shape_9{2048, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_9{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_10{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_10{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_11{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_11{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_12{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_12{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_13{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_13{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_14{512, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_14{2048, 1};
constexpr std::array<std::uint64_t, 2> prototype_shape_15{1, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_15{2048, 1};
constexpr std::array<std::uint64_t, 2> prototype_shape_16{3, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_16{2048, 1};
constexpr std::array<std::uint64_t, 0> prototype_shape_17{};
constexpr std::array<std::int64_t, 0> prototype_stride_17{};
constexpr std::array<std::uint64_t, 1> prototype_shape_18{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_18{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_19{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_19{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_20{6144, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_20{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_21{6144};
constexpr std::array<std::int64_t, 1> prototype_stride_21{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_22{2048, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_22{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_23{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_23{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_24{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_24{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_25{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_25{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_26{32768, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_26{2048, 1};
constexpr std::array<std::uint64_t, 2> prototype_shape_27{2048, 16384};
constexpr std::array<std::int64_t, 2> prototype_stride_27{16384, 1};
constexpr std::array<std::uint64_t, 2> prototype_shape_28{1, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_28{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_29{1};
constexpr std::array<std::int64_t, 1> prototype_stride_29{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_30{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_30{1};
constexpr std::array<std::uint64_t, 1> prototype_shape_31{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_31{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_32{1, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_32{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_33{1};
constexpr std::array<std::int64_t, 1> prototype_stride_33{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_34{2048, 512};
constexpr std::array<std::int64_t, 2> prototype_stride_34{512, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_35{2048};
constexpr std::array<std::int64_t, 1> prototype_stride_35{1};
constexpr std::array<std::uint64_t, 2> prototype_shape_36{4, 2048};
constexpr std::array<std::int64_t, 2> prototype_stride_36{2048, 1};
constexpr std::array<std::uint64_t, 1> prototype_shape_37{4};
constexpr std::array<std::int64_t, 1> prototype_stride_37{1};
constexpr std::array<PrototypeTensorProfile, 38> prototype_tensors{{
    PrototypeTensorProfile{"bridges.text.projection.0.weight", "0", TensorDType::float32, prototype_shape_0, prototype_stride_0, 0, 16384, false},
    PrototypeTensorProfile{"bridges.text.projection.2.weight", "1", TensorDType::float32, prototype_shape_1, prototype_stride_1, 0, 4194304, false},
    PrototypeTensorProfile{"bridges.text.projection.3.weight", "2", TensorDType::float32, prototype_shape_2, prototype_stride_2, 0, 2048, false},
    PrototypeTensorProfile{"bridges.text.projection.3.bias", "3", TensorDType::float32, prototype_shape_3, prototype_stride_3, 0, 2048, false},
    PrototypeTensorProfile{"bridges.image.projection.0.weight", "4", TensorDType::float32, prototype_shape_4, prototype_stride_4, 0, 34816, false},
    PrototypeTensorProfile{"bridges.image.projection.2.weight", "5", TensorDType::float32, prototype_shape_5, prototype_stride_5, 0, 4194304, false},
    PrototypeTensorProfile{"bridges.image.projection.3.weight", "6", TensorDType::float32, prototype_shape_6, prototype_stride_6, 0, 2048, false},
    PrototypeTensorProfile{"bridges.image.projection.3.bias", "7", TensorDType::float32, prototype_shape_7, prototype_stride_7, 0, 2048, false},
    PrototypeTensorProfile{"bridges.simple_3d.projection.0.weight", "8", TensorDType::float32, prototype_shape_8, prototype_stride_8, 0, 22528, false},
    PrototypeTensorProfile{"bridges.simple_3d.projection.2.weight", "9", TensorDType::float32, prototype_shape_9, prototype_stride_9, 0, 4194304, false},
    PrototypeTensorProfile{"bridges.simple_3d.projection.3.weight", "10", TensorDType::float32, prototype_shape_10, prototype_stride_10, 0, 2048, false},
    PrototypeTensorProfile{"bridges.simple_3d.projection.3.bias", "11", TensorDType::float32, prototype_shape_11, prototype_stride_11, 0, 2048, false},
    PrototypeTensorProfile{"alignment.readout.0.weight", "12", TensorDType::float32, prototype_shape_12, prototype_stride_12, 0, 2048, false},
    PrototypeTensorProfile{"alignment.readout.0.bias", "13", TensorDType::float32, prototype_shape_13, prototype_stride_13, 0, 2048, false},
    PrototypeTensorProfile{"alignment.readout.1.weight", "14", TensorDType::float32, prototype_shape_14, prototype_stride_14, 0, 1048576, false},
    PrototypeTensorProfile{"alignment.pool_score.weight", "15", TensorDType::float32, prototype_shape_15, prototype_stride_15, 0, 2048, false},
    PrototypeTensorProfile{"cognition.role_embeddings", "16", TensorDType::float32, prototype_shape_16, prototype_stride_16, 0, 6144, false},
    PrototypeTensorProfile{"cognition.halt_evidence_scale", "17", TensorDType::float32, prototype_shape_17, prototype_stride_17, 0, 1, false},
    PrototypeTensorProfile{"cognition.cell.attention_norm.weight", "18", TensorDType::float32, prototype_shape_18, prototype_stride_18, 0, 2048, false},
    PrototypeTensorProfile{"cognition.cell.attention_norm.bias", "19", TensorDType::float32, prototype_shape_19, prototype_stride_19, 0, 2048, false},
    PrototypeTensorProfile{"cognition.cell.attention.in_proj_weight", "20", TensorDType::float32, prototype_shape_20, prototype_stride_20, 0, 12582912, false},
    PrototypeTensorProfile{"cognition.cell.attention.in_proj_bias", "21", TensorDType::float32, prototype_shape_21, prototype_stride_21, 0, 6144, false},
    PrototypeTensorProfile{"cognition.cell.attention.out_proj.weight", "22", TensorDType::float32, prototype_shape_22, prototype_stride_22, 0, 4194304, false},
    PrototypeTensorProfile{"cognition.cell.attention.out_proj.bias", "23", TensorDType::float32, prototype_shape_23, prototype_stride_23, 0, 2048, false},
    PrototypeTensorProfile{"cognition.cell.mlp_norm.weight", "24", TensorDType::float32, prototype_shape_24, prototype_stride_24, 0, 2048, false},
    PrototypeTensorProfile{"cognition.cell.mlp_norm.bias", "25", TensorDType::float32, prototype_shape_25, prototype_stride_25, 0, 2048, false},
    PrototypeTensorProfile{"cognition.cell.mlp_in.weight", "26", TensorDType::float32, prototype_shape_26, prototype_stride_26, 0, 67108864, false},
    PrototypeTensorProfile{"cognition.cell.mlp_out.weight", "27", TensorDType::float32, prototype_shape_27, prototype_stride_27, 0, 33554432, false},
    PrototypeTensorProfile{"cognition.cell.update_gate.weight", "28", TensorDType::float32, prototype_shape_28, prototype_stride_28, 0, 2048, false},
    PrototypeTensorProfile{"cognition.cell.update_gate.bias", "29", TensorDType::float32, prototype_shape_29, prototype_stride_29, 0, 1, false},
    PrototypeTensorProfile{"cognition.final_norm.weight", "30", TensorDType::float32, prototype_shape_30, prototype_stride_30, 0, 2048, false},
    PrototypeTensorProfile{"cognition.final_norm.bias", "31", TensorDType::float32, prototype_shape_31, prototype_stride_31, 0, 2048, false},
    PrototypeTensorProfile{"cognition.halt_head.weight", "32", TensorDType::float32, prototype_shape_32, prototype_stride_32, 0, 2048, false},
    PrototypeTensorProfile{"cognition.halt_head.bias", "33", TensorDType::float32, prototype_shape_33, prototype_stride_33, 0, 1, false},
    PrototypeTensorProfile{"evidence_state_head.weight", "34", TensorDType::float32, prototype_shape_34, prototype_stride_34, 0, 1048576, false},
    PrototypeTensorProfile{"evidence_state_head.bias", "35", TensorDType::float32, prototype_shape_35, prototype_stride_35, 0, 2048, false},
    PrototypeTensorProfile{"image_geometry_head.weight", "36", TensorDType::float32, prototype_shape_36, prototype_stride_36, 0, 8192, false},
    PrototypeTensorProfile{"image_geometry_head.bias", "37", TensorDType::float32, prototype_shape_37, prototype_stride_37, 0, 4, false},
}};
constexpr std::array<PrototypeMemberProfile, 44> prototype_members_631{{
    PrototypeMemberProfile{"data.pkl", 6047ULL, 894221361U, 64ULL},
    PrototypeMemberProfile{".format_version", 1ULL, 2212294583U, 6208ULL},
    PrototypeMemberProfile{".storage_alignment", 2ULL, 3916527423U, 6336ULL},
    PrototypeMemberProfile{"byteorder", 6ULL, 434322821U, 6464ULL},
    PrototypeMemberProfile{"data/0", 65536ULL, 3994871378U, 6592ULL},
    PrototypeMemberProfile{"data/1", 16777216ULL, 2891890708U, 72256ULL},
    PrototypeMemberProfile{"data/2", 8192ULL, 2939667634U, 16849600ULL},
    PrototypeMemberProfile{"data/3", 8192ULL, 811200457U, 16857920ULL},
    PrototypeMemberProfile{"data/4", 139264ULL, 2102708666U, 16866240ULL},
    PrototypeMemberProfile{"data/5", 16777216ULL, 700938363U, 17005632ULL},
    PrototypeMemberProfile{"data/6", 8192ULL, 3854767781U, 33782976ULL},
    PrototypeMemberProfile{"data/7", 8192ULL, 202278625U, 33791296ULL},
    PrototypeMemberProfile{"data/8", 90112ULL, 3131881755U, 33799616ULL},
    PrototypeMemberProfile{"data/9", 16777216ULL, 2146875238U, 33889856ULL},
    PrototypeMemberProfile{"data/10", 8192ULL, 3787105350U, 50667200ULL},
    PrototypeMemberProfile{"data/11", 8192ULL, 419683138U, 50675520ULL},
    PrototypeMemberProfile{"data/12", 8192ULL, 737661633U, 50683840ULL},
    PrototypeMemberProfile{"data/13", 8192ULL, 1779294509U, 50692160ULL},
    PrototypeMemberProfile{"data/14", 4194304ULL, 603091012U, 50700480ULL},
    PrototypeMemberProfile{"data/15", 8192ULL, 779481490U, 54894912ULL},
    PrototypeMemberProfile{"data/16", 24576ULL, 112205366U, 54903232ULL},
    PrototypeMemberProfile{"data/17", 4ULL, 2654645206U, 54927936ULL},
    PrototypeMemberProfile{"data/18", 8192ULL, 2203777485U, 54928064ULL},
    PrototypeMemberProfile{"data/19", 8192ULL, 2063903886U, 54936384ULL},
    PrototypeMemberProfile{"data/20", 50331648ULL, 1555190939U, 54944704ULL},
    PrototypeMemberProfile{"data/21", 24576ULL, 2581048401U, 105276480ULL},
    PrototypeMemberProfile{"data/22", 16777216ULL, 631779024U, 105301184ULL},
    PrototypeMemberProfile{"data/23", 8192ULL, 1643812361U, 122078528ULL},
    PrototypeMemberProfile{"data/24", 8192ULL, 843504922U, 122086848ULL},
    PrototypeMemberProfile{"data/25", 8192ULL, 1368854149U, 122095168ULL},
    PrototypeMemberProfile{"data/26", 268435456ULL, 1744036504U, 122103488ULL},
    PrototypeMemberProfile{"data/27", 134217728ULL, 769024659U, 390539072ULL},
    PrototypeMemberProfile{"data/28", 8192ULL, 4263085693U, 524756928ULL},
    PrototypeMemberProfile{"data/29", 4ULL, 793910785U, 524765248ULL},
    PrototypeMemberProfile{"data/30", 8192ULL, 1346728069U, 524765376ULL},
    PrototypeMemberProfile{"data/31", 8192ULL, 1179423586U, 524773696ULL},
    PrototypeMemberProfile{"data/32", 8192ULL, 2033766009U, 524782016ULL},
    PrototypeMemberProfile{"data/33", 4ULL, 452954566U, 524790336ULL},
    PrototypeMemberProfile{"data/34", 4194304ULL, 555576824U, 524790464ULL},
    PrototypeMemberProfile{"data/35", 8192ULL, 4237443011U, 528984896ULL},
    PrototypeMemberProfile{"data/36", 32768ULL, 4121708169U, 528993216ULL},
    PrototypeMemberProfile{"data/37", 16ULL, 2788702784U, 529026112ULL},
    PrototypeMemberProfile{"version", 2ULL, 1432854225U, 529026240ULL},
    PrototypeMemberProfile{".data/serialization_id", 40ULL, 3063894218U, 529026368ULL},
}};
constexpr std::array<PrototypeMemberProfile, 44> prototype_members_641{{
    PrototypeMemberProfile{"data.pkl", 6047ULL, 894221361U, 64ULL},
    PrototypeMemberProfile{".format_version", 1ULL, 2212294583U, 6208ULL},
    PrototypeMemberProfile{".storage_alignment", 2ULL, 3916527423U, 6336ULL},
    PrototypeMemberProfile{"byteorder", 6ULL, 434322821U, 6464ULL},
    PrototypeMemberProfile{"data/0", 65536ULL, 2288282616U, 6592ULL},
    PrototypeMemberProfile{"data/1", 16777216ULL, 1741703851U, 72256ULL},
    PrototypeMemberProfile{"data/2", 8192ULL, 2920880627U, 16849600ULL},
    PrototypeMemberProfile{"data/3", 8192ULL, 1407705558U, 16857920ULL},
    PrototypeMemberProfile{"data/4", 139264ULL, 3610908292U, 16866240ULL},
    PrototypeMemberProfile{"data/5", 16777216ULL, 194847261U, 17005632ULL},
    PrototypeMemberProfile{"data/6", 8192ULL, 3157256416U, 33782976ULL},
    PrototypeMemberProfile{"data/7", 8192ULL, 193090951U, 33791296ULL},
    PrototypeMemberProfile{"data/8", 90112ULL, 3080241452U, 33799616ULL},
    PrototypeMemberProfile{"data/9", 16777216ULL, 1814114246U, 33889856ULL},
    PrototypeMemberProfile{"data/10", 8192ULL, 3005215948U, 50667200ULL},
    PrototypeMemberProfile{"data/11", 8192ULL, 37476527U, 50675520ULL},
    PrototypeMemberProfile{"data/12", 8192ULL, 968661335U, 50683840ULL},
    PrototypeMemberProfile{"data/13", 8192ULL, 1839196993U, 50692160ULL},
    PrototypeMemberProfile{"data/14", 4194304ULL, 3584041250U, 50700480ULL},
    PrototypeMemberProfile{"data/15", 8192ULL, 1963902968U, 54894912ULL},
    PrototypeMemberProfile{"data/16", 24576ULL, 48402066U, 54903232ULL},
    PrototypeMemberProfile{"data/17", 4ULL, 3390984826U, 54927936ULL},
    PrototypeMemberProfile{"data/18", 8192ULL, 2296422175U, 54928064ULL},
    PrototypeMemberProfile{"data/19", 8192ULL, 1627108007U, 54936384ULL},
    PrototypeMemberProfile{"data/20", 50331648ULL, 1210178700U, 54944704ULL},
    PrototypeMemberProfile{"data/21", 24576ULL, 2655944520U, 105276480ULL},
    PrototypeMemberProfile{"data/22", 16777216ULL, 2554441087U, 105301184ULL},
    PrototypeMemberProfile{"data/23", 8192ULL, 2906984889U, 122078528ULL},
    PrototypeMemberProfile{"data/24", 8192ULL, 148074713U, 122086848ULL},
    PrototypeMemberProfile{"data/25", 8192ULL, 2429467179U, 122095168ULL},
    PrototypeMemberProfile{"data/26", 268435456ULL, 1078655051U, 122103488ULL},
    PrototypeMemberProfile{"data/27", 134217728ULL, 3197828738U, 390539072ULL},
    PrototypeMemberProfile{"data/28", 8192ULL, 3987471334U, 524756928ULL},
    PrototypeMemberProfile{"data/29", 4ULL, 1887277026U, 524765248ULL},
    PrototypeMemberProfile{"data/30", 8192ULL, 3214445854U, 524765376ULL},
    PrototypeMemberProfile{"data/31", 8192ULL, 3512170146U, 524773696ULL},
    PrototypeMemberProfile{"data/32", 8192ULL, 3785405399U, 524782016ULL},
    PrototypeMemberProfile{"data/33", 4ULL, 3006366523U, 524790336ULL},
    PrototypeMemberProfile{"data/34", 4194304ULL, 1845206506U, 524790464ULL},
    PrototypeMemberProfile{"data/35", 8192ULL, 2916238765U, 528984896ULL},
    PrototypeMemberProfile{"data/36", 32768ULL, 2538014470U, 528993216ULL},
    PrototypeMemberProfile{"data/37", 16ULL, 1063606834U, 529026112ULL},
    PrototypeMemberProfile{"version", 2ULL, 1432854225U, 529026240ULL},
    PrototypeMemberProfile{".data/serialization_id", 40ULL, 3993600940U, 529026368ULL},
}};

constexpr std::string_view prototype_source_config_path =
    "/var/tmp/tinylm-original-audit-3bddcb7a/configs/"
    "rozephine_cognitive_kernel_prototype1_n0.json";
constexpr std::string_view prototype_source_config_sha256 =
    "543784817730b6e79261621d8b7b4ce3d54eff23a9027788d5104e05cdd7eea5";
constexpr std::string_view prototype_runtime_config_json =
    "{\"attention_heads\":16,\"evidence_logit_epsilon\":0.0001,"
    "\"executive_slots\":8,\"halt_threshold\":0.5,\"hidden_dim\":2048,"
    "\"maximum_cycles\":4,\"maximum_update\":1.0,\"minimum_cycles\":1,"
    "\"mlp_hidden_dim\":16384,\"scratch_slots\":8,\"semantic_slots\":32}";
constexpr std::string_view prototype_runtime_config_sha256 =
    "d5d58abcc1e13c31d501231b22e8ac19f956d6d1f98594e9ced0e8dd342bf301";
constexpr PrototypeRuntimeCognitionConfig prototype_runtime_config{
    32, 8, 8, 2048, 16, 16384, 1, 4, 0.5, 1e-4, 1.0,
};

constexpr std::array<PrototypeCheckpointProfile, 2> prototype_profiles{{
    PrototypeCheckpointProfile{
        "prototype0.pt",
        "/var/home/raspie/Documents/Codex/tinylm slicer/outputs/rozephine/cognitive_kernel_k5a/k5a_132m_completeness300_b8_4070_seed631/prototype0.pt",
        "2d067dd9c46a01d0bce16d2c123f0a306956c970a0eff6d7bbd607a34df76e74",
        529029365ULL,
        "5df57ada8f9e710c4bf3b00f50c0b6ddd7586b566d9ea33d53f9ed9311d7d3a2",
        6047ULL,
        1499,
        "0690472957255330750100897481345979877657",
        "4a67cf5320921a1a247d96d19a79efff4835e6448708403ebe78a2af9e95bd3c",
        prototype_source_config_path,
        prototype_source_config_sha256,
        prototype_runtime_config_sha256,
        prototype_runtime_config,
        prototype_tensors,
        prototype_members_631,
    },
    PrototypeCheckpointProfile{
        "prototype0.pt",
        "/var/home/raspie/Documents/Codex/tinylm slicer/outputs/rozephine/cognitive_kernel_k5a/k5a_132m_completeness300_b8_5060_seed641/prototype0.pt",
        "ef8418b90b1fbcd206fa954be60833f505e94f353efee7222147afa268873c2f",
        529029365ULL,
        "5df57ada8f9e710c4bf3b00f50c0b6ddd7586b566d9ea33d53f9ed9311d7d3a2",
        6047ULL,
        1499,
        "0690472957255330750101171389017499513010",
        "4a67cf5320921a1a247d96d19a79efff4835e6448708403ebe78a2af9e95bd3c",
        prototype_source_config_path,
        prototype_source_config_sha256,
        prototype_runtime_config_sha256,
        prototype_runtime_config,
        prototype_tensors,
        prototype_members_641,
    },
}};

[[noreturn]] void prototype_invalid(const char* message) { throw std::runtime_error(message); }

std::string digest_hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

std::string archive_digest(const RestrictedZip& archive) {
    constexpr std::size_t chunk_bytes = 4U << 20;
    std::vector<std::byte> buffer(static_cast<std::size_t>(
        std::min<std::uint64_t>(chunk_bytes, archive.archive_bytes())));
    architecture::Sha256 digest;
    std::uint64_t offset = 0;
    while (offset != archive.archive_bytes()) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), archive.archive_bytes() - offset));
        auto view = std::span<std::byte>(buffer).first(count);
        archive.read_archive(offset, view);
        digest.update(view);
        offset += count;
    }
    return digest_hex(digest.finish());
}

std::string byte_text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string_view prototype_dtype_name(const TensorDType type) {
    switch (type) {
    case TensorDType::float16: return "float16";
    case TensorDType::float32: return "float32";
    case TensorDType::bfloat16: return "bfloat16";
    case TensorDType::int64: return "int64";
    }
    prototype_invalid("Prototype0 tensor dtype is unsupported");
}

TensorDType prototype_dtype(const StorageType type) {
    switch (type) {
    case StorageType::half: return TensorDType::float16;
    case StorageType::float32: return TensorDType::float32;
    case StorageType::bfloat16: return TensorDType::bfloat16;
    case StorageType::int64: return TensorDType::int64;
    }
    prototype_invalid("Prototype0 storage dtype is unsupported");
}

std::string prototype_manifest_line(const CheckpointTensor& tensor) {
    // Preserve the existing audited tensor_manifest_line contract. Prototype0
    // uses its bare key as both state_path and state_dict_key and every entry is
    // model state; changing this serialization would create a second ledger.
    std::string line = tensor.state_path + '\t' +
        (tensor.model_state ? "1\t" : "0\t") +
        std::string(prototype_dtype_name(tensor.dtype)) + '\t' + tensor.storage_key + '\t' +
        std::to_string(tensor.storage_elements) + '\t' +
        std::to_string(tensor.storage_offset_elements) + '\t';
    for (std::size_t index = 0; index != tensor.shape.size(); ++index) {
        if (index != 0) line.push_back(',');
        line += std::to_string(tensor.shape[index]);
    }
    line.push_back('\t');
    for (std::size_t index = 0; index != tensor.stride.size(); ++index) {
        if (index != 0) line.push_back(',');
        line += std::to_string(tensor.stride[index]);
    }
    line += tensor.requires_grad ? "\t1\n" : "\t0\n";
    return line;
}

void validate_members(const RestrictedZip& archive, const std::string& root,
                      const PrototypeCheckpointProfile& profile) {
    if (archive.members().size() != profile.members.size())
        prototype_invalid("Prototype0 ZIP member count mismatch");
    for (std::size_t index = 0; index != profile.members.size(); ++index) {
        const auto& actual = archive.members()[index];
        const auto& expected = profile.members[index];
        if (actual.name != root + '/' + std::string(expected.suffix) ||
            actual.size != expected.size || actual.crc32 != expected.crc32 ||
            actual.payload_offset != expected.payload_offset || actual.flags != 2056 ||
            actual.method != 0)
            prototype_invalid("Prototype0 ZIP member manifest mismatch");
    }
}

void validate_metadata(const Symbolic& metadata) {
    if (!metadata || metadata->kind != SymbolicKind::ordered_dictionary ||
        metadata->entries.size() != 38)
        prototype_invalid("Prototype0 OrderedDict metadata mismatch");
    std::unordered_set<std::string> names;
    for (const auto& [key, value] : metadata->entries) {
        if (!key || key->kind != SymbolicKind::string || !names.insert(key->text).second ||
            !value || value->kind != SymbolicKind::dictionary || value->entries.size() != 1)
            prototype_invalid("Prototype0 module metadata entry mismatch");
        const auto& [version_key, version_value] = value->entries.front();
        if (!version_key || version_key->kind != SymbolicKind::string ||
            version_key->text != "version" || !version_value ||
            version_value->kind != SymbolicKind::integer || version_value->integer != 1)
            prototype_invalid("Prototype0 module metadata version mismatch");
    }
}

CheckpointTensor prototype_tensor(const TensorRecord& record,
                                  const PrototypeTensorProfile& expected,
                                  const std::string& root,
                                  const RestrictedZip& archive) {
    CheckpointTensor tensor;
    tensor.state_path = std::string(expected.key);
    tensor.state_dict_key = std::string(expected.key);
    tensor.storage_key = record.storage.key;
    tensor.storage_member = root + "/data/" + record.storage.key;
    tensor.dtype = prototype_dtype(record.storage.type);
    tensor.shape = record.shape;
    tensor.stride.reserve(record.stride.size());
    for (const auto value : record.stride) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            prototype_invalid("Prototype0 tensor stride exceeds signed range");
        tensor.stride.push_back(static_cast<std::int64_t>(value));
    }
    tensor.storage_offset_elements = record.storage_offset;
    tensor.storage_elements = record.storage.elements;
    tensor.requires_grad = record.requires_grad;
    tensor.model_state = true;
    if (record.storage.device != "cuda:0")
        prototype_invalid("Prototype0 storage device mismatch");
    const TensorLayout layout{tensor.dtype, tensor.shape, tensor.stride,
                              tensor.storage_offset_elements, tensor.storage_elements};
    const auto checked = check_tensor_span(layout);
    if (!checked) prototype_invalid("Prototype0 tensor span is invalid");
    tensor.numel = checked.numel;
    tensor.logical_bytes = checked.logical_bytes;
    tensor.storage_span_start_byte = checked.storage_span_start_byte;
    tensor.storage_span_end_byte_exclusive = checked.storage_span_end_byte_exclusive;
    tensor.contiguous = checked.contiguous;
    if (tensor.state_dict_key != expected.key || tensor.storage_key != expected.storage_key ||
        tensor.dtype != expected.dtype || tensor.shape.size() != expected.shape.size() ||
        !std::equal(tensor.shape.begin(), tensor.shape.end(), expected.shape.begin()) ||
        tensor.stride.size() != expected.stride.size() ||
        !std::equal(tensor.stride.begin(), tensor.stride.end(), expected.stride.begin()) ||
        tensor.storage_offset_elements != expected.storage_offset_elements ||
        tensor.storage_elements != expected.storage_elements ||
        tensor.requires_grad != expected.requires_grad)
        prototype_invalid("Prototype0 tensor profile mismatch");
    const auto* member = archive.find(tensor.storage_member);
    const auto item_bytes = dtype_size(tensor.dtype);
    if (member == nullptr || item_bytes == 0 ||
        tensor.storage_elements > std::numeric_limits<std::uint64_t>::max() / item_bytes ||
        member->size != tensor.storage_elements * item_bytes ||
        member->payload_offset % 64 != 0 || member->method != 0)
        prototype_invalid("Prototype0 tensor storage member mismatch");
    return tensor;
}

}  // namespace

std::span<const PrototypeCheckpointProfile> prototype_checkpoint_profiles() noexcept {
    return prototype_profiles;
}

const PrototypeCheckpointProfile* find_prototype_checkpoint_profile(
    const std::string_view checkpoint_sha256) noexcept {
    const auto found = std::find_if(prototype_profiles.begin(), prototype_profiles.end(),
        [checkpoint_sha256](const auto& profile) {
            return profile.checkpoint_sha256 == checkpoint_sha256;
        });
    return found == prototype_profiles.end() ? nullptr : &*found;
}

static bool validate_prototype_checkpoint_profile(
    const PrototypeCheckpointProfile& profile) {
    const auto& config = profile.runtime_cognition_config;
    const auto runtime_config_digest = digest_hex(architecture::Sha256::of(
        std::as_bytes(std::span(prototype_runtime_config_json))));
    if (profile.source_config_path != prototype_source_config_path ||
        profile.source_config_sha256 != prototype_source_config_sha256 ||
        profile.runtime_cognition_config_canonical_sha256 !=
            prototype_runtime_config_sha256 ||
        runtime_config_digest != profile.runtime_cognition_config_canonical_sha256 ||
        config.semantic_slots != 32 || config.executive_slots != 8 ||
        config.scratch_slots != 8 || config.hidden_dim != 2048 ||
        config.attention_heads != 16 || config.mlp_hidden_dim != 16384 ||
        config.minimum_cycles != 1 || config.maximum_cycles != 4 ||
        config.halt_threshold != 0.5 || config.evidence_logit_epsilon != 1e-4 ||
        config.maximum_update != 1.0 || config.hidden_dim % config.attention_heads != 0 ||
        profile.tensors.size() != prototype_tensors.size() ||
        profile.members.size() != 44)
        return false;

    const auto tensor = [&profile](const std::string_view key)
        -> const PrototypeTensorProfile* {
        const auto found = std::find_if(profile.tensors.begin(), profile.tensors.end(),
            [key](const auto& candidate) { return candidate.key == key; });
        return found == profile.tensors.end() ? nullptr : &*found;
    };
    const auto* roles = tensor("cognition.role_embeddings");
    const auto* attention = tensor("cognition.cell.attention.in_proj_weight");
    const auto* mlp_in = tensor("cognition.cell.mlp_in.weight");
    const auto* mlp_out = tensor("cognition.cell.mlp_out.weight");
    return roles != nullptr && roles->shape.size() == 2 && roles->shape[0] == 3 &&
        roles->shape[1] == config.hidden_dim && attention != nullptr &&
        attention->shape.size() == 2 && attention->shape[0] == 3 * config.hidden_dim &&
        attention->shape[1] == config.hidden_dim && mlp_in != nullptr &&
        mlp_in->shape.size() == 2 && mlp_in->shape[0] == 2 * config.mlp_hidden_dim &&
        mlp_in->shape[1] == config.hidden_dim && mlp_out != nullptr &&
        mlp_out->shape.size() == 2 && mlp_out->shape[0] == config.hidden_dim &&
        mlp_out->shape[1] == config.mlp_hidden_dim;
}

const CheckpointTensor* PrototypeCheckpointManifest::find_tensor(
    const std::string_view key) const noexcept {
    const auto found = std::find_if(tensors.begin(), tensors.end(), [key](const auto& tensor) {
        return tensor.state_dict_key == key;
    });
    return found == tensors.end() ? nullptr : &*found;
}

PrototypeCheckpoint PrototypeCheckpoint::open(const std::filesystem::path& path) {
    auto archive = RestrictedZip::open(path);
    const auto checkpoint_sha = archive_digest(archive);
    const auto* profile = find_prototype_checkpoint_profile(checkpoint_sha);
    if (profile == nullptr || !validate_prototype_checkpoint_profile(*profile) ||
        archive.archive_bytes() != profile->checkpoint_bytes)
        prototype_invalid("Prototype0 checkpoint identity is not audited");

    std::string root;
    for (const auto& member : archive.members()) {
        if (!member.name.ends_with("/data.pkl")) continue;
        if (!root.empty()) prototype_invalid("Prototype0 has multiple pickle roots");
        root = member.name.substr(0, member.name.size() - std::string_view("/data.pkl").size());
    }
    if (root != "prototype0") prototype_invalid("Prototype0 archive root mismatch");
    validate_members(archive, root, *profile);

    const auto member_text = [&](const std::string_view suffix) {
        return byte_text(archive.read(root + std::string(suffix)));
    };
    if (member_text("/.format_version") != "1" ||
        member_text("/.storage_alignment") != "64" ||
        member_text("/byteorder") != "little" || member_text("/version") != "3\n" ||
        member_text("/.data/serialization_id") != profile->serialization_id)
        prototype_invalid("Prototype0 serialization metadata mismatch");

    const auto pickle = archive.read(root + "/data.pkl");
    const auto pickle_sha = digest_hex(architecture::Sha256::of(pickle));
    if (pickle.size() != profile->data_pickle_bytes || pickle_sha != profile->data_pickle_sha256)
        prototype_invalid("Prototype0 pickle identity mismatch");
    const auto parsed = parse_prototype_pickle(pickle, {});
    if (parsed.opcode_count != profile->pickle_opcode_count || !parsed.root ||
        parsed.root->kind != SymbolicKind::ordered_dictionary ||
        parsed.root->entries.size() != profile->tensors.size() + 1)
        prototype_invalid("Prototype0 state_dict root mismatch");

    PrototypeCheckpointManifest manifest;
    manifest.profile = profile;
    manifest.archive_root = root;
    manifest.checkpoint_sha256 = checkpoint_sha;
    manifest.data_pickle_sha256 = pickle_sha;
    manifest.serialization_id = std::string(profile->serialization_id);
    manifest.pickle_opcode_count = parsed.opcode_count;
    manifest.tensors.reserve(profile->tensors.size());
    std::unordered_set<std::string> keys;
    std::unordered_set<std::string> storage_keys;
    architecture::Sha256 manifest_digest;
    for (std::size_t index = 0; index != profile->tensors.size(); ++index) {
        const auto& [key, value] = parsed.root->entries[index];
        const auto& expected = profile->tensors[index];
        if (!key || key->kind != SymbolicKind::string || key->text != expected.key ||
            !keys.insert(key->text).second || !value || value->kind != SymbolicKind::tensor)
            prototype_invalid("Prototype0 state_dict key/order mismatch");
        auto tensor = prototype_tensor(value->tensor, expected, root, archive);
        if (!storage_keys.insert(tensor.storage_key).second)
            prototype_invalid("Prototype0 storage key is reused");
        manifest_digest.update(prototype_manifest_line(tensor));
        manifest.tensors.push_back(std::move(tensor));
    }
    const auto& [metadata_key, metadata] = parsed.root->entries.back();
    if (!metadata_key || metadata_key->kind != SymbolicKind::string ||
        metadata_key->text != "_metadata")
        prototype_invalid("Prototype0 metadata key/order mismatch");
    validate_metadata(metadata);
    if (storage_keys.size() != profile->tensors.size())
        prototype_invalid("Prototype0 storage record count mismatch");
    manifest.tensor_manifest_sha256 = digest_hex(manifest_digest.finish());
    if (manifest.tensor_manifest_sha256 != profile->tensor_manifest_sha256)
        prototype_invalid("Prototype0 tensor manifest identity mismatch");

    archive.verify_all();
    return PrototypeCheckpoint(std::move(archive), std::move(manifest));
}

std::vector<std::byte> PrototypeCheckpoint::read_storage(const std::string_view storage_key) const {
    const auto* tensor = manifest_.find_tensor(storage_key);
    if (tensor != nullptr && tensor->storage_key != storage_key) tensor = nullptr;
    if (tensor == nullptr) {
        const auto found = std::find_if(manifest_.tensors.begin(), manifest_.tensors.end(),
            [storage_key](const auto& candidate) { return candidate.storage_key == storage_key; });
        if (found == manifest_.tensors.end())
            prototype_invalid("Prototype0 storage key is not in the manifest");
        tensor = &*found;
    }
    return archive_.read(tensor->storage_member);
}

}  // namespace swegca::checkpoint
