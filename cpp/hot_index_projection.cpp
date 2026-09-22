#include "hot_index_projection.hpp"

#include "digest.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

constexpr std::string_view magic = "VRSHDR01";
constexpr std::string_view schema = "swegca-vrs2-hot-index-projection-v1";
constexpr std::size_t maximum_body_bytes = 16 * 1024 * 1024;
constexpr std::size_t frame_prefix_bytes = 16;
constexpr std::size_t checksum_bytes = 64;

// SWEGCA: src/swegca_vrs2/store.py@7536139:53-54
Json strings_json(const std::vector<std::string>& values) {
    Json::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return Json(std::move(result));
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:53-54
std::vector<std::string> strings_from_json(const Json& value) {
    std::vector<std::string> result;
    result.reserve(value.array().size());
    for (const auto& item : value.array()) result.push_back(item.string());
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:158-175
Json optional_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:158-175
std::optional<std::string> optional_from_json(const Json& value) {
    if (std::holds_alternative<std::nullptr_t>(value.data)) return std::nullopt;
    return value.string();
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:158-175
Json header_json(const HotIndexEpisodeHeader& header) {
    Json::Object value;
    value.emplace("episode_id", Json(header.episode_id));
    value.emplace("cues", strings_json(header.cues));
    value.emplace("source_addresses", strings_json(header.source_addresses));
    value.emplace("revision", Json(header.revision));
    value.emplace("verification_state", Json(header.verification_state));
    value.emplace("historical_outcomes", strings_json(header.historical_outcomes));
    value.emplace("proposition_id", optional_json(header.proposition_id));
    value.emplace("evidence_polarity", optional_json(header.evidence_polarity));
    value.emplace("supersedes", optional_json(header.supersedes));
    return Json(std::move(value));
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:158-175
HotIndexEpisodeHeader header_from_json(const Json& value) {
    if (value.object().size() != 9)
        throw std::runtime_error("hot_index_projection_header_invalid");
    return HotIndexEpisodeHeader{
        value.at("episode_id").string(), strings_from_json(value.at("cues")),
        strings_from_json(value.at("source_addresses")),
        value.at("revision").string(), value.at("verification_state").string(),
        strings_from_json(value.at("historical_outcomes")),
        optional_from_json(value.at("proposition_id")),
        optional_from_json(value.at("evidence_polarity")),
        optional_from_json(value.at("supersedes"))};
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
void put_u64(std::byte* destination, std::uint64_t value) {
    for (unsigned at = 0; at < 8; ++at)
        destination[at] = std::byte((value >> (8 * at)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::uint64_t get_u64(const std::byte* source) {
    std::uint64_t value = 0;
    for (unsigned at = 0; at < 8; ++at)
        value |= std::uint64_t(std::to_integer<unsigned char>(source[at])) << (8 * at);
    return value;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
HotIndexProjectionRow project_hot_index_addition(
    const HotIndexAppendPlan& plan, std::int64_t journal_sequence,
    std::string pair_snapshot_id) {
    if (!plan.added() || journal_sequence < 1 || pair_snapshot_id.empty())
        throw std::runtime_error("hot_index_projection_source_invalid");
    auto header = hot_index_header_from_episode(*plan.episode);
    if (header.episode_id != plan.identifier)
        throw std::runtime_error("hot_index_projection_source_invalid");
    return HotIndexProjectionRow{journal_sequence, std::move(pair_snapshot_id),
                                 std::move(header), plan.posting_cues};
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
std::vector<std::byte> encode_hot_index_projection(const HotIndexProjectionRow& row) {
    if (row.journal_sequence < 1 || row.pair_snapshot_id.empty() ||
        row.header.episode_id.empty() || row.posting_cues.empty())
        throw std::runtime_error("hot_index_projection_source_invalid");
    Json::Object value;
    value.emplace("schema", Json(std::string(schema)));
    value.emplace("journal_sequence", Json(row.journal_sequence));
    value.emplace("pair_snapshot_id", Json(row.pair_snapshot_id));
    value.emplace("header", header_json(row.header));
    value.emplace("posting_cues", strings_json(row.posting_cues));
    const auto body = Json(std::move(value)).canonical();
    if (body.size() > maximum_body_bytes)
        throw std::runtime_error("hot_index_projection_frame_too_large");
    const auto digest = sha256_hex(body);
    std::vector<std::byte> frame(frame_prefix_bytes + body.size() + checksum_bytes);
    std::memcpy(frame.data(), magic.data(), magic.size());
    put_u64(frame.data() + magic.size(), body.size());
    std::memcpy(frame.data() + frame_prefix_bytes, body.data(), body.size());
    std::memcpy(frame.data() + frame_prefix_bytes + body.size(),
                digest.data(), digest.size());
    return frame;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
HotIndexProjectionRow decode_hot_index_projection(std::span<const std::byte> frame) {
    if (frame.size() < frame_prefix_bytes + checksum_bytes ||
        std::memcmp(frame.data(), magic.data(), magic.size()) != 0)
        throw std::runtime_error("hot_index_projection_frame_invalid");
    const auto length = get_u64(frame.data() + magic.size());
    if (length > maximum_body_bytes ||
        frame.size() != frame_prefix_bytes + length + checksum_bytes)
        throw std::runtime_error("hot_index_projection_frame_invalid");
    const std::string_view body(
        reinterpret_cast<const char*>(frame.data() + frame_prefix_bytes),
        static_cast<std::size_t>(length));
    const auto digest = sha256_hex(body);
    if (std::memcmp(frame.data() + frame_prefix_bytes + length,
                    digest.data(), digest.size()) != 0)
        throw std::runtime_error("hot_index_projection_checksum_invalid");
    const auto value = Json::parse(body);
    if (value.object().size() != 5 || value.at("schema").string() != schema)
        throw std::runtime_error("hot_index_projection_frame_invalid");
    const auto sequence = value.at("journal_sequence").integer();
    auto pair = value.at("pair_snapshot_id").string();
    auto header = header_from_json(value.at("header"));
    auto postings = strings_from_json(value.at("posting_cues"));
    if (sequence < 1 || pair.empty() || header.episode_id.empty() || postings.empty())
        throw std::runtime_error("hot_index_projection_frame_invalid");
    return HotIndexProjectionRow{sequence, std::move(pair),
                                 std::move(header), std::move(postings)};
}

}  // namespace swegca::vrs
