#include "world/vrs_array_blocks.hpp"

#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory_resource>
#include <numeric>
#include <set>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace swegca::world {
namespace {
using architecture::Sha256;
using transport::Json;

[[noreturn]] void reject(const char* message) { throw std::invalid_argument(message); }
[[noreturn]] void io_error(const char* message) {
    throw std::system_error(errno, std::generic_category(), message);
}

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const auto byte = std::to_integer<unsigned>(digest[i]);
        result[2 * i] = digits[byte >> 4];
        result[2 * i + 1] = digits[byte & 15];
    }
    return result;
}

std::string sha256(const std::span<const std::byte> data) {
    return hex(Sha256::of(data));
}
std::string sha256(const std::string_view data) {
    Sha256 hash;
    hash.update(data);
    return hex(hash.finish());
}

bool identifier(const std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void require_identifier(const std::string_view value) {
    if (!identifier(value)) reject("invalid content address");
}

std::size_t multiply(const std::size_t left, const std::size_t right) {
    if (left && right > std::numeric_limits<std::size_t>::max() / left)
        throw std::length_error("array byte layout overflow");
    return left * right;
}

std::size_t dtype_size(const std::string_view dtype) {
    if (dtype == "|b1" || dtype == "|i1" || dtype == "|u1") return 1;
    if (dtype.size() < 3 || (dtype[0] != '<' && dtype[0] != '>'))
        reject("only canonical plain bounded numeric dtypes are supported");
    const char kind = dtype[1];
    std::size_t size{};
    const auto parsed = std::from_chars(dtype.data() + 2, dtype.data() + dtype.size(), size);
    if (parsed.ec != std::errc{} || parsed.ptr != dtype.data() + dtype.size() || !size || size > 16)
        reject("only canonical plain bounded numeric dtypes are supported");
    const bool valid =
        ((kind == 'i' || kind == 'u') && (size == 2 || size == 4 || size == 8)) ||
        (kind == 'f' && (size == 2 || size == 4 || size == 8 || size == 16)) ||
        (kind == 'c' && (size == 8 || size == 16));
    if (!valid) reject("only canonical plain bounded numeric dtypes are supported");
    return size;
}

std::size_t element_count(const std::span<const std::size_t> shape) {
    if (shape.size() > 8) reject("invalid array shape");
    std::size_t count = 1;
    for (const auto extent : shape) count = multiply(count, extent);
    return count;
}

std::uint32_t checksum(const std::span<const std::byte> raw) {
    auto value = ::crc32(0L, Z_NULL, 0);
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const auto chunk = static_cast<uInt>(std::min<std::size_t>(raw.size() - offset, UINT_MAX));
        value = ::crc32(value,
                        reinterpret_cast<const Bytef*>(raw.data() + offset), chunk);
        offset += chunk;
    }
    return static_cast<std::uint32_t>(value);
}

std::string block_header(const LosslessVrsBlock& block) {
    return "{\"codec\":\"" + std::string(block.codec_name()) + "\",\"crc32\":" +
           std::to_string(block.crc32) + ",\"raw_size\":" +
           std::to_string(block.raw_size) + "}";
}

std::vector<std::byte> block_wire(const LosslessVrsBlock& block) {
    const auto header = block_header(block);
    std::vector<std::byte> result;
    result.reserve(header.size() + 1 + block.payload.size());
    for (const char c : header) result.push_back(static_cast<std::byte>(c));
    result.push_back(std::byte{'\n'});
    result.insert(result.end(), block.payload.begin(), block.payload.end());
    return result;
}

std::shared_ptr<const LosslessVrsBlock> make_block(
    const std::span<const std::byte> raw, const VrsBlockCodec requested) {
    if (raw.empty() || raw.size() > maximum_vrs_block_bytes)
        reject("block raw_size outside bounded range");
    std::vector<std::byte> payload(raw.begin(), raw.end());
    auto codec = VrsBlockCodec::raw;
    if (requested == VrsBlockCodec::zlib) {
        uLongf bound = ::compressBound(static_cast<uLong>(raw.size()));
        std::vector<std::byte> encoded(bound);
        auto encoded_size = bound;
        const auto status = ::compress2(
            reinterpret_cast<Bytef*>(encoded.data()), &encoded_size,
            reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), 3);
        if (status != Z_OK) throw std::runtime_error("zlib compression failed");
        encoded.resize(encoded_size);
        if (encoded.size() < raw.size()) {
            payload = std::move(encoded);
            codec = VrsBlockCodec::zlib;
        }
    }
    return std::make_shared<const LosslessVrsBlock>(codec, std::move(payload), raw.size(), checksum(raw));
}

std::string generation_manifest(const VrsBlockGeneration& generation) {
    std::string result = "{\"block_bytes\":" + std::to_string(generation.block_bytes) +
                         ",\"blocks\":[";
    for (std::size_t i = 0; i < generation.blocks.size(); ++i) {
        if (i) result.push_back(',');
        result += "\"" + generation.blocks[i]->digest + "\"";
    }
    result += "],\"raw_size\":" + std::to_string(generation.raw_size) +
              ",\"schema\":\"" + std::string(vrs_block_generation_schema) + "\"}";
    return result;
}

std::size_t uint_value(const Json& value) {
    if (value.kind != Json::Kind::number || value.scalar.empty() || value.scalar.front() == '-')
        reject("expected nonnegative integer");
    std::size_t result{};
    const auto parsed = std::from_chars(value.scalar.data(), value.scalar.data() + value.scalar.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.scalar.data() + value.scalar.size())
        reject("expected nonnegative integer");
    return result;
}

void exact_keys(const Json& value, const std::initializer_list<std::string_view> expected) {
    if (value.kind != Json::Kind::object || value.keys.size() != expected.size())
        reject("JSON object schema changed");
    for (const auto key : expected) if (!value.find(key)) reject("JSON object schema changed");
}

std::vector<std::size_t> shape_value(const Json& value) {
    if (value.kind != Json::Kind::array || value.values.size() > 8) reject("invalid array shape");
    std::vector<std::size_t> shape;
    shape.reserve(value.values.size());
    for (const auto& extent : value.values) shape.push_back(uint_value(extent));
    return shape;
}

std::vector<std::byte> as_bytes(const std::string_view value) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                  reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

std::string json_ascii(const std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result{"\""};
    const auto escaped = [&](const std::uint32_t codepoint) {
        result += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4)
            result.push_back(digits[(codepoint >> shift) & 15]);
    };
    for (std::size_t offset = 0; offset < value.size();) {
        const auto first = static_cast<unsigned char>(value[offset++]);
        if (first < 0x80) {
            switch (first) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\t': result += "\\t"; break;
            case '\n': result += "\\n"; break;
            case '\f': result += "\\f"; break;
            case '\r': result += "\\r"; break;
            default:
                if (first < 0x20) escaped(first);
                else result.push_back(static_cast<char>(first));
            }
            continue;
        }
        unsigned remaining{};
        std::uint32_t codepoint{};
        std::uint32_t minimum{};
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1; codepoint = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2; codepoint = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3; codepoint = first & 0x07; minimum = 0x10000;
        } else reject("array name is not valid UTF-8");
        if (remaining > value.size() - offset) reject("array name is not valid UTF-8");
        for (unsigned i = 0; i < remaining; ++i) {
            const auto next = static_cast<unsigned char>(value[offset++]);
            if ((next & 0xc0) != 0x80) reject("array name is not valid UTF-8");
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff))
            reject("array name is not valid UTF-8");
        if (codepoint <= 0xffff) escaped(codepoint);
        else {
            const auto scalar = codepoint - 0x10000;
            escaped(0xd800 + (scalar >> 10));
            escaped(0xdc00 + (scalar & 0x3ff));
        }
    }
    result.push_back('"');
    return result;
}

void sync_directory(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) io_error("open VRS block directory");
    int result;
    do result = ::fsync(descriptor); while (result < 0 && errno == EINTR);
    const int saved = errno;
    ::close(descriptor);
    if (result < 0) { errno = saved; io_error("sync VRS block directory"); }
}

void write_all(const int descriptor, const std::span<const std::byte> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(descriptor, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) io_error("write VRS candidate");
        offset += static_cast<std::size_t>(written);
    }
}

}  // namespace

LosslessVrsBlock::LosslessVrsBlock(const VrsBlockCodec codec_value,
                                   std::vector<std::byte> payload_value,
                                   const std::size_t raw_size_value,
                                   const std::uint32_t crc32_value)
    : codec(codec_value), payload(std::move(payload_value)), raw_size(raw_size_value),
      crc32(crc32_value) {
    if (!raw_size || raw_size > maximum_vrs_block_bytes || payload.size() > raw_size ||
        (codec == VrsBlockCodec::raw && payload.size() != raw_size))
        reject("invalid lossless VRS block");
}

std::string_view LosslessVrsBlock::codec_name() const noexcept {
    return codec == VrsBlockCodec::raw ? "raw" : "zlib";
}

std::vector<std::byte> LosslessVrsBlock::decode() const {
    std::vector<std::byte> raw(raw_size);
    if (codec == VrsBlockCodec::raw) raw = payload;
    else {
        z_stream stream{};
        if (::inflateInit(&stream) != Z_OK) throw CorruptVrsBlock("zlib decoder initialization failed");
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(payload.data()));
        stream.avail_in = static_cast<uInt>(payload.size());
        stream.next_out = reinterpret_cast<Bytef*>(raw.data());
        stream.avail_out = static_cast<uInt>(raw.size());
        const int status = ::inflate(&stream, Z_FINISH);
        const bool valid = status == Z_STREAM_END && stream.total_out == raw_size &&
                           stream.avail_in == 0;
        ::inflateEnd(&stream);
        if (!valid) throw CorruptVrsBlock("incomplete, trailing or oversized zlib frame");
    }
    if (checksum(raw) != crc32) throw CorruptVrsBlock("block length or checksum mismatch");
    return raw;
}

StoredVrsBlock::StoredVrsBlock(std::shared_ptr<const LosslessVrsBlock> block_value,
                               std::string digest_value)
    : block(std::move(block_value)), digest(std::move(digest_value)) {
    if (!block || !identifier(digest) || sha256(block_wire(*block)) != digest)
        throw CorruptVrsBlock("block address mismatch");
    (void)block->decode();
}

std::shared_ptr<const StoredVrsBlock> StoredVrsBlock::build(
    const std::span<const std::byte> raw, const VrsBlockCodec codec) {
    const auto block = make_block(raw, codec);
    return std::make_shared<const StoredVrsBlock>(block, sha256(block_wire(*block)));
}

VrsBlockGeneration::VrsBlockGeneration(
    std::vector<std::shared_ptr<const StoredVrsBlock>> blocks_value,
    const std::size_t block_bytes_value, const std::size_t raw_size_value)
    : blocks(std::move(blocks_value)), block_bytes(block_bytes_value), raw_size(raw_size_value) {
    if (!block_bytes || block_bytes > maximum_vrs_block_bytes ||
        blocks.size() != (raw_size + block_bytes - 1) / block_bytes)
        reject("invalid block generation layout");
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (!blocks[i] || blocks[i]->block->raw_size !=
                std::min(block_bytes, raw_size - i * block_bytes))
            reject("invalid block generation geometry");
    }
}

std::shared_ptr<const VrsBlockGeneration> VrsBlockGeneration::build(
    const std::span<const std::byte> raw, const std::size_t block_bytes,
    const VrsBlockCodec codec) {
    if (!block_bytes || block_bytes > maximum_vrs_block_bytes)
        reject("invalid block size");
    std::vector<std::shared_ptr<const StoredVrsBlock>> blocks;
    for (std::size_t start = 0; start < raw.size(); start += block_bytes)
        blocks.push_back(StoredVrsBlock::build(raw.subspan(
            start, std::min(block_bytes, raw.size() - start)), codec));
    return std::make_shared<const VrsBlockGeneration>(
        std::move(blocks), block_bytes, raw.size());
}

std::vector<std::byte> VrsBlockGeneration::read(
    const std::size_t start, const std::optional<std::size_t> stop_value) const {
    const auto stop = stop_value.value_or(raw_size);
    if (start > stop || stop > raw_size) reject("read outside generation");
    std::vector<std::byte> result;
    result.reserve(stop - start);
    if (start == stop) return result;
    for (std::size_t i = start / block_bytes; i <= (stop - 1) / block_bytes; ++i) {
        const auto raw = blocks[i]->block->decode();
        const auto base = i * block_bytes;
        const auto begin = std::max(start, base) - base;
        const auto end = std::min(stop, base + raw.size()) - base;
        result.insert(result.end(), raw.begin() + static_cast<std::ptrdiff_t>(begin),
                      raw.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
}

VrsGenerationBlockStore::VrsGenerationBlockStore(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root))) {
    std::filesystem::create_directories(root_);
}

void VrsGenerationBlockStore::publish_immutable(
    const std::string_view name, const std::span<const std::byte> data) {
    publish(name, data);
}

std::vector<std::byte> VrsGenerationBlockStore::read_immutable(
    const std::string_view name, const std::size_t maximum_bytes) const {
    if (name.empty() || name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos)
        throw std::invalid_argument("immutable record name changed");
    return read_limited(root_ / name, maximum_bytes);
}

void VrsGenerationBlockStore::publish(const std::string_view name,
                                      const std::span<const std::byte> data) {
    if (name.empty() || name.find('/') != std::string_view::npos) reject("invalid candidate name");
    std::string pattern = (root_ / ".pending-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    int descriptor = ::mkstemp(mutable_pattern.data());
    if (descriptor < 0) io_error("create VRS pending candidate");
    const std::filesystem::path pending(mutable_pattern.data());
    try {
        write_all(descriptor, data);
        int result;
        do result = ::fsync(descriptor); while (result < 0 && errno == EINTR);
        if (result < 0) io_error("sync VRS pending candidate");
        const auto close_result = ::close(descriptor);
        descriptor = -1;
        if (close_result < 0) io_error("close VRS pending candidate");
        const auto target = root_ / std::string(name);
        if (::link(pending.c_str(), target.c_str()) < 0) {
            if (errno != EEXIST) io_error("publish immutable VRS candidate");
            const auto status = std::filesystem::symlink_status(target);
            if (!std::filesystem::is_regular_file(status) ||
                read_limited(target, data.size()) != std::vector<std::byte>(data.begin(), data.end()))
                throw CorruptVrsBlock("existing content address differs");
        }
        sync_directory(root_);
    } catch (...) {
        if (descriptor >= 0) ::close(descriptor);
        ::unlink(pending.c_str());
        throw;
    }
    ::unlink(pending.c_str());
}

std::vector<std::byte> VrsGenerationBlockStore::read_limited(
    const std::filesystem::path& path, const std::size_t maximum_bytes) const {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw CorruptVrsBlock("missing VRS candidate");
    std::vector<std::byte> data;
    data.resize(maximum_bytes + 1);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(stream.gcount()));
    return data;
}

VrsBlockSaveReceipt VrsGenerationBlockStore::save(const VrsBlockGeneration& generation) {
    VrsBlockSaveReceipt receipt;
    for (const auto& entry : generation.blocks) {
        if (const auto known = verified_[entry->digest].lock()) {
            if (known->digest != entry->digest || block_wire(*known->block) != block_wire(*entry->block))
                throw CorruptVrsBlock("hot block address collision");
            continue;
        }
        const auto wire = block_wire(*entry->block);
        publish(entry->digest + ".block", wire);
        verified_[entry->digest] = entry;
        ++receipt.block_publish_count;
        receipt.block_publish_bytes += wire.size();
    }
    const auto manifest = generation_manifest(generation);
    receipt.manifest_sha256 = sha256(manifest);
    receipt.manifest_bytes = manifest.size();
    const auto bytes = as_bytes(manifest);
    publish(receipt.manifest_sha256 + ".json", bytes);
    return receipt;
}

std::shared_ptr<const VrsBlockGeneration> VrsGenerationBlockStore::load(
    const std::string_view digest, const std::size_t maximum_raw_bytes,
    const std::size_t maximum_manifest_bytes) {
    require_identifier(digest);
    if (!maximum_manifest_bytes) reject("manifest capacity must be positive");
    const auto bytes = read_limited(root_ / (std::string(digest) + ".json"), maximum_manifest_bytes);
    if (bytes.size() > maximum_manifest_bytes || sha256(bytes) != digest)
        throw CorruptVrsBlock("manifest length or digest mismatch");
    try {
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::pmr::monotonic_buffer_resource memory;
        const auto manifest = transport::parse_json(text, memory);
        exact_keys(manifest, {"schema", "block_bytes", "raw_size", "blocks"});
        if (manifest.at("schema").string() != vrs_block_generation_schema)
            reject("block manifest schema changed");
        const auto block_bytes = uint_value(manifest.at("block_bytes"));
        const auto raw_size = uint_value(manifest.at("raw_size"));
        const auto& addresses = manifest.at("blocks");
        if (!block_bytes || block_bytes > maximum_vrs_block_bytes || raw_size > maximum_raw_bytes ||
            addresses.kind != Json::Kind::array ||
            addresses.values.size() != (raw_size + block_bytes - 1) / block_bytes)
            reject("manifest geometry or capacity mismatch");
        std::vector<std::shared_ptr<const StoredVrsBlock>> blocks;
        std::unordered_map<std::string, std::shared_ptr<const StoredVrsBlock>> loaded;
        blocks.reserve(addresses.values.size());
        for (const auto& address_value : addresses.values) {
            const auto address = std::string(address_value.string());
            require_identifier(address);
            auto entry = loaded[address];
            if (!entry) {
                const auto wire = read_limited(root_ / (address + ".block"),
                                               maximum_vrs_block_bytes + 1024);
                if (wire.size() > maximum_vrs_block_bytes + 1024 || sha256(wire) != address)
                    throw CorruptVrsBlock("stored block digest mismatch");
                const auto newline = std::find(wire.begin(), wire.end(), std::byte{'\n'});
                if (newline == wire.end()) throw CorruptVrsBlock("stored block header missing");
                const std::string header(reinterpret_cast<const char*>(wire.data()),
                                         static_cast<std::size_t>(newline - wire.begin()));
                std::pmr::monotonic_buffer_resource header_memory;
                const auto metadata = transport::parse_json(header, header_memory);
                exact_keys(metadata, {"codec", "raw_size", "crc32"});
                const auto codec_text = metadata.at("codec").string();
                const auto codec = codec_text == "raw" ? VrsBlockCodec::raw :
                                   codec_text == "zlib" ? VrsBlockCodec::zlib :
                                   throw std::invalid_argument("unsupported block codec");
                const auto stored_raw_size = uint_value(metadata.at("raw_size"));
                const auto stored_crc = uint_value(metadata.at("crc32"));
                if (stored_crc > std::numeric_limits<std::uint32_t>::max())
                    reject("invalid block checksum");
                std::vector<std::byte> payload(newline + 1, wire.end());
                const auto block = std::make_shared<const LosslessVrsBlock>(
                    codec, std::move(payload), stored_raw_size,
                    static_cast<std::uint32_t>(stored_crc));
                entry = std::make_shared<const StoredVrsBlock>(block, address);
                loaded[address] = entry;
            }
            blocks.push_back(entry);
        }
        auto result = std::make_shared<const VrsBlockGeneration>(
            std::move(blocks), block_bytes, raw_size);
        for (const auto& [address, entry] : loaded) verified_[address] = entry;
        return result;
    } catch (const CorruptVrsBlock&) { throw; }
    catch (const std::exception& error) {
        throw CorruptVrsBlock(std::string("invalid generation: ") + error.what());
    }
}

VrsArrayBlocks::VrsArrayBlocks(std::string dtype_value,
                               std::vector<std::size_t> shape_value,
                               std::shared_ptr<const VrsBlockGeneration> data_value)
    : dtype(std::move(dtype_value)), shape(std::move(shape_value)), data(std::move(data_value)) {
    const auto item = dtype_size(dtype);
    const auto count = element_count(shape);
    if (!data || multiply(count, item) != data->raw_size)
        reject("dtype/shape/byte layout mismatch");
}

std::size_t VrsArrayBlocks::item_size() const { return dtype_size(dtype); }
std::vector<std::byte> VrsArrayBlocks::restore() const { return data->read(); }

VrsArrayBlocks::Prepared VrsArrayBlocks::prepare(
    const NumericArrayView array, std::shared_ptr<const VrsArrayBlocks> parent,
    std::size_t block_bytes, const VrsBlockCodec codec) {
    const auto item = dtype_size(array.dtype);
    const auto count = element_count(array.shape);
    if (multiply(count, item) != array.bytes.size()) reject("dtype/shape/byte layout mismatch");
    if (!array.c_contiguous && array.shape.size() != 1)
        reject("multidimensional input must be C-contiguous");
    if (parent) {
        if (parent->dtype != array.dtype) reject("parent array dtype changed");
        block_bytes = parent->data->block_bytes;
    }
    if (block_bytes < item) reject("block is smaller than one element");
    block_bytes -= block_bytes % item;
    if (!block_bytes || block_bytes > maximum_vrs_block_bytes) reject("invalid block size");
    std::vector<std::shared_ptr<const StoredVrsBlock>> blocks;
    VrsArrayPrepareReceipt receipt{array.bytes.size(), 0, 0, false};
    for (std::size_t start = 0, index = 0; start < array.bytes.size(); start += block_bytes, ++index) {
        const auto raw = array.bytes.subspan(start, std::min(block_bytes, array.bytes.size() - start));
        std::shared_ptr<const StoredVrsBlock> prior;
        if (parent && index < parent->data->blocks.size()) prior = parent->data->blocks[index];
        if (prior) receipt.candidate_bytes_compared += raw.size();
        if (prior && prior->block->decode() == std::vector<std::byte>(raw.begin(), raw.end()))
            blocks.push_back(prior);
        else {
            blocks.push_back(StoredVrsBlock::build(raw, codec));
            receipt.raw_bytes_encoded += raw.size();
        }
    }
    auto generation = std::make_shared<const VrsBlockGeneration>(
        std::move(blocks), block_bytes, array.bytes.size());
    return {std::make_shared<const VrsArrayBlocks>(
                std::string(array.dtype), std::vector<std::size_t>(array.shape.begin(), array.shape.end()),
                std::move(generation)), receipt};
}

VrsArrayBlocks::Patched VrsArrayBlocks::patch_and_append(
    const std::shared_ptr<const VrsArrayBlocks>& self,
    const std::span<const std::size_t> indices,
    const NumericArrayView* values, const NumericArrayView* append,
    const VrsBlockCodec codec) {
    if (!self || self->shape.empty() || self->data->block_bytes % self->item_size())
        reject("row append requires element-aligned nonscalar layout");
    const auto count = element_count(self->shape);
    std::set<std::size_t> unique;
    for (const auto index : indices)
        if (index >= count || !unique.insert(index).second)
            reject("distinct in-range old element offsets required");

    const std::array<std::size_t, 1> empty_values_shape{0};
    const NumericArrayView empty_values{self->dtype, empty_values_shape, {}};
    const auto& actual_values = values ? *values : empty_values;
    if (actual_values.dtype != self->dtype || actual_values.shape.size() != 1 ||
        actual_values.shape[0] != indices.size() ||
        actual_values.bytes.size() != multiply(indices.size(), self->item_size()))
        reject("sparse values changed dtype/shape");

    std::vector<std::size_t> default_append_shape = self->shape;
    default_append_shape[0] = 0;
    const NumericArrayView empty_append{self->dtype, default_append_shape, {}};
    const auto& actual_append = append ? *append : empty_append;
    if (actual_append.dtype != self->dtype || actual_append.shape.size() != self->shape.size() ||
        !std::equal(actual_append.shape.begin() + 1, actual_append.shape.end(), self->shape.begin() + 1) ||
        actual_append.bytes.size() != multiply(element_count(actual_append.shape), self->item_size()))
        reject("appended rows changed dtype/shape");
    const auto appended_rows = actual_append.shape[0];
    if (appended_rows > std::numeric_limits<std::size_t>::max() - self->shape[0])
        throw std::length_error("array row count overflow");
    auto shape = self->shape;
    shape[0] += appended_rows;
    VrsArrayPatchReceipt receipt{actual_values.bytes.size() + actual_append.bytes.size(), 0, 0, false, true};
    if (indices.empty() && actual_append.bytes.empty()) {
        if (shape == self->shape) return {self, receipt};
        return {std::make_shared<const VrsArrayBlocks>(self->dtype, std::move(shape), self->data), receipt};
    }

    const auto size = self->data->block_bytes;
    std::map<std::size_t, std::vector<std::byte>> buffers;
    std::map<std::size_t, std::vector<std::byte>> originals;
    const auto block = [&](const std::size_t number) -> std::vector<std::byte>& {
        if (!buffers.contains(number)) {
            std::vector<std::byte> raw;
            if (number < self->data->blocks.size()) raw = self->data->blocks[number]->block->decode();
            receipt.old_payload_bytes_decoded += raw.size();
            originals[number] = raw;
            buffers[number] = std::move(raw);
        }
        return buffers[number];
    };
    const auto item = self->item_size();
    for (std::size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
        const auto offset = multiply(indices[ordinal], item);
        const auto number = offset / size;
        const auto local = offset % size;
        auto& target = block(number);
        std::copy_n(actual_values.bytes.begin() + static_cast<std::ptrdiff_t>(ordinal * item), item,
                    target.begin() + static_cast<std::ptrdiff_t>(local));
    }
    std::size_t position = 0;
    while (position < actual_append.bytes.size()) {
        const auto absolute = self->data->raw_size + position;
        const auto number = absolute / size;
        const auto local = absolute % size;
        const auto length = std::min(size - local, actual_append.bytes.size() - position);
        auto& target = block(number);
        if (target.size() < local + length) target.resize(local + length);
        std::copy_n(actual_append.bytes.begin() + static_cast<std::ptrdiff_t>(position), length,
                    target.begin() + static_cast<std::ptrdiff_t>(local));
        position += length;
    }
    auto blocks = self->data->blocks;
    for (auto& [number, raw] : buffers) {
        if (raw == originals[number]) continue;
        auto entry = StoredVrsBlock::build(raw, codec);
        receipt.raw_bytes_encoded += raw.size();
        if (number == blocks.size()) blocks.push_back(std::move(entry));
        else blocks[number] = std::move(entry);
    }
    auto generation = std::make_shared<const VrsBlockGeneration>(
        std::move(blocks), size, self->data->raw_size + actual_append.bytes.size());
    return {std::make_shared<const VrsArrayBlocks>(self->dtype, std::move(shape),
                                                   std::move(generation)), receipt};
}

VrsArrayBundle::VrsArrayBundle(
    std::map<std::string, std::shared_ptr<const VrsArrayBlocks>> arrays_value)
    : arrays(std::move(arrays_value)) {
    if (arrays.empty()) reject("invalid array bundle");
    for (const auto& [name, array] : arrays)
        if (name.empty() || !array) reject("invalid array bundle");
}

VrsArrayBundlePrepared VrsArrayBundle::prepare(
    const std::map<std::string, NumericArrayView>& source,
    const VrsArrayBundle* parent, const std::size_t block_bytes,
    const VrsBlockCodec codec) {
    if (source.empty()) reject("invalid array bundle");
    if (parent) {
        if (source.size() != parent->arrays.size())
            reject("array names changed; explicit schema migration required");
        for (const auto& [name, unused] : source) {
            (void)unused;
            if (!parent->arrays.contains(name))
                reject("array names changed; explicit schema migration required");
        }
    }
    std::map<std::string, std::shared_ptr<const VrsArrayBlocks>> prepared;
    std::map<std::string, VrsArrayPrepareReceipt> receipts;
    for (const auto& [name, array] : source) {
        const auto prior = parent ? parent->arrays.at(name) : nullptr;
        auto result = VrsArrayBlocks::prepare(array, prior, block_bytes, codec);
        receipts.emplace(name, result.receipt);
        prepared.emplace(name, std::move(result.array));
    }
    return {VrsArrayBundle(std::move(prepared)), std::move(receipts)};
}

VrsArrayBundleSaveReceipt VrsArrayBundle::save(VrsGenerationBlockStore& store) const {
    VrsArrayBundleSaveReceipt receipt;
    std::map<std::string, std::string> manifests;
    for (const auto& [name, array] : arrays) {
        auto saved = store.save(*array->data);
        manifests[name] = saved.manifest_sha256;
        receipt.arrays.emplace(name, std::move(saved));
    }
    std::string wire = "{\"arrays\":{";
    std::size_t ordinal = 0;
    for (const auto& [name, array] : arrays) {
        if (ordinal++) wire.push_back(',');
        wire += json_ascii(name) + ":{\"dtype\":\"" + array->dtype +
                "\",\"manifest_sha256\":\"" + manifests.at(name) + "\",\"shape\":[";
        for (std::size_t i = 0; i < array->shape.size(); ++i) {
            if (i) wire.push_back(',');
            wire += std::to_string(array->shape[i]);
        }
        wire += "]}";
    }
    wire += "},\"schema\":\"" + std::string(vrs_array_bundle_schema) + "\"}";
    receipt.bundle_sha256 = sha256(wire);
    receipt.bundle_bytes = wire.size();
    const auto bytes = as_bytes(wire);
    store.publish(receipt.bundle_sha256 + ".arrays.json", bytes);
    return receipt;
}

VrsArrayBundle VrsArrayBundle::load(
    VrsGenerationBlockStore& store, const std::string_view digest,
    const std::size_t maximum_raw_bytes, const std::size_t maximum_manifest_bytes) {
    require_identifier(digest);
    if (!maximum_manifest_bytes) reject("invalid cold capacity");
    const auto bytes = store.read_limited(
        store.root() / (std::string(digest) + ".arrays.json"), maximum_manifest_bytes);
    if (bytes.size() > maximum_manifest_bytes || sha256(bytes) != digest)
        throw CorruptVrsBlock("array manifest digest/size changed");
    try {
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::pmr::monotonic_buffer_resource memory;
        const auto manifest = transport::parse_json(text, memory);
        exact_keys(manifest, {"schema", "arrays"});
        if (manifest.at("schema").string() != vrs_array_bundle_schema)
            reject("array manifest schema changed");
        const auto& descriptors = manifest.at("arrays");
        if (descriptors.kind != Json::Kind::object || descriptors.keys.empty())
            reject("array manifest schema changed");
        struct Layout final { std::string dtype; std::vector<std::size_t> shape; std::size_t size; std::string address; };
        std::map<std::string, Layout> layouts;
        std::size_t total = 0;
        for (std::size_t i = 0; i < descriptors.keys.size(); ++i) {
            const auto& descriptor = descriptors.values[i];
            exact_keys(descriptor, {"dtype", "shape", "manifest_sha256"});
            auto dtype = std::string(descriptor.at("dtype").string());
            const auto item = dtype_size(dtype);
            auto shape = shape_value(descriptor.at("shape"));
            const auto size = multiply(element_count(shape), item);
            if (size > maximum_raw_bytes - total) reject("bundle exceeds total cold capacity");
            total += size;
            auto address = std::string(descriptor.at("manifest_sha256").string());
            require_identifier(address);
            layouts.emplace(std::string(descriptors.keys[i]),
                            Layout{std::move(dtype), std::move(shape), size, std::move(address)});
        }
        std::map<std::string, std::shared_ptr<const VrsArrayBlocks>> arrays;
        for (auto& [name, layout] : layouts) {
            auto generation = store.load(layout.address, layout.size, maximum_manifest_bytes);
            arrays.emplace(name, std::make_shared<const VrsArrayBlocks>(
                std::move(layout.dtype), std::move(layout.shape), std::move(generation)));
        }
        return VrsArrayBundle(std::move(arrays));
    } catch (const CorruptVrsBlock&) { throw; }
    catch (const std::exception& error) {
        throw CorruptVrsBlock(std::string("invalid typed bundle: ") + error.what());
    }
}

}  // namespace swegca::world
