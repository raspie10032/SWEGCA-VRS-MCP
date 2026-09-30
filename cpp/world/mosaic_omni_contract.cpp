#include "world/mosaic_omni.hpp"

#include "transport/json.hpp"

#include <fstream>
#include <memory_resource>
#include <regex>
#include <stdexcept>

namespace swegca::world {
namespace {

std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void utf8(std::string& output, const char32_t value) {
    if (value <= 0x7f) output.push_back(static_cast<char>(value));
    else if (value <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

std::string log_text(const std::optional<std::filesystem::path>& path) {
    if (!path || !std::filesystem::is_regular_file(*path)) return {};
    const auto raw = bytes(*path);
    if (raw.size() < 2 || !((static_cast<unsigned char>(raw[0]) == 0xff &&
        static_cast<unsigned char>(raw[1]) == 0xfe) ||
        (static_cast<unsigned char>(raw[0]) == 0xfe && static_cast<unsigned char>(raw[1]) == 0xff)))
        return raw;
    const bool little = static_cast<unsigned char>(raw[0]) == 0xff;
    std::string result;
    for (std::size_t offset = 2; offset + 1 < raw.size(); offset += 2) {
        const auto left = static_cast<unsigned char>(raw[offset]);
        const auto right = static_cast<unsigned char>(raw[offset + 1]);
        const auto unit = static_cast<char32_t>(little ? left | (right << 8) : (left << 8) | right);
        utf8(result, unit);
    }
    return result;
}

JsonValue convert(const transport::Json& value) {
    switch (value.kind) {
    case transport::Json::Kind::null: return nullptr;
    case transport::Json::Kind::boolean: return value.scalar == "true";
    case transport::Json::Kind::number: {
        const std::string text(value.scalar);
        if (text.find_first_of(".eE") == std::string::npos) {
            try { return static_cast<std::int64_t>(std::stoll(text)); }
            catch (const std::exception&) { return JsonInteger{text}; }
        }
        return std::stod(text);
    }
    case transport::Json::Kind::string: return std::string(value.scalar);
    case transport::Json::Kind::array: {
        JsonValue::Array result; result.reserve(value.values.size());
        for (const auto& item : value.values) result.push_back(convert(item));
        return result;
    }
    case transport::Json::Kind::object: {
        JsonValue::Object result;
        for (std::size_t index = 0; index < value.keys.size(); ++index)
            result.emplace(std::string(value.keys[index]), convert(value.values[index]));
        return result;
    }
    }
    throw std::logic_error("unknown JSON kind");
}

JsonValue parse_file(const std::optional<std::filesystem::path>& path) {
    if (!path || !std::filesystem::is_regular_file(*path)) return JsonValue::Object{};
    const auto raw = bytes(*path); std::pmr::monotonic_buffer_resource memory;
    return convert(transport::parse_json(raw, memory));
}

const JsonValue* optional(const JsonValue::Object& rows, const std::string_view key) {
    const auto found = rows.find(key); return found == rows.end() ? nullptr : &found->second;
}

JsonValue field(const JsonValue::Object& rows, const std::string_view key) {
    const auto* value = optional(rows, key); return value ? *value : JsonValue(nullptr);
}

JsonValue asset(const std::optional<std::filesystem::path>& path) {
    if (!path) return nullptr;
    const auto exists = std::filesystem::is_regular_file(*path);
    return JsonValue::Object{{"path", path->generic_string()}, {"exists", exists},
        {"bytes", exists ? JsonValue(JsonInteger{std::to_string(std::filesystem::file_size(*path))})
                          : JsonValue(nullptr)}};
}

std::optional<std::uint64_t> existing_bytes(const JsonValue::Object& assets,
    const std::initializer_list<std::string_view> names) {
    std::uint64_t result = 0;
    for (const auto name : names) {
        const auto& row = assets.at(name).as_object();
        const auto* exists = std::get_if<bool>(&row.at("exists").storage());
        if (!exists || !*exists) return std::nullopt;
        const auto& stored = row.at("bytes").storage();
        if (const auto* value = std::get_if<JsonInteger>(&stored)) result += std::stoull(value->value);
        else if (const auto* value = std::get_if<std::int64_t>(&stored)) result += *value;
        else return std::nullopt;
    }
    return result;
}

JsonValue maybe_integer(const std::optional<std::uint64_t> value) {
    return value ? JsonValue(JsonInteger{std::to_string(*value)}) : JsonValue(nullptr);
}

}  // namespace

JsonValue::Object inspect_omni_local_contract(const OmniLocalContractPaths& paths) {
    const auto config_value = parse_file(paths.gemma_config);
    const auto processor_value = parse_file(paths.gemma_processor);
    const auto& config = config_value.as_object(); const auto& processor = processor_value.as_object();
    JsonValue text_hidden(nullptr), video_frames(nullptr);
    if (const auto* row = optional(config, "text_config")) text_hidden = field(row->as_object(), "hidden_size");
    if (const auto* row = optional(processor, "video_processor")) video_frames = field(row->as_object(), "num_frames");
    const auto log = log_text(paths.raw_image_smoke_log);
    std::smatch match; const std::regex hidden(R"(image=\((\d+),\s*(\d+)\))");
    JsonValue shape(nullptr);
    if (std::regex_search(log, match, hidden))
        shape = JsonValue::Array{static_cast<std::int64_t>(std::stoll(match[1].str())),
                                 static_cast<std::int64_t>(std::stoll(match[2].str()))};
    JsonValue::Object assets{{"anima_q4", asset(paths.anima_q4)},
        {"anima_bf16", asset(paths.anima_bf16)}, {"anima_vae", asset(paths.anima_vae)},
        {"anima_text_encoder", asset(paths.anima_text_encoder)}, {"gemma_q4", asset(paths.gemma_q4)}};
    if (assets.at("gemma_q4").is_object())
        const_cast<JsonValue::Object&>(assets.at("gemma_q4").as_object()).emplace(
            "usage_boundary", "file-size evidence only; not asserted as the valid unified raw-image runtime");
    const auto edge = existing_bytes(assets, {"anima_q4", "anima_vae"});
    const auto current = existing_bytes(assets, {"anima_q4", "anima_vae", "anima_text_encoder"});
    const auto co_resident = existing_bytes(assets, {"anima_q4", "anima_vae", "gemma_q4"});
    constexpr std::uint64_t four_gib = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    return {{"gemma_unified", JsonValue::Object{{"architectures", field(config, "architectures")},
                {"model_type", field(config, "model_type")}, {"text_hidden_size", text_hidden},
                {"processor_class", field(processor, "processor_class")},
                {"image_seq_length", field(processor, "image_seq_length")},
                {"video_input_frames", video_frames}, {"audio_seq_length", field(processor, "audio_seq_length")}}},
        {"raw_image_hidden_smoke", JsonValue::Object{
            {"log", paths.raw_image_smoke_log ? JsonValue(paths.raw_image_smoke_log->generic_string()) : JsonValue(nullptr)},
            {"exists", !log.empty()}, {"hidden_shape", shape},
            {"int2_weight_path_observed", log.find("\"quanto_weights\": \"int2\"") != std::string::npos},
            {"direct_raw_image_mode_observed", log.find("\"cache_mode\": \"gemma4_12b_direct_raw_image_hidden\"") != std::string::npos}}},
        {"assets", assets}, {"weight_file_floors", JsonValue::Object{
            {"edge_renderer_anima_q4_plus_vae_bytes", maybe_integer(edge)},
            {"current_anima_q4_vae_te_bytes", maybe_integer(current)},
            {"anima_q4_vae_plus_gemma_q4_bytes", maybe_integer(co_resident)},
            {"four_gib_bytes", JsonInteger{std::to_string(four_gib)}},
            {"gemma12b_co_resident_fits_4gib_by_files_only", co_resident
                ? JsonValue(*co_resident <= four_gib) : JsonValue(nullptr)},
            {"note", "file-size sums exclude activations, allocator overhead, runtime code, and operating-system memory"}}}};
}

}  // namespace swegca::world
