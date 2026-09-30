#include "world/text_lm.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace swegca::world {
namespace {

std::int64_t integer(const JsonValue::Object& rows, const std::string_view key,
                     const std::int64_t fallback) {
    const auto found = rows.find(key);
    if (found == rows.end()) return fallback;
    const auto* value = std::get_if<std::int64_t>(&found->second.storage());
    if (!value || *value <= 0) throw std::invalid_argument("positive text config integer required");
    return *value;
}

double number(const JsonValue::Object& rows, const std::string_view key,
              const double fallback) {
    const auto found = rows.find(key);
    return found == rows.end() ? fallback : found->second.as_number();
}

std::uint64_t add(const std::uint64_t left, const std::uint64_t right) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        throw std::overflow_error("text model parameter count overflow");
    return left + right;
}

std::uint64_t mul(const std::uint64_t left, const std::uint64_t right) {
    if (left && right > std::numeric_limits<std::uint64_t>::max() / left)
        throw std::overflow_error("text model parameter count overflow");
    return left * right;
}

bool valid_utf8(const std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset]);
        std::size_t count = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2
            : (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 0;
        if (!count || offset + count > text.size()) return false;
        for (std::size_t index = 1; index < count; ++index)
            if ((static_cast<unsigned char>(text[offset + index]) & 0xc0) != 0x80) return false;
        offset += count;
    }
    return true;
}

}  // namespace

void MosaicTextConfig::validate() const {
    if (!patch_size || !byte_embedding_dim || !model_dim || !attention_heads || !ffn_dim ||
        !physical_layers || !workspace_slots || !retriever_dim || !operator_basis_count ||
        !operator_rank || !maximum_recurrent_depth)
        throw std::invalid_argument("model dimensions and counts must be positive");
    if (model_dim % attention_heads)
        throw std::invalid_argument("model_dim must be divisible by attention_heads");
    if (!(maximum_operator_update > 0 && maximum_operator_update <= 1))
        throw std::invalid_argument("maximum_operator_update must be in (0, 1]");
    if (!(dropout >= 0 && dropout < 1))
        throw std::invalid_argument("dropout must be in [0, 1)");
}

JsonValue::Object MosaicTextConfig::to_dict() const {
    return {{"schema_version", "mosaic-text-lm-config-v0"},
        {"patch_size", static_cast<std::int64_t>(patch_size)},
        {"byte_embedding_dim", static_cast<std::int64_t>(byte_embedding_dim)},
        {"model_dim", static_cast<std::int64_t>(model_dim)},
        {"attention_heads", static_cast<std::int64_t>(attention_heads)},
        {"ffn_dim", static_cast<std::int64_t>(ffn_dim)},
        {"physical_layers", static_cast<std::int64_t>(physical_layers)},
        {"workspace_slots", static_cast<std::int64_t>(workspace_slots)},
        {"retriever_dim", static_cast<std::int64_t>(retriever_dim)},
        {"operator_basis_count", static_cast<std::int64_t>(operator_basis_count)},
        {"operator_rank", static_cast<std::int64_t>(operator_rank)},
        {"max_recurrent_depth", static_cast<std::int64_t>(maximum_recurrent_depth)},
        {"maximum_operator_update", maximum_operator_update}, {"dropout", dropout}};
}

MosaicTextConfig MosaicTextConfig::from_dict(const JsonValue::Object& rows) {
    const auto schema = rows.find("schema_version");
    if (schema != rows.end() && schema->second.as_string() != "mosaic-text-lm-config-v0")
        throw std::invalid_argument("unsupported MOSAIC text config schema");
    MosaicTextConfig result{
        static_cast<std::size_t>(integer(rows, "patch_size", 8)),
        static_cast<std::size_t>(integer(rows, "byte_embedding_dim", 64)),
        static_cast<std::size_t>(integer(rows, "model_dim", 128)),
        static_cast<std::size_t>(integer(rows, "attention_heads", 4)),
        static_cast<std::size_t>(integer(rows, "ffn_dim", 512)),
        static_cast<std::size_t>(integer(rows, "physical_layers", 2)),
        static_cast<std::size_t>(integer(rows, "workspace_slots", 4)),
        static_cast<std::size_t>(integer(rows, "retriever_dim", 128)),
        static_cast<std::size_t>(integer(rows, "operator_basis_count", 16)),
        static_cast<std::size_t>(integer(rows, "operator_rank", 2)),
        static_cast<std::size_t>(integer(rows, "max_recurrent_depth", 4)),
        number(rows, "maximum_operator_update", 0.25), number(rows, "dropout", 0.0)};
    result.validate(); return result;
}

BytePatchCodec::BytePatchCodec(const std::size_t patch_size) : patch_size_(patch_size) {
    if (!patch_size_) throw std::invalid_argument("patch_size must be positive");
}

std::vector<std::int64_t> BytePatchCodec::encode(
    const std::string_view text, const bool add_bos, const bool add_eos) const {
    std::vector<std::int64_t> result;
    result.reserve(text.size() + static_cast<std::size_t>(add_bos) + static_cast<std::size_t>(add_eos));
    if (add_bos) result.push_back(mosaic_bos_id);
    for (const unsigned char byte : text) result.push_back(byte);
    if (add_eos) result.push_back(mosaic_eos_id);
    return result;
}

std::string BytePatchCodec::decode(
    const std::vector<std::int64_t>& ids, const bool replace_invalid_utf8) const {
    std::string result;
    for (const auto value : ids) {
        if (value == mosaic_eos_id) break;
        if (value == mosaic_pad_id || value == mosaic_bos_id) continue;
        if (value < 0 || value > 255) throw std::invalid_argument("invalid byte token");
        result.push_back(static_cast<char>(static_cast<unsigned char>(value)));
    }
    if (!valid_utf8(result)) {
        if (!replace_invalid_utf8) throw std::invalid_argument("invalid UTF-8 byte token sequence");
        return "\xef\xbf\xbd";
    }
    return result;
}

std::vector<std::vector<std::int64_t>> BytePatchCodec::pack(
    const std::vector<std::int64_t>& ids) const {
    if (ids.empty()) throw std::invalid_argument("cannot pack an empty token sequence");
    for (const auto value : ids) if (value < 0 || value >= mosaic_vocab_size)
        throw std::invalid_argument("token ids must be in the MOSAIC byte vocabulary");
    const auto rows = (ids.size() + patch_size_ - 1) / patch_size_;
    std::vector<std::vector<std::int64_t>> result(
        rows, std::vector<std::int64_t>(patch_size_, mosaic_pad_id));
    for (std::size_t index = 0; index < ids.size(); ++index)
        result[index / patch_size_][index % patch_size_] = ids[index];
    return result;
}

std::vector<std::int64_t> BytePatchCodec::unpack(
    const std::vector<std::vector<std::int64_t>>& patches) const {
    std::vector<std::int64_t> result;
    for (const auto& patch : patches) {
        if (patch.size() != patch_size_)
            throw std::invalid_argument("patches must have shape [patches, patch_size]");
        result.insert(result.end(), patch.begin(), patch.end());
    }
    while (!result.empty() && result.back() == mosaic_pad_id) result.pop_back();
    return result;
}

JsonValue::Object MosaicTextModelProfile::receipt(const MosaicTextConfig& config) const {
    return {{"schema_version", "mosaic-text-lm-profile-v0"}, {"config", config.to_dict()},
        {"parameter_count", JsonInteger{std::to_string(parameter_count)}},
        {"raw_weight_mib", JsonValue::Object{{"bf16", raw_weight_mib_bf16},
            {"fp32", raw_weight_mib_fp32}}},
        {"recurrent_depth_parameter_invariant", recurrent_depth_parameter_invariant}};
}

MosaicTextModelProfile profile_mosaic_text_model(const MosaicTextConfig& config) {
    config.validate();
    const auto p = static_cast<std::uint64_t>(config.patch_size);
    const auto e = static_cast<std::uint64_t>(config.byte_embedding_dim);
    const auto d = static_cast<std::uint64_t>(config.model_dim);
    const auto f = static_cast<std::uint64_t>(config.ffn_dim);
    std::uint64_t count = mul(mosaic_vocab_size, e);
    count = add(count, add(mul(d, mul(p, e)), d));
    count = add(count, mul(2, d));
    count = add(count, mul(3, d));
    count = add(count, mul(config.workspace_slots, d));
    count = add(count, d);
    count = add(count, add(mul(d, config.retriever_dim), d));
    count = add(count, mul(config.maximum_recurrent_depth, d));
    const auto transformer = add(add(mul(4, mul(d, d)), mul(2, mul(d, f))), add(mul(9, d), f));
    count = add(count, mul(config.physical_layers, transformer));
    count = add(count, mul(2, mul(mul(config.operator_basis_count, d), config.operator_rank)));
    count = add(count, add(add(mul(3, mul(d, e)), mul(3, mul(d, d))), mul(6, d)));
    count = add(count, mul(2, d));
    count = add(count, add(mul(mosaic_vocab_size, d), mosaic_vocab_size));
    constexpr double mib = 1024.0 * 1024.0;
    return {count, count * 2.0 / mib, count * 4.0 / mib, true};
}

}  // namespace swegca::world
