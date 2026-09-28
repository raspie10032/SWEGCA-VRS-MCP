#include "checkpoint/restricted_pickle.hpp"

#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace swegca::checkpoint;

namespace {

void byte(std::vector<std::byte>& out, unsigned value) { out.push_back(std::byte(value)); }
void opcode(std::vector<std::byte>& out, char value) { byte(out, static_cast<unsigned char>(value)); }
void u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) byte(out, value >> shift);
}
void text(std::vector<std::byte>& out, std::string_view value) {
    opcode(out, 'X'); u32(out, static_cast<std::uint32_t>(value.size()));
    for (const auto ch : value) byte(out, static_cast<unsigned char>(ch));
}
void global(std::vector<std::byte>& out, std::string_view module, std::string_view name) {
    opcode(out, 'c');
    for (auto value : {module, name}) {
        for (const auto ch : value) byte(out, static_cast<unsigned char>(ch));
        opcode(out, '\n');
    }
}
void memo(std::vector<std::byte>& out, unsigned index) { opcode(out, 'q'); byte(out, index); }
void get(std::vector<std::byte>& out, unsigned index) { opcode(out, 'h'); byte(out, index); }

std::vector<std::byte> tensor_pickle() {
    std::vector<std::byte> p;
    byte(p, 0x80); byte(p, 2);              // PROTO 2
    opcode(p, '}');                         // root dict
    opcode(p, '(');                         // MARK for SETITEMS
    text(p, "weight");
    global(p, "torch._utils", "_rebuild_tensor_v2"); memo(p, 0);
    opcode(p, '(');                         // tensor args
    opcode(p, '('); text(p, "storage");
    global(p, "torch", "FloatStorage"); memo(p, 1);
    text(p, "7"); text(p, "cpu"); opcode(p, 'J'); u32(p, 6);
    opcode(p, 't'); opcode(p, 'Q');         // storage PID
    opcode(p, 'K'); byte(p, 0);             // offset
    opcode(p, 'K'); byte(p, 2); opcode(p, 'K'); byte(p, 3); byte(p, 0x86); // shape
    opcode(p, 'K'); byte(p, 3); opcode(p, 'K'); byte(p, 1); byte(p, 0x86); // stride
    byte(p, 0x89);                          // requires_grad false
    global(p, "collections", "OrderedDict"); memo(p, 2); opcode(p, ')'); opcode(p, 'R');
    opcode(p, 't'); opcode(p, 'R');         // inert tensor rebuild
    opcode(p, 'u'); opcode(p, '.');
    return p;
}

bool rejected(std::vector<std::byte> pickle, const RestrictedPickleLimits& limits = {}) {
    try { (void)parse_restricted_pickle(pickle, limits); }
    catch (const RestrictedPickleError&) { return true; }
    return false;
}

}  // namespace

int main() {
    const auto positive = tensor_pickle();
    const auto parsed = parse_restricted_pickle(positive);
    assert(parsed.bytes_consumed == positive.size());
    assert(parsed.root->kind == SymbolicKind::dictionary && parsed.root->entries.size() == 1);
    assert(parsed.root->entries[0].first->text == "weight");
    const auto& tensor = parsed.root->entries[0].second;
    assert(tensor->kind == SymbolicKind::tensor);
    assert(tensor->tensor.storage.type == StorageType::float32);
    assert(tensor->tensor.storage.key == "7" && tensor->tensor.storage.device == "cpu");
    assert(tensor->tensor.storage.elements == 6 && tensor->tensor.storage_offset == 0);
    assert((tensor->tensor.shape == std::vector<std::uint64_t>{2, 3}));
    assert((tensor->tensor.stride == std::vector<std::uint64_t>{3, 1}));
    assert(!tensor->tensor.requires_grad);

    // Memo aliasing must preserve container mutations.
    std::vector<std::byte> alias{std::byte{0x80}, std::byte{2}};
    opcode(alias, ']'); memo(alias, 0); opcode(alias, 'K'); byte(alias, 9); opcode(alias, 'a');
    get(alias, 0); byte(alias, 0x86); opcode(alias, '.');
    const auto alias_result = parse_restricted_pickle(alias);
    assert(alias_result.root->sequence[0].get() == alias_result.root->sequence[1].get());
    assert(alias_result.root->sequence[0]->sequence[0]->integer == 9);

    auto unknown = positive; unknown[2] = std::byte{'0'}; assert(rejected(unknown));
    auto trailing = positive; trailing.push_back(std::byte{0}); assert(rejected(trailing));
    auto wrong_protocol = positive; wrong_protocol[1] = std::byte{4}; assert(rejected(wrong_protocol));

    std::vector<std::byte> evil{std::byte{0x80}, std::byte{2}};
    global(evil, "builtins", "eval"); opcode(evil, '.'); assert(rejected(evil));

    auto missing_memo = std::vector<std::byte>{std::byte{0x80}, std::byte{2}, std::byte{'h'},
                                               std::byte{9}, std::byte{'.'}};
    assert(rejected(missing_memo));

    // The PID tag, device and element count are type checked.
    auto wrong_pid = positive;
    const std::string needle = "storage";
    for (std::size_t i = 0; i + needle.size() <= wrong_pid.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
            match &= std::to_integer<char>(wrong_pid[i + j]) == needle[j];
        if (match) { wrong_pid[i] = std::byte{'x'}; break; }
    }
    assert(rejected(wrong_pid));

    // A valid reducer cannot be applied with a mismatched argument tuple.
    std::vector<std::byte> bad_reduce{std::byte{0x80}, std::byte{2}};
    global(bad_reduce, "collections", "OrderedDict"); opcode(bad_reduce, 'K'); byte(bad_reduce, 1);
    byte(bad_reduce, 0x85); opcode(bad_reduce, 'R'); opcode(bad_reduce, '.');
    assert(rejected(bad_reduce));

    // Shape [2,4] with contiguous stride needs eight elements, while storage has six.
    auto out_of_span = positive;
    for (std::size_t i = 0; i + 3 < out_of_span.size(); ++i) {
        if (out_of_span[i] == std::byte{'K'} && out_of_span[i + 1] == std::byte{2} &&
            out_of_span[i + 2] == std::byte{'K'} && out_of_span[i + 3] == std::byte{3}) {
            out_of_span[i + 3] = std::byte{4}; break;
        }
    }
    assert(rejected(out_of_span));

    RestrictedPickleLimits tiny;
    tiny.maximum_opcodes = 3;
    assert(rejected(positive, tiny));
    tiny = {};
    tiny.maximum_string_bytes = 3;
    assert(rejected(positive, tiny));

    std::cout << "restricted pickle symbolic VM tests passed\n";
}
