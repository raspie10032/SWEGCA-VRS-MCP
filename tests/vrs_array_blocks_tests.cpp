#include "world/vrs_array_blocks.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

using namespace swegca::world;

namespace {

std::vector<std::byte> bytes(const auto& values) {
    const auto view = std::as_bytes(std::span(values));
    return {view.begin(), view.end()};
}

NumericArrayView view(const std::string_view dtype,
                      const std::vector<std::size_t>& shape,
                      const std::vector<std::byte>& data,
                      const bool contiguous = true) {
    return {dtype, shape, data, contiguous};
}

bool rejects(const auto& operation) {
    try { operation(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

bool corrupts(const auto& operation) {
    try { operation(); }
    catch (const CorruptVrsBlock&) { return true; }
    return false;
}

struct TemporaryDirectory final {
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "swegca-array-blocks-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* result = ::mkdtemp(writable.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

void exact_python_fixture() {
    std::vector<float> source(65);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(i);
    const std::vector<std::size_t> source_shape{65};
    const auto source_bytes = bytes(source);
    const auto prepared = VrsArrayBlocks::prepare(
        view("<f4", source_shape, source_bytes), {}, 64);
    assert(prepared.array->dtype == "<f4" && prepared.array->shape == source_shape);
    assert(prepared.receipt.candidate_bytes_scanned == 260);
    assert(prepared.receipt.candidate_bytes_compared == 0);
    assert(prepared.receipt.raw_bytes_encoded == 260);
    assert(!prepared.receipt.numerical_incrementality_claimed);
    const std::array<std::string_view, 5> source_digests{
        "4c355e80fb61e8d0e5cd8af13f0f045db6c9129b7af31d44d59a390d7363dda9",
        "9bff7e0bd9d0d0d9aa45d3d91b62d1ad0fff2a4fc7be25bb287f29462a771b95",
        "e5d6cae4cec8f59f9460d743ad7222c5c01c649f5702bc689131af2c81070698",
        "89faed198a4d69ffe91d966f2f36dc5febb004887f9205792343c812fc742d52",
        "b2c138dbe3b67cf202e723065cafc51b43d888f85b9f47aa5cc42b36e491b5e4",
    };
    for (std::size_t i = 0; i < source_digests.size(); ++i) {
        if (prepared.array->data->blocks[i]->digest != source_digests[i])
            std::cerr << "source digest mismatch " << i << " actual="
                      << prepared.array->data->blocks[i]->digest << " expected="
                      << source_digests[i] << '\n';
        assert(prepared.array->data->blocks[i]->digest == source_digests[i]);
    }

    const std::vector<float> values{10.0F, 0.0F, 12.0F};
    std::vector<float> tail(15);
    for (std::size_t i = 0; i < tail.size(); ++i) tail[i] = static_cast<float>(i);
    const std::vector<std::size_t> values_shape{3}, tail_shape{15};
    const auto values_bytes = bytes(values), tail_bytes = bytes(tail);
    const auto values_view = view("<f4", values_shape, values_bytes);
    const auto tail_view = view("<f4", tail_shape, tail_bytes);
    const std::array<std::size_t, 3> offsets{0, 31, 64};
    const auto patched = VrsArrayBlocks::patch_and_append(
        prepared.array, offsets, &values_view, &tail_view);
    assert((patched.array->shape == std::vector<std::size_t>{80}));
    assert(patched.receipt.candidate_bytes_scanned == 72);
    assert(patched.receipt.old_payload_bytes_decoded == 132);
    assert(patched.receipt.raw_bytes_encoded == 192);
    assert(patched.receipt.sparse_storage_update);
    const std::array<std::string_view, 5> updated_digests{
        "5062731c215f524a4e92e8b08631448261df3f43d9a1159d071485f1fe1f85e1",
        "58d4d96c5dc89ab7f6c077869dd6106118ce4d57f89669972feeedb52ba77d9b",
        "e5d6cae4cec8f59f9460d743ad7222c5c01c649f5702bc689131af2c81070698",
        "89faed198a4d69ffe91d966f2f36dc5febb004887f9205792343c812fc742d52",
        "c6123010e00b9d8d94122f5a76807cc5ba5afe5f860d5407a78538c81e2d317d",
    };
    for (std::size_t i = 0; i < updated_digests.size(); ++i)
        assert(patched.array->data->blocks[i]->digest == updated_digests[i]);
    assert(patched.array->data->blocks[2] == prepared.array->data->blocks[2]);
    assert(patched.array->data->blocks[3] == prepared.array->data->blocks[3]);

    auto expected = source;
    expected[0] = 10.0F;
    expected[31] = 0.0F;
    expected[64] = 12.0F;
    expected.insert(expected.end(), tail.begin(), tail.end());
    assert(patched.array->restore() == bytes(expected));
    assert(prepared.array->restore() == source_bytes);

    TemporaryDirectory temporary;
    VrsGenerationBlockStore store(temporary.path);
    VrsArrayBundle bundle({{"score", patched.array}});
    const auto saved = bundle.save(store);
    assert(saved.bundle_sha256 ==
           "22ee9de6efbe971637ad849e71466865719453334ff1567c5781ef00b14bac01");
    assert(saved.bundle_bytes == 181 && !saved.main_committed);
    const auto& array_receipt = saved.arrays.at("score");
    assert(array_receipt.manifest_sha256 ==
           "ccf4dc342636c38cb89642e688986f4eb51682bdfbe254a11d0cd9d363932339");
    assert(array_receipt.block_publish_count == 5);
    assert(array_receipt.block_publish_bytes == 518);
    assert(array_receipt.manifest_bytes == 418);
    assert(!array_receipt.main_committed && !array_receipt.current_pointer_written);
    const auto restored = VrsArrayBundle::load(
        store, saved.bundle_sha256, 1024 * 1024);
    assert(restored.arrays.at("score")->restore() == bytes(expected));

    // A new store instance verifies pre-existing immutable addresses instead
    // of treating the filesystem cache as a mutable current pointer.
    VrsGenerationBlockStore reopened(temporary.path);
    const auto cold = VrsArrayBundle::load(
        reopened, saved.bundle_sha256, 1024 * 1024);
    assert(cold.arrays.at("score")->restore() == bytes(expected));
}

void sparse_geometry_and_bits() {
    for (const auto& [dtype, item] : std::vector<std::pair<std::string, std::size_t>>{
             {"<f2", 2}, {"<f4", 4}, {">f8", 8}, {"<u8", 8},
             {">i4", 4}, {"|b1", 1}, {"<c16", 16}}) {
        for (const auto& shape : {std::vector<std::size_t>{65},
                                  std::vector<std::size_t>{13, 5}}) {
            std::vector<std::byte> raw(65 * item);
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::byte>((i * 37 + 11) & 0xff);
            auto prepared = VrsArrayBlocks::prepare(view(dtype, shape, raw), {}, 64);
            std::vector<std::byte> replacements(3 * item);
            for (std::size_t i = 0; i < replacements.size(); ++i)
                replacements[i] = static_cast<std::byte>((i * 13 + 7) & 0xff);
            const std::vector<std::size_t> value_shape{3};
            const auto value_view = view(dtype, value_shape, replacements);
            const std::vector<std::size_t> append_shape = shape.size() == 1 ?
                std::vector<std::size_t>{15} : std::vector<std::size_t>{3, 5};
            std::vector<std::byte> tail(15 * item);
            for (std::size_t i = 0; i < tail.size(); ++i)
                tail[i] = static_cast<std::byte>((i * 19 + 3) & 0xff);
            const auto append_view = view(dtype, append_shape, tail);
            const std::array<std::size_t, 3> offsets{0, 31, 64};
            const auto changed = VrsArrayBlocks::patch_and_append(
                prepared.array, offsets, &value_view, &append_view);
            auto expected = raw;
            for (std::size_t n = 0; n < offsets.size(); ++n)
                std::copy_n(replacements.begin() + static_cast<std::ptrdiff_t>(n * item), item,
                            expected.begin() + static_cast<std::ptrdiff_t>(offsets[n] * item));
            expected.insert(expected.end(), tail.begin(), tail.end());
            assert(changed.array->restore() == expected);
            assert(prepared.array->restore() == raw);
        }
    }

    const std::array<std::uint32_t, 3> bit_values{0x80000000U, 0x7fc00001U, 0x7fc12345U};
    const std::vector<std::uint32_t> bits(bit_values.begin(), bit_values.end());
    const auto raw = bytes(bits);
    const std::vector<std::size_t> shape{3};
    const auto prepared = VrsArrayBlocks::prepare(view("<f4", shape, raw), {}, 8);
    const auto no_op = VrsArrayBlocks::patch_and_append(prepared.array);
    assert(no_op.array == prepared.array && no_op.receipt.candidate_bytes_scanned == 0);
    const std::vector<std::uint32_t> same_bits{0x80000000U};
    const auto same_raw = bytes(same_bits);
    const std::vector<std::size_t> one_shape{1};
    const auto same_view = view("<f4", one_shape, same_raw);
    const std::array<std::size_t, 1> first{0};
    const auto same = VrsArrayBlocks::patch_and_append(
        prepared.array, first, &same_view);
    for (std::size_t i = 0; i < prepared.array->data->blocks.size(); ++i)
        assert(same.array->data->blocks[i] == prepared.array->data->blocks[i]);

    const std::vector<std::size_t> empty_shape{2, 0};
    const std::vector<std::byte> empty;
    const auto zero_width = VrsArrayBlocks::prepare(view("<f4", empty_shape, empty));
    const std::vector<std::size_t> grown_shape{5, 0};
    const auto empty_rows = view("<f4", grown_shape, empty);
    const auto grown = VrsArrayBlocks::patch_and_append(
        zero_width.array, {}, nullptr, &empty_rows);
    assert((grown.array->shape == std::vector<std::size_t>{7, 0}));
    assert(grown.array->data == zero_width.array->data);
}

void validation_and_corruption() {
    std::vector<float> numbers(8);
    const auto raw = bytes(numbers);
    const std::vector<std::size_t> shape{8};
    const auto prepared = VrsArrayBlocks::prepare(view("<f4", shape, raw));
    const std::map<std::string, NumericArrayView> source_map{{"score", view("<f4", shape, raw)}};
    const auto prepared_bundle = VrsArrayBundle::prepare(source_map);
    const auto reused_bundle = VrsArrayBundle::prepare(
        source_map, &prepared_bundle.bundle);
    assert(reused_bundle.bundle.arrays.at("score")->data->blocks[0] ==
           prepared_bundle.bundle.arrays.at("score")->data->blocks[0]);
    const std::map<std::string, NumericArrayView> renamed_map{{"renamed", view("<f4", shape, raw)}};
    assert(rejects([&] {
        (void)VrsArrayBundle::prepare(renamed_map, &prepared_bundle.bundle);
    }));
    const std::vector<float> replacement{2.0F};
    const auto replacement_raw = bytes(replacement);
    const std::vector<std::size_t> one_shape{1};
    const auto replacement_view = view("<f4", one_shape, replacement_raw);
    const std::array<std::size_t, 2> duplicate{1, 1};
    assert(rejects([&] {
        (void)VrsArrayBlocks::patch_and_append(prepared.array, duplicate, &replacement_view);
    }));
    const std::array<std::size_t, 1> past_end{8};
    assert(rejects([&] {
        (void)VrsArrayBlocks::patch_and_append(prepared.array, past_end, &replacement_view);
    }));
    const auto wrong_dtype = view(">f4", one_shape, replacement_raw);
    const std::array<std::size_t, 1> one{1};
    assert(rejects([&] {
        (void)VrsArrayBlocks::patch_and_append(prepared.array, one, &wrong_dtype);
    }));
    const std::vector<std::size_t> wrong_tail_shape{2, 2};
    const std::vector<std::byte> wrong_tail(16);
    const auto wrong_tail_view = view("<f4", wrong_tail_shape, wrong_tail);
    assert(rejects([&] {
        (void)VrsArrayBlocks::patch_and_append(prepared.array, one, &replacement_view,
                                               &wrong_tail_view);
    }));
    const std::vector<std::size_t> scalar_shape;
    const std::vector<float> scalar_value{1.0F};
    const auto scalar_raw = bytes(scalar_value);
    const auto scalar = VrsArrayBlocks::prepare(view("<f4", scalar_shape, scalar_raw));
    assert(rejects([&] {
        (void)VrsArrayBlocks::patch_and_append(scalar.array, one, &replacement_view);
    }));
    assert(rejects([&] {
        (void)VrsArrayBlocks::prepare(view("f4", shape, raw));
    }));
    const std::vector<std::size_t> matrix_shape{2, 4};
    assert(rejects([&] {
        (void)VrsArrayBlocks::prepare(view("<f4", matrix_shape, raw, false));
    }));

    TemporaryDirectory temporary;
    VrsGenerationBlockStore store(temporary.path);
    VrsArrayBundle bundle({{"score", prepared.array}});
    const auto saved = bundle.save(store);
    const auto path = temporary.path / (saved.bundle_sha256 + ".arrays.json");
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        char byte{};
        file.read(&byte, 1);
        file.seekp(0);
        byte ^= 1;
        file.write(&byte, 1);
    }
    assert(corrupts([&] {
        (void)VrsArrayBundle::load(store, saved.bundle_sha256, 1024);
    }));

    TemporaryDirectory unicode_temporary;
    VrsGenerationBlockStore unicode_store(unicode_temporary.path);
    const std::vector<float> one_number{1.0F};
    const auto one_raw = bytes(one_number);
    const std::vector<std::size_t> unicode_shape{1};
    const auto unicode_array = VrsArrayBlocks::prepare(
        view("<f4", unicode_shape, one_raw), {}, 64);
    const std::string unicode_name = "\xec\xa0\x90\xec\x88\x98\xf0\x9f\x98\x80\n";
    const auto unicode_saved = VrsArrayBundle({{unicode_name, unicode_array.array}}).save(
        unicode_store);
    assert(unicode_saved.bundle_sha256 ==
           "fc57b68352cb934388dd6bf9766b42ed0ecb7c9f38b5fceaefebb14e3a09d3cf");
    assert(unicode_saved.bundle_bytes == 201);
    const auto unicode_loaded = VrsArrayBundle::load(
        unicode_store, unicode_saved.bundle_sha256, 1024);
    assert(unicode_loaded.arrays.contains(unicode_name));
    assert(unicode_loaded.arrays.at(unicode_name)->restore() == one_raw);
}

}  // namespace

int main() {
    static_assert(!std::is_assignable_v<VrsArrayBlocks&, const VrsArrayBlocks&>);
    assert(vrs_array_blocks_source_sha256 ==
           "52689577b7cbb1d4fc14cc5f4c84eb35a1cbfc3692a493622321176de2c40e24");
    assert(vrs_block_store_source_sha256 ==
           "d0ac58b872e73943e40d7ec4ecfdfb2f2b9f01c762ffc697800d7cce8edf2b19");
    exact_python_fixture();
    sparse_geometry_and_bits();
    validation_and_corruption();
    std::cout << "VRS typed array block tests passed\n";
}
