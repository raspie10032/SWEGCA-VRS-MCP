#include "world/resident_directory_archive.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/resident_observation_tree.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

constexpr std::uint32_t schema = 1;

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool text(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    });
}

LosslessBlockCodec codec(const std::uint8_t value) {
    if (value > static_cast<std::uint8_t>(LosslessBlockCodec::zstd))
        throw std::invalid_argument("unknown resident compression codec");
    return static_cast<LosslessBlockCodec>(value);
}

ExactFloatFormat float_format(const std::uint8_t value) {
    if (value > static_cast<std::uint8_t>(ExactFloatFormat::binary64))
        throw std::invalid_argument("unknown resident float format");
    return static_cast<ExactFloatFormat>(value);
}

struct Writer final {
    std::vector<std::byte> bytes;
    void u8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
    void raw(std::span<const std::byte> value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
    void string(std::string_view value) {
        u64(value.size());
        raw(std::as_bytes(std::span(value.data(), value.size())));
    }
    void strings(const std::vector<std::string>& values) {
        u64(values.size());
        for (const auto& value : values) string(value);
    }
};

struct Reader final {
    std::span<const std::byte> bytes;
    std::size_t position{};
    void need(std::size_t count) const {
        if (position > bytes.size() || count > bytes.size() - position)
            throw std::invalid_argument("truncated resident directory archive");
    }
    std::uint8_t u8() { need(1); return std::to_integer<std::uint8_t>(bytes[position++]); }
    std::uint32_t u32() {
        need(4); std::uint32_t value{};
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= std::to_integer<std::uint32_t>(bytes[position++]) << shift;
        return value;
    }
    std::uint64_t u64() {
        need(8); std::uint64_t value{};
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= std::to_integer<std::uint64_t>(bytes[position++]) << shift;
        return value;
    }
    std::size_t extent() {
        const auto value = u64();
        if (value > std::numeric_limits<std::size_t>::max())
            throw std::length_error("resident directory extent exceeds host range");
        return static_cast<std::size_t>(value);
    }
    std::span<const std::byte> raw(std::size_t count) {
        need(count); const auto result = bytes.subspan(position, count); position += count; return result;
    }
    std::string string() {
        const auto value = raw(extent());
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    std::vector<std::string> strings() {
        const auto count = extent();
        if (count > (bytes.size() - position) / sizeof(std::uint64_t))
            throw std::invalid_argument("resident string directory exceeds extent");
        std::vector<std::string> result; result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) result.push_back(string());
        return result;
    }
};

void write_stats(Writer& writer, const CompressedMemoryStats& value) {
    writer.u64(value.compressed_string_references);
    writer.u64(value.unique_compressed_strings);
    writer.u64(value.logical_compressed_string_utf8_bytes);
    writer.u64(value.unique_compressed_string_utf8_bytes);
    writer.u64(value.compressed_string_payload_bytes);
    writer.u64(value.packed_numeric_tuple_references);
    writer.u64(value.packed_numeric_values);
    writer.u64(value.packed_numeric_payload_bytes);
}

CompressedMemoryStats read_stats(Reader& reader) {
    return {reader.extent(), reader.extent(), reader.extent(), reader.extent(),
            reader.extent(), reader.extent(), reader.extent(), reader.extent()};
}

void write_blob(Writer& writer, const LosslessBlob& blob) {
    writer.u64(blob.block_bytes); writer.u64(blob.raw_size); writer.string(blob.content_sha256);
    writer.u64(blob.blocks.size());
    for (const auto& block : blob.blocks) {
        writer.u8(static_cast<std::uint8_t>(block->codec));
        writer.u64(block->raw_size); writer.u32(block->crc32);
        writer.u64(block->payload.size()); writer.raw(block->payload);
    }
}

std::shared_ptr<const LosslessBlob> read_blob(Reader& reader) {
    const auto block_bytes = reader.extent();
    const auto raw_size = reader.extent();
    auto content_digest = reader.string();
    const auto count = reader.extent();
    std::vector<std::shared_ptr<const LosslessBlock>> blocks; blocks.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto block_codec = codec(reader.u8());
        const auto block_raw_size = reader.extent();
        const auto crc = reader.u32();
        const auto payload = reader.raw(reader.extent());
        blocks.push_back(std::make_shared<const LosslessBlock>(block_codec,
            std::vector<std::byte>(payload.begin(), payload.end()), block_raw_size, crc));
    }
    return std::make_shared<const LosslessBlob>(std::move(blocks), block_bytes,
                                                raw_size, std::move(content_digest));
}

void write_observation(Writer& writer, const PackedResidentObservation& packed) {
    const auto bytes = packed.bytes(); writer.u64(bytes.size()); writer.raw(bytes);
    writer.u64(packed.externals().size());
    for (const auto& external : packed.externals()) {
        if (const auto* value = std::get_if<std::shared_ptr<const CompressedText>>(&external)) {
            writer.u8(1); write_blob(writer, *(*value)->blob);
        } else {
            const auto& value = std::get<std::shared_ptr<const PackedFloatTuple>>(external);
            writer.u8(2); writer.u64(value->count);
            writer.u8(static_cast<std::uint8_t>(value->format));
            write_blob(writer, *value->blob);
        }
    }
}

PackedResidentObservation read_observation(Reader& reader) {
    const auto raw = reader.raw(reader.extent());
    auto bytes = std::make_shared<const std::vector<std::byte>>(raw.begin(), raw.end());
    const auto count = reader.extent();
    std::vector<ResidentObservationExternal> externals; externals.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto kind = reader.u8();
        if (kind == 1) {
            externals.emplace_back(std::make_shared<const CompressedText>(
                CompressedText{read_blob(reader)}));
        } else if (kind == 2) {
            const auto values = reader.extent();
            const auto format = float_format(reader.u8());
            externals.emplace_back(std::make_shared<const PackedFloatTuple>(
                read_blob(reader), values, format));
        } else throw std::invalid_argument("unknown resident observation external");
    }
    return PackedResidentObservation(std::move(bytes), std::move(externals));
}

void write_step(Writer& writer, const MemoryStep& step, const CompressionPolicy& policy) {
    writer.string(step.phase); writer.strings(step.relations); writer.string(step.judgment);
    writer.string(step.outcome); writer.strings(step.evidence_refs);
    write_observation(writer, pack_observation(step.observation, policy));
}

MemoryStep read_step(Reader& reader, const bool materialize) {
    auto phase = reader.string(); auto relations = reader.strings();
    auto judgment = reader.string(); auto outcome = reader.string();
    auto evidence = reader.strings(); auto packed = read_observation(reader);
    return {std::move(phase), materialize ? packed.materialize() : JsonValue::Object{},
            std::move(relations), std::move(judgment), std::move(outcome), std::move(evidence)};
}

void write_episode(Writer& writer, const MemoryEpisode& episode,
                   const CompressionPolicy& policy) {
    writer.string(episode.episode_id); writer.strings(episode.cues);
    writer.strings(episode.source_addresses); writer.string(episode.revision);
    writer.string(episode.verification_state); writer.u64(episode.steps.size());
    for (const auto& step : episode.steps) write_step(writer, step, policy);
}

MemoryEpisode read_episode(Reader& reader, const bool materialize) {
    auto identifier = reader.string(); auto cues = reader.strings();
    auto addresses = reader.strings(); auto revision = reader.string();
    auto state = reader.string(); const auto count = reader.extent();
    std::vector<MemoryStep> steps; steps.reserve(count);
    for (std::size_t i = 0; i < count; ++i) steps.push_back(read_step(reader, materialize));
    return {std::move(identifier), std::move(cues), std::move(steps),
            std::move(addresses), std::move(revision), std::move(state)};
}

}  // namespace

std::string resident_archive_sha256(const std::span<const std::byte> archive) {
    const auto value = architecture::Sha256::of(archive);
    constexpr char digits[] = "0123456789abcdef";
    std::string result(value.size() * 2, '0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = std::to_integer<unsigned>(value[i]);
        result[2 * i] = digits[byte >> 4U]; result[2 * i + 1] = digits[byte & 15U];
    }
    return result;
}

std::vector<std::byte> dump_directory_leaf(const CompressedMemoryActivationIndex& source) {
    Writer writer;
    writer.raw(std::as_bytes(std::span(native_resident_directory_magic.data(),
                                       native_resident_directory_magic.size())));
    writer.u32(schema); writer.string(source.snapshot_id());
    writer.u8(static_cast<std::uint8_t>(source.policy.codec));
    writer.u32(static_cast<std::uint32_t>(source.policy.level));
    writer.u64(source.policy.block_bytes); writer.u64(source.policy.minimum_utf8_bytes);
    write_stats(writer, source.compression_stats);
    writer.u64(source.index->episodes_by_id.size());
    for (const auto& [unused, episode] : source.index->episodes_by_id) {
        static_cast<void>(unused); write_episode(writer, episode, source.policy);
    }
    writer.u64(source.index->postings_by_cue.size());
    for (const auto& [cue, identifiers] : source.index->postings_by_cue) {
        writer.string(cue); writer.strings(identifiers);
    }
    return std::move(writer.bytes);
}

ResidentDirectoryMemoryIndex::ResidentDirectoryMemoryIndex(
    std::shared_ptr<const std::vector<std::byte>> archive,
    std::string snapshot, CompressionPolicy policy_value, CompressedMemoryStats stats,
    std::map<std::string, std::size_t, std::less<>> offsets,
    std::map<std::string, std::vector<std::string>, std::less<>> postings,
    std::map<std::string, std::size_t, std::less<>> outcomes)
    : policy(policy_value), compression_stats(stats), archive_(std::move(archive)),
      snapshot_id_(std::move(snapshot)), episode_offsets_(std::move(offsets)),
      postings_(std::move(postings)), outcome_counts_(std::move(outcomes)) {}

std::string_view ResidentDirectoryMemoryIndex::snapshot_id() const noexcept { return snapshot_id_; }
std::size_t ResidentDirectoryMemoryIndex::episode_count() const noexcept { return episode_offsets_.size(); }
const std::map<std::string, std::size_t, std::less<>>&
ResidentDirectoryMemoryIndex::outcome_counts() const noexcept { return outcome_counts_; }
bool ResidentDirectoryMemoryIndex::contains_episode(const std::string_view id) const {
    return episode_offsets_.contains(id);
}

const MemoryEpisode& ResidentDirectoryMemoryIndex::episode(const std::string_view id) const {
    {
        std::lock_guard lock(cache_mutex_);
        if (const auto found = cache_.find(id); found != cache_.end()) return *found->second;
    }
    const auto offset = episode_offsets_.find(id);
    if (offset == episode_offsets_.end()) throw std::out_of_range("resident episode unavailable");
    Reader reader{*archive_, offset->second};
    auto value = std::make_shared<const MemoryEpisode>(read_episode(reader, true));
    if (value->episode_id != id) throw std::invalid_argument("resident episode address differs");
    std::lock_guard lock(cache_mutex_);
    return *cache_.emplace(value->episode_id, std::move(value)).first->second;
}

std::vector<std::string> ResidentDirectoryMemoryIndex::episode_ids_for_cue(
    const std::string_view cue) const {
    const auto found = postings_.find(cue);
    return found == postings_.end() ? std::vector<std::string>{} : found->second;
}

std::vector<std::string> ResidentDirectoryMemoryIndex::iter_episode_ids() const {
    std::vector<std::string> result; result.reserve(episode_offsets_.size());
    for (const auto& [identifier, unused] : episode_offsets_) {
        static_cast<void>(unused); result.push_back(identifier);
    }
    return result;
}

std::shared_ptr<const ResidentDirectoryMemoryIndex> load_directory_leaf(
    const std::span<const std::byte> archive, const std::string_view expected_sha256,
    const std::string_view expected_snapshot_id, const std::size_t maximum_bytes) {
    if (!digest_id(expected_sha256) || !digest_id(expected_snapshot_id))
        throw std::invalid_argument("external SHA256 seal required");
    if (archive.size() > maximum_bytes)
        throw std::invalid_argument("resident directory exceeds cold capacity");
    if (resident_archive_sha256(archive) != expected_sha256)
        throw std::invalid_argument("resident directory seal differs");
    auto owned = std::make_shared<const std::vector<std::byte>>(archive.begin(), archive.end());
    Reader reader{*owned};
    const auto magic = reader.raw(native_resident_directory_magic.size());
    if (!std::ranges::equal(magic, std::as_bytes(std::span(
            native_resident_directory_magic.data(), native_resident_directory_magic.size()))) ||
        reader.u32() != schema)
        throw std::invalid_argument("invalid resident directory");
    auto snapshot = reader.string();
    if (snapshot != expected_snapshot_id)
        throw std::invalid_argument("resident directory schema or identity differs");
    CompressionPolicy policy{codec(reader.u8()), static_cast<int>(reader.u32()),
                             reader.extent(), reader.extent()};
    policy.validate();
    auto stats = read_stats(reader);
    const auto episode_count = reader.extent();
    std::map<std::string, std::size_t, std::less<>> offsets;
    std::map<std::string, std::size_t, std::less<>> outcomes;
    for (const auto& outcome : memory_outcomes) outcomes.emplace(outcome, 0);
    for (std::size_t i = 0; i < episode_count; ++i) {
        const auto offset = reader.position;
        auto episode = read_episode(reader, false);
        if (!offsets.emplace(episode.episode_id, offset).second)
            throw std::invalid_argument("duplicate resident episode address");
        for (const auto& step : episode.steps) ++outcomes.at(step.outcome);
    }
    const auto posting_count = reader.extent();
    std::map<std::string, std::vector<std::string>, std::less<>> postings;
    for (std::size_t i = 0; i < posting_count; ++i) {
        auto cue = reader.string(); auto identifiers = reader.strings();
        if (identifiers.empty() || !std::ranges::is_sorted(identifiers) ||
            std::ranges::adjacent_find(identifiers) != identifiers.end() ||
            std::ranges::any_of(identifiers, [&](const auto& id) { return !offsets.contains(id); }) ||
            !postings.emplace(std::move(cue), std::move(identifiers)).second)
            throw std::invalid_argument("invalid resident posting directory");
    }
    if (reader.position != owned->size())
        throw std::invalid_argument("unclaimed resident directory bytes");
    return std::shared_ptr<const ResidentDirectoryMemoryIndex>(
        new ResidentDirectoryMemoryIndex(std::move(owned), std::move(snapshot), policy,
            stats, std::move(offsets), std::move(postings), std::move(outcomes)));
}

}  // namespace swegca::world
