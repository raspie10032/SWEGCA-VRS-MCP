#include "checkpoint/restricted_zip.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using swegca::checkpoint::RestrictedZip;
using swegca::checkpoint::ZipLimits;

namespace {

std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (unsigned bit = 0; bit != 8; ++bit) {
            const auto mask = std::uint32_t(0) - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void put16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(std::byte(value));
    out.push_back(std::byte(value >> 8U));
}

void put32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index) out.push_back(std::byte(value >> (8U * index)));
}

void put64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index) out.push_back(std::byte(value >> (8U * index)));
}

std::uint16_t get16(std::span<const std::byte> body, std::size_t at) {
    assert(at <= body.size() && body.size() - at >= 2);
    return std::uint16_t(std::to_integer<unsigned>(body[at])) |
        std::uint16_t(std::to_integer<unsigned>(body[at + 1]) << 8U);
}

std::uint32_t get32(std::span<const std::byte> body, std::size_t at) {
    assert(at <= body.size() && body.size() - at >= 4);
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index) {
        value |= std::uint32_t(std::to_integer<unsigned>(body[at + index])) << (8U * index);
    }
    return value;
}

void set16(std::vector<std::byte>& out, std::size_t at, std::uint16_t value) {
    out.at(at) = std::byte(value);
    out.at(at + 1) = std::byte(value >> 8U);
}

void set32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index) out.at(at + index) = std::byte(value >> (8U * index));
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t index = 0; index != text.size(); ++index) out[index] = std::byte(text[index]);
    return out;
}

struct Input {
    std::string name;
    std::vector<std::byte> body;
    std::uint16_t flags = 0x0808;
    std::uint16_t method = 0;
};

struct Built {
    std::vector<std::byte> body;
    std::vector<std::size_t> local_headers;
    std::vector<std::size_t> payloads;
    std::vector<std::size_t> descriptors;
    std::vector<std::size_t> central_headers;
    std::size_t eocd = 0;
};

Built build(std::span<const Input> inputs) {
    Built result;
    struct Central { Input input; std::uint32_t crc; std::uint32_t local; };
    std::vector<Central> central;
    for (const auto& input : inputs) {
        const auto local = static_cast<std::uint32_t>(result.body.size());
        result.local_headers.push_back(result.body.size());
        const auto crc = crc32(input.body);
        put32(result.body, 0x04034b50U); put16(result.body, 0); put16(result.body, input.flags);
        put16(result.body, input.method); put16(result.body, 0); put16(result.body, 0);
        const bool descriptor = (input.flags & 8U) != 0;
        put32(result.body, descriptor ? 0 : crc);
        put32(result.body, descriptor ? 0 : static_cast<std::uint32_t>(input.body.size()));
        put32(result.body, descriptor ? 0 : static_cast<std::uint32_t>(input.body.size()));
        put16(result.body, static_cast<std::uint16_t>(input.name.size())); put16(result.body, 0);
        for (const char value : input.name) result.body.push_back(std::byte(value));
        result.payloads.push_back(result.body.size());
        result.body.insert(result.body.end(), input.body.begin(), input.body.end());
        if (descriptor) {
            result.descriptors.push_back(result.body.size());
            put32(result.body, 0x08074b50U); put32(result.body, crc);
            put32(result.body, static_cast<std::uint32_t>(input.body.size()));
            put32(result.body, static_cast<std::uint32_t>(input.body.size()));
        } else {
            result.descriptors.push_back(0);
        }
        central.push_back({input, crc, local});
    }
    const auto central_offset = static_cast<std::uint32_t>(result.body.size());
    for (const auto& entry : central) {
        result.central_headers.push_back(result.body.size());
        put32(result.body, 0x02014b50U); put16(result.body, 0); put16(result.body, 0);
        put16(result.body, entry.input.flags); put16(result.body, entry.input.method);
        put16(result.body, 0); put16(result.body, 0); put32(result.body, entry.crc);
        put32(result.body, static_cast<std::uint32_t>(entry.input.body.size()));
        put32(result.body, static_cast<std::uint32_t>(entry.input.body.size()));
        put16(result.body, static_cast<std::uint16_t>(entry.input.name.size()));
        put16(result.body, 0); put16(result.body, 0); put16(result.body, 0); put16(result.body, 0);
        put32(result.body, 0); put32(result.body, entry.local);
        for (const char value : entry.input.name) result.body.push_back(std::byte(value));
    }
    const auto central_size = static_cast<std::uint32_t>(result.body.size() - central_offset);
    result.eocd = result.body.size();
    put32(result.body, 0x06054b50U); put16(result.body, 0); put16(result.body, 0);
    put16(result.body, static_cast<std::uint16_t>(inputs.size()));
    put16(result.body, static_cast<std::uint16_t>(inputs.size()));
    put32(result.body, central_size); put32(result.body, central_offset); put16(result.body, 0);
    return result;
}

Built with_zip64_trailer(Built value) {
    const auto entries = get16(value.body, value.eocd + 10);
    const auto central_size = get32(value.body, value.eocd + 12);
    const auto central_offset = get32(value.body, value.eocd + 16);
    const auto zip64_offset = value.eocd;
    std::vector<std::byte> trailer;
    put32(trailer, 0x06064b50U); put64(trailer, 44); put16(trailer, 45); put16(trailer, 45);
    put32(trailer, 0); put32(trailer, 0); put64(trailer, entries); put64(trailer, entries);
    put64(trailer, central_size); put64(trailer, central_offset);
    put32(trailer, 0x07064b50U); put32(trailer, 0); put64(trailer, zip64_offset); put32(trailer, 1);
    value.body.insert(value.body.begin() + static_cast<std::ptrdiff_t>(value.eocd),
        trailer.begin(), trailer.end());
    value.eocd += trailer.size();
    return value;
}

std::filesystem::path write(const std::filesystem::path& root, std::string_view name,
    std::span<const std::byte> body) {
    const auto path = root / name;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    if (!stream) throw std::runtime_error("test ZIP write failed");
    return path;
}

template<class Function> void fails(Function&& function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("restricted-zip-tests-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::array<Input, 2> input{{
        {"checkpoint/data.pkl", bytes("pickle-metadata")},
        {"checkpoint/data/0", bytes("tensor-storage")},
    }};
    const auto valid = build(input);
    const auto valid_path = write(root, "valid.zip", valid.body);
    {
        auto archive = RestrictedZip::open(valid_path);
        const auto identity = archive.identity();
        assert(identity.size == valid.body.size());
        assert(archive.members().size() == 2);
        assert(archive.archive_bytes() == valid.body.size());
        assert(archive.find("checkpoint/data.pkl") != nullptr);
        assert(archive.find("missing") == nullptr);
        assert(archive.read("checkpoint/data.pkl") == input[0].body);
        assert(archive.read("checkpoint/data/0") == input[1].body);
        fails([&] { (void)archive.read("missing"); });
        auto moved = std::move(archive);
        assert(moved.identity() == identity);
        assert(moved.read("checkpoint/data/0") == input[1].body);
    }
    {
        const auto mutable_path = write(root, "mutated-after-open.zip", valid.body);
        auto archive = RestrictedZip::open(mutable_path);
        auto changed = valid.body;
        changed[valid.payloads[0]] ^= std::byte{1};
        write(root, "mutated-after-open.zip", changed);
        assert(archive.identity().inode != 0);
        fails([&] { (void)archive.read("checkpoint/data/0"); });
    }
    {
        const auto zip64 = with_zip64_trailer(build(input));
        auto archive = RestrictedZip::open(write(root, "valid-zip64.zip", zip64.body));
        assert(archive.read("checkpoint/data.pkl") == input[0].body);
        auto changed = zip64.body;
        // Corrupt the ZIP64 locator's referenced EOCD offset.
        changed[zip64.eocd - 12] ^= std::byte{1};
        fails([&] { (void)RestrictedZip::open(write(root, "bad-zip64.zip", changed)); });
    }
    fails([&] { (void)RestrictedZip::open(valid_path, ZipLimits{8, 8, 2, 128}); });
    fails([&] { (void)RestrictedZip::open(valid_path, ZipLimits{4096, 4, 2, 128}); });
    fails([&] { (void)RestrictedZip::open(valid_path, ZipLimits{4096, 4096, 1, 128}); });

    const std::array<Input, 2> duplicate{{{"same", bytes("a")}, {"same", bytes("b")}}};
    fails([&] { (void)RestrictedZip::open(write(root, "duplicate.zip", build(duplicate).body)); });
    const std::array<Input, 1> unsafe{{{"../escape", bytes("x")}}};
    fails([&] { (void)RestrictedZip::open(write(root, "unsafe.zip", build(unsafe).body)); });
    const std::array<Input, 1> encrypted{{{"safe", bytes("x"), 0x0809, 0}}};
    fails([&] { (void)RestrictedZip::open(write(root, "encrypted.zip", build(encrypted).body)); });
    const std::array<Input, 1> compressed{{{"safe", bytes("x"), 0x0808, 8}}};
    fails([&] { (void)RestrictedZip::open(write(root, "compressed.zip", build(compressed).body)); });
    const std::array<Input, 1> direct{{{"direct", bytes("stored"), 0x0800, 0}}};
    {
        auto archive = RestrictedZip::open(write(root, "direct.zip", build(direct).body));
        assert(archive.read("direct") == direct[0].body);
    }

    {
        auto changed = valid.body;
        changed[valid.payloads[0]] ^= std::byte{1};
        auto archive = RestrictedZip::open(write(root, "bad-crc.zip", changed));
        fails([&] { (void)archive.read("checkpoint/data.pkl"); });
        assert(archive.read("checkpoint/data/0") == input[1].body);
    }
    {
        auto changed = valid.body;
        changed[valid.local_headers[0] + 30] ^= std::byte{1};
        fails([&] { (void)RestrictedZip::open(write(root, "name-mismatch.zip", changed)); });
    }
    {
        auto changed = valid.body;
        set32(changed, valid.descriptors[0] + 4, 0);
        fails([&] { (void)RestrictedZip::open(write(root, "descriptor-mismatch.zip", changed)); });
    }
    {
        auto changed = valid.body;
        set32(changed, valid.central_headers[0] + 42, 0xffffffffU);
        fails([&] { (void)RestrictedZip::open(write(root, "zip64-offset.zip", changed)); });
    }
    {
        auto changed = valid.body;
        set16(changed, valid.central_headers[0] + 8, 0x1808);
        fails([&] { (void)RestrictedZip::open(write(root, "unsupported-flags.zip", changed)); });
    }
    {
        auto changed = valid.body;
        set16(changed, valid.central_headers[0] + 4, 0x031e);
        set32(changed, valid.central_headers[0] + 38, 0120000U << 16U);
        fails([&] { (void)RestrictedZip::open(write(root, "symlink.zip", changed)); });
    }
    {
        auto changed = valid.body;
        changed.pop_back();
        fails([&] { (void)RestrictedZip::open(write(root, "truncated.zip", changed)); });
    }
    {
        auto changed = valid.body;
        set32(changed, valid.eocd + 12, 1);
        fails([&] { (void)RestrictedZip::open(write(root, "central-span.zip", changed)); });
    }

    std::filesystem::remove_all(root);
    std::cout << "PASS restricted stored ZIP32 parsing, integrity, bounds, and rejection gates\n";
}
