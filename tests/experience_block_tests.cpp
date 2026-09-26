#include "vrs/experience_block.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace swegca::vrs;
namespace fs = std::filesystem;
static unsigned checks = 0;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        std::abort(); \
    } \
} while (false)

template<class Exception, class Function>
void expect_throw(Function operation) {
    bool expected = false;
    try { operation(); } catch (const Exception&) { expected = true; }
    CHECK(expected);
}

std::span<const std::byte> bytes(const std::string& value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

int main() {
    std::array<char, 64> pattern{};
    const std::string base = (fs::temp_directory_path() / "swegca-block-test-XXXXXX").string();
    CHECK(base.size() < pattern.size());
    std::copy(base.begin(), base.end(), pattern.begin());
    const auto made = ::mkdtemp(pattern.data());
    CHECK(made != nullptr);
    const fs::path directory(made);
    const auto path = directory / "original.block";
    swegca::architecture::DigestBytes identity{};
    identity[0] = std::byte{1};
    const auto capacity = std::uint64_t{1} << 20;
    const std::string raw("첫 대화\0반박과 불확실성", sizeof("첫 대화\0반박과 불확실성") - 1);
    const OriginalExperienceView first{0, 1234, "session-1", "user-input", "text/plain", bytes(raw)};
    std::string binary(100000, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<char>(i % 251);
    const OriginalExperienceView second{1, 1235, "session-1", "tool-output", "application/octet-stream", bytes(binary)};
    ExperienceLocation first_location, second_location, empty_location;
    {
        auto block = ExperienceBlock::create(path, identity, capacity);
        CHECK(block.can_append());
        first_location = block.append(first);
        second_location = block.append(second);
        empty_location = block.append({2, 1236, "session-1", "assistant", "text/plain", {}});
        CHECK(first_location.offset == ExperienceBlock::header_bytes);
        CHECK(second_location.offset == first_location.offset + first_location.bytes);
        CHECK(empty_location.offset == second_location.offset + second_location.bytes);
        CHECK(fs::file_size(path) == empty_location.offset + empty_location.bytes);
        expect_throw<std::system_error>([&] { (void)ExperienceBlock::open_writer(path); });
        expect_throw<std::system_error>([&] { (void)ExperienceBlock::create(path, identity, capacity); });
        auto reader = ExperienceBlock::open_reader(path);
        CHECK(!reader.can_append());
        expect_throw<std::logic_error>([&] { (void)reader.append(first); });
        auto original = reader.read(first_location, capacity);
        CHECK(original.view().sequence == 0);
        CHECK(original.view().observed_at_ns == 1234);
        CHECK(original.view().session == first.session);
        CHECK(original.view().source == first.source);
        CHECK(original.view().media_type == first.media_type);
        CHECK(std::ranges::equal(original.view().content, bytes(raw)));
        CHECK(reader.read(empty_location, capacity).view().content.empty());
        auto moved = std::move(original);
        CHECK(std::ranges::equal(moved.view().content, bytes(raw)));
        const auto inspected = reader.inspect();
        CHECK(inspected.complete_records == 3);
        CHECK(inspected.complete_bytes == fs::file_size(path));
        CHECK(inspected.unfinished_bytes == 0);
        expect_throw<std::invalid_argument>([&] { (void)reader.read(first_location, first_location.bytes - 1); });
        auto wrong = first_location;
        wrong.block[1] = std::byte{7};
        expect_throw<std::invalid_argument>([&] { (void)reader.read(wrong, capacity); });
        wrong = first_location;
        wrong.offset = std::numeric_limits<std::uint64_t>::max();
        expect_throw<std::invalid_argument>([&] { (void)reader.read(wrong, capacity); });
        wrong = first_location;
        wrong.bytes = std::numeric_limits<std::uint64_t>::max();
        expect_throw<std::invalid_argument>([&] { (void)reader.read(wrong, capacity); });
        wrong = first_location;
        wrong.digest[0] ^= std::byte{1};
        expect_throw<std::runtime_error>([&] { (void)reader.read(wrong, capacity); });
        wrong = first_location;
        ++wrong.bytes;
        expect_throw<std::runtime_error>([&] { (void)reader.read(wrong, capacity); });
    }
    // Writer reopen validates previous records with bounded scratch, then
    // appends without changing the old bytes or locations.
    {
        auto block = ExperienceBlock::open_writer(path);
        CHECK(block.can_append());
        auto moved = std::move(block);
        CHECK(!block.can_append());
        const auto appended = moved.append({3, 1237, "session-1", "user-input", "text/plain", bytes(raw)});
        CHECK(appended.offset == empty_location.offset + empty_location.bytes);
        CHECK(moved.inspect().complete_records == 4);
        const auto saved = moved.read(second_location, capacity);
        CHECK(std::ranges::equal(saved.view().content, bytes(binary)));
    }
    // Short last record at three different physical boundaries. Keep the
    // complete prefix readable and leave every unfinished byte untouched.
    unsigned variant = 0;
    for (const auto tail : std::array<std::uint64_t, 3>{17, 80, second_location.bytes - 3}) {
        const auto torn_path = directory / ("torn-" + std::to_string(variant++));
        fs::copy_file(path, torn_path);
        fs::resize_file(torn_path, second_location.offset + tail);
        const auto before = fs::file_size(torn_path);
        auto torn = ExperienceBlock::open_writer(torn_path);
        CHECK(!torn.can_append());
        const auto recovered = torn.inspect();
        CHECK(recovered.complete_records == 1);
        CHECK(recovered.complete_bytes == second_location.offset);
        CHECK(recovered.unfinished_bytes == tail);
        const auto intact = torn.read(first_location, capacity);
        CHECK(std::ranges::equal(intact.view().content, bytes(raw)));
        expect_throw<std::logic_error>([&] { (void)torn.append(first); });
        expect_throw<std::runtime_error>([&] { (void)torn.read(second_location, capacity); });
        CHECK(fs::file_size(torn_path) == before);
    }
    // Completed record corruption must throw, not masquerade as an unfinished
    // tail. A selected read of an earlier intact record does not scan the bad one.
    const auto corrupt_path = directory / "corrupt.block";
    fs::copy_file(path, corrupt_path);
    {
        std::fstream damaged(corrupt_path, std::ios::in | std::ios::out | std::ios::binary);
        damaged.seekp(static_cast<std::streamoff>(second_location.offset + 64));
        damaged.put('X');
        CHECK(bool(damaged));
    }
    {
        auto corrupt = ExperienceBlock::open_reader(corrupt_path);
        const auto intact = corrupt.read(first_location, capacity);
        CHECK(std::ranges::equal(intact.view().content, bytes(raw)));
        expect_throw<std::runtime_error>([&] { (void)corrupt.read(second_location, capacity); });
        expect_throw<std::runtime_error>([&] { (void)corrupt.inspect(); });
        expect_throw<std::runtime_error>([&] { (void)ExperienceBlock::open_writer(corrupt_path); });
    }
    const auto malformed_path = directory / "malformed.block";
    fs::copy_file(path, malformed_path);
    {
        std::fstream damaged(malformed_path, std::ios::in | std::ios::out | std::ios::binary);
        damaged.seekp(static_cast<std::streamoff>(first_location.offset + 32));
        const std::array<char, 8> impossible_length{char(0xff), char(0xff), char(0xff), char(0xff),
                                                  char(0xff), char(0xff), char(0xff), char(0xff)};
        damaged.write(impossible_length.data(), impossible_length.size());
        CHECK(bool(damaged));
    }
    {
        auto malformed = ExperienceBlock::open_reader(malformed_path);
        expect_throw<std::runtime_error>([&] { (void)malformed.read(first_location, capacity); });
        expect_throw<std::runtime_error>([&] { (void)malformed.inspect(); });
    }
    const auto short_header = directory / "short-header.block";
    fs::copy_file(path, short_header);
    fs::resize_file(short_header, 40);
    expect_throw<std::runtime_error>([&] { (void)ExperienceBlock::open_reader(short_header); });
    CHECK(fs::file_size(short_header) == 40);
    const auto bounded_path = directory / "bounded.block";
    {
        auto bounded = ExperienceBlock::create(bounded_path, identity, ExperienceBlock::header_bytes + first_location.bytes);
        const auto before = fs::file_size(bounded_path);
        auto no_source = first;
        no_source.source = {};
        expect_throw<std::invalid_argument>([&] { (void)bounded.append(no_source); });
        expect_throw<std::length_error>([&] { (void)bounded.append(second); });
        CHECK(fs::file_size(bounded_path) == before);
        CHECK(bounded.can_append());
        const auto exact = bounded.append(first);
        CHECK(exact.bytes == first_location.bytes);
        CHECK(fs::file_size(bounded_path) == bounded.capacity());
        expect_throw<std::length_error>([&] { (void)bounded.append(first); });
        CHECK(bounded.inspect().complete_records == 1);
    }
    expect_throw<std::invalid_argument>([&] { (void)ExperienceBlock::create(directory / "zero.block", {}, capacity); });
    CHECK(!fs::exists(directory / "zero.block"));
    fs::remove_all(directory);  // Only this mkdtemp-owned test directory.
    std::printf("PASS: %u experience block checks\n", checks);
}
