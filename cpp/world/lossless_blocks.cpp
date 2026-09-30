#include "world/lossless_blocks.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include <zlib.h>
#include <zstd.h>

namespace swegca::world {
namespace {

using architecture::Sha256;

[[nodiscard]] std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

[[nodiscard]] std::string digest(const std::span<const std::byte> data) {
    return hex(Sha256::of(data));
}

[[nodiscard]] bool digest_text(const std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](const char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

[[nodiscard]] std::uint32_t checksum(const std::span<const std::byte> raw) {
    auto value = ::crc32(0L, Z_NULL, 0);
    std::size_t offset = 0;
    while (offset != raw.size()) {
        const auto length = static_cast<uInt>(
            std::min<std::size_t>(raw.size() - offset,
                                  std::numeric_limits<uInt>::max()));
        value = ::crc32(value,
            reinterpret_cast<const Bytef*>(raw.data() + offset), length);
        offset += length;
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::vector<std::byte> encode_zlib(
    const std::span<const std::byte> raw, const int level) {
    auto size = ::compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::byte> result(size);
    if (::compress2(reinterpret_cast<Bytef*>(result.data()), &size,
                    reinterpret_cast<const Bytef*>(raw.data()),
                    static_cast<uLong>(raw.size()), level) != Z_OK)
        throw std::runtime_error("zlib compression failed");
    result.resize(size);
    return result;
}

[[nodiscard]] std::vector<std::byte> encode_zstd(
    const std::span<const std::byte> raw, const int level) {
    auto* context = ZSTD_createCCtx();
    if (!context) throw std::runtime_error("zstd compression context allocation failed");
    const auto release = [&] { ZSTD_freeCCtx(context); };
    auto status = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(status))
        status = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(status)) {
        release();
        throw std::runtime_error("zstd compression parameter rejected");
    }
    std::vector<std::byte> result(ZSTD_compressBound(raw.size()));
    status = ZSTD_compress2(context, result.data(), result.size(), raw.data(), raw.size());
    release();
    if (ZSTD_isError(status)) throw std::runtime_error("zstd compression failed");
    result.resize(status);
    return result;
}

[[nodiscard]] std::shared_ptr<const LosslessBlock> make_block(
    const std::span<const std::byte> raw, const LosslessBlockCodec requested,
    const int level) {
    std::vector<std::byte> encoded;
    if (requested == LosslessBlockCodec::zlib) encoded = encode_zlib(raw, level);
    else if (requested == LosslessBlockCodec::zstd) encoded = encode_zstd(raw, level);
    else encoded.assign(raw.begin(), raw.end());
    const bool compressed = encoded.size() < raw.size();
    auto block = std::make_shared<const LosslessBlock>(
        compressed ? requested : LosslessBlockCodec::raw,
        compressed ? std::move(encoded)
                   : std::vector<std::byte>(raw.begin(), raw.end()),
        raw.size(), checksum(raw));
    if (block->decode() != std::vector<std::byte>(raw.begin(), raw.end()))
        throw CorruptLosslessBlock("build roundtrip mismatch");
    return block;
}

}  // namespace

LosslessBlock::LosslessBlock(
    const LosslessBlockCodec codec_value, std::vector<std::byte> payload_value,
    const std::size_t raw_size_value, const std::uint32_t crc32_value)
    : codec(codec_value), payload(std::move(payload_value)), raw_size(raw_size_value),
      crc32(crc32_value) {
    if (!raw_size || raw_size > maximum_lossless_block_bytes ||
        payload.size() > raw_size ||
        (codec == LosslessBlockCodec::raw && payload.size() != raw_size))
        throw std::invalid_argument("invalid lossless block layout");
}

std::string_view LosslessBlock::codec_name() const noexcept {
    switch (codec) {
    case LosslessBlockCodec::raw: return "raw";
    case LosslessBlockCodec::zlib: return "zlib";
    case LosslessBlockCodec::zstd: return "zstd";
    }
    return {};
}

std::vector<std::byte> LosslessBlock::decode() const {
    std::vector<std::byte> raw(raw_size);
    if (codec == LosslessBlockCodec::raw) raw = payload;
    else if (codec == LosslessBlockCodec::zlib) {
        z_stream stream{};
        if (::inflateInit(&stream) != Z_OK)
            throw CorruptLosslessBlock("invalid compressed frame");
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(payload.data()));
        stream.avail_in = static_cast<uInt>(payload.size());
        stream.next_out = reinterpret_cast<Bytef*>(raw.data());
        stream.avail_out = static_cast<uInt>(raw.size());
        const auto status = ::inflate(&stream, Z_FINISH);
        const bool valid = status == Z_STREAM_END && stream.total_out == raw_size &&
                           stream.avail_in == 0;
        ::inflateEnd(&stream);
        if (!valid)
            throw CorruptLosslessBlock("incomplete, trailing or oversized zlib frame");
    } else {
        const auto frame_size = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
        const auto content_size = ZSTD_getFrameContentSize(payload.data(), payload.size());
        if (ZSTD_isError(frame_size) || frame_size != payload.size() ||
            (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size != raw_size))
            throw CorruptLosslessBlock("incomplete, trailing or oversized zstd frame");
        auto* context = ZSTD_createDCtx();
        if (!context) throw std::runtime_error("zstd decompression context allocation failed");
        auto status = ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, 23);
        if (!ZSTD_isError(status))
            status = ZSTD_decompressDCtx(context, raw.data(), raw.size(),
                                         payload.data(), payload.size());
        ZSTD_freeDCtx(context);
        if (ZSTD_isError(status) || status != raw_size)
            throw CorruptLosslessBlock("invalid compressed frame");
    }
    if (raw.size() != raw_size || checksum(raw) != crc32)
        throw CorruptLosslessBlock("block length or checksum mismatch");
    return raw;
}

LosslessBlob::LosslessBlob(
    std::vector<std::shared_ptr<const LosslessBlock>> block_values,
    const std::size_t block_bytes_value, const std::size_t raw_size_value,
    std::string content_digest)
    : blocks(std::move(block_values)), block_bytes(block_bytes_value),
      raw_size(raw_size_value), content_sha256(std::move(content_digest)) {
    if (!block_bytes || block_bytes > maximum_lossless_block_bytes)
        throw std::invalid_argument("block_bytes outside bounded range");
    const auto expected = (raw_size + block_bytes - 1) / block_bytes;
    if (blocks.size() != expected)
        throw std::invalid_argument("block count mismatch");
    for (std::size_t index = 0; index != blocks.size(); ++index) {
        if (!blocks[index] || blocks[index]->raw_size !=
            std::min(block_bytes, raw_size - index * block_bytes))
            throw std::invalid_argument("block layout mismatch");
    }
    if (!digest_text(content_sha256))
        throw std::invalid_argument("invalid content digest");
}

std::shared_ptr<const LosslessBlob> LosslessBlob::build(
    const std::span<const std::byte> raw, const LosslessBlockCodec codec,
    const int level, const std::size_t block_bytes) {
    if (!block_bytes || block_bytes > maximum_lossless_block_bytes)
        throw std::invalid_argument("block_bytes outside bounded range");
    if (level < 1 || level > 9)
        throw std::invalid_argument("diagnostic compression level must be 1..9");
    std::vector<std::shared_ptr<const LosslessBlock>> blocks;
    for (std::size_t start = 0; start < raw.size(); start += block_bytes) {
        const auto length = std::min(block_bytes, raw.size() - start);
        blocks.push_back(make_block(raw.subspan(start, length), codec, level));
    }
    return std::make_shared<const LosslessBlob>(
        std::move(blocks), block_bytes, raw.size(), digest(raw));
}

LosslessReadReceipt LosslessBlob::read(
    const std::size_t start, const std::optional<std::size_t> stop_value) const {
    const auto stop = stop_value.value_or(raw_size);
    if (start > stop || stop > raw_size)
        throw std::invalid_argument("byte range outside blob");
    if (start == stop) return {};
    const auto first = start / block_bytes;
    const auto last = (stop - 1) / block_bytes;
    LosslessReadReceipt receipt;
    receipt.data.reserve(stop - start);
    receipt.block_indices.reserve(last - first + 1);
    for (std::size_t index = first; index <= last; ++index) {
        const auto raw = blocks[index]->decode();
        receipt.decoded_bytes += raw.size();
        receipt.block_indices.push_back(index);
        const auto base = index * block_bytes;
        const auto left = std::max(start, base) - base;
        const auto right = std::min(stop, base + raw.size()) - base;
        receipt.data.insert(receipt.data.end(), raw.begin() + left, raw.begin() + right);
    }
    return receipt;
}

void LosslessBlob::verify_cold() const {
    Sha256 value;
    for (const auto& block : blocks) value.update(block->decode());
    if (hex(value.finish()) != content_sha256)
        throw CorruptLosslessBlock("content digest mismatch");
}

std::size_t LosslessBlob::stored_payload_bytes() const noexcept {
    std::size_t result = 0;
    for (const auto& block : blocks) result += block->payload.size();
    return result;
}

std::size_t LosslessBlob::resident_size_estimate() const noexcept {
    std::size_t result = sizeof(*this) + content_sha256.capacity() +
                         blocks.capacity() * sizeof(blocks.front());
    for (const auto& block : blocks)
        result += sizeof(*block) + block->payload.capacity();
    return result;
}

ResidentRecordStore::ResidentRecordStore(
    std::map<std::string, std::shared_ptr<const LosslessBlob>, std::less<>> values)
    : records(std::move(values)) {
    for (const auto& [address, blob] : records)
        if (address.empty() || !blob)
            throw std::invalid_argument("invalid record address or blob");
    using RecordEntry =
        std::pair<const std::string, std::shared_ptr<const LosslessBlob>>;
    mapping_size_ = records.size() * sizeof(RecordEntry);
}

ResidentRecordStore ResidentRecordStore::build(
    std::vector<Record> values, const LosslessBlockCodec codec,
    const int level, const std::size_t block_bytes) {
    std::map<std::string, std::shared_ptr<const LosslessBlob>, std::less<>> table;
    std::map<std::string, std::shared_ptr<const LosslessBlob>, std::less<>> shared;
    for (auto& [address, raw] : values) {
        if (address.empty())
            throw std::invalid_argument("record address must be a nonempty string");
        if (table.contains(address))
            throw std::invalid_argument(
                "duplicate address; do not silently overwrite experience");
        const auto key = digest(raw);
        auto found = shared.find(key);
        std::shared_ptr<const LosslessBlob> blob;
        if (found == shared.end()) {
            blob = LosslessBlob::build(raw, codec, level, block_bytes);
            shared.emplace(key, blob);
        } else {
            blob = found->second;
            if (blob->read().data != raw)
                throw CorruptLosslessBlock("content hash collision");
        }
        table.emplace(std::move(address), std::move(blob));
    }
    return ResidentRecordStore(std::move(table));
}

LosslessReadReceipt ResidentRecordStore::read(
    const std::string_view address, const std::size_t start,
    const std::optional<std::size_t> stop) const {
    return records.at(address)->read(start, stop);
}

ResidentRecordStats ResidentRecordStore::stats() const noexcept {
    ResidentRecordStats result;
    result.address_count = records.size();
    std::map<const LosslessBlob*, std::shared_ptr<const LosslessBlob>> unique;
    for (const auto& [address, blob] : records) {
        unique.emplace(blob.get(), blob);
        result.logical_raw_bytes += blob->raw_size;
        result.address_table_estimate_bytes += address.capacity();
    }
    result.address_table_estimate_bytes += mapping_size_ + sizeof(records);
    result.unique_blob_count = unique.size();
    for (const auto& [unused, blob] : unique) {
        static_cast<void>(unused);
        result.unique_raw_bytes += blob->raw_size;
        result.stored_payload_bytes += blob->stored_payload_bytes();
        result.resident_estimate_bytes += blob->resident_size_estimate();
    }
    result.resident_estimate_bytes += sizeof(*this) + result.address_table_estimate_bytes;
    return result;
}

}  // namespace swegca::world
