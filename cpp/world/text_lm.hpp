#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::int64_t mosaic_pad_id = 256;
inline constexpr std::int64_t mosaic_bos_id = 257;
inline constexpr std::int64_t mosaic_eos_id = 258;
inline constexpr std::int64_t mosaic_vocab_size = 259;
inline constexpr std::int64_t mosaic_ignore_index = -100;

struct MosaicTextConfig final {
    std::size_t patch_size{8};
    std::size_t byte_embedding_dim{64};
    std::size_t model_dim{128};
    std::size_t attention_heads{4};
    std::size_t ffn_dim{512};
    std::size_t physical_layers{2};
    std::size_t workspace_slots{4};
    std::size_t retriever_dim{128};
    std::size_t operator_basis_count{16};
    std::size_t operator_rank{2};
    std::size_t maximum_recurrent_depth{4};
    double maximum_operator_update{0.25};
    double dropout{};

    void validate() const;
    [[nodiscard]] JsonValue::Object to_dict() const;
    [[nodiscard]] static MosaicTextConfig from_dict(const JsonValue::Object& values);
};

class BytePatchCodec final {
public:
    explicit BytePatchCodec(std::size_t patch_size = 8);
    [[nodiscard]] std::vector<std::int64_t> encode(
        std::string_view text, bool add_bos = true, bool add_eos = true) const;
    [[nodiscard]] std::string decode(
        const std::vector<std::int64_t>& ids, bool replace_invalid_utf8 = false) const;
    [[nodiscard]] std::vector<std::vector<std::int64_t>> pack(
        const std::vector<std::int64_t>& ids) const;
    [[nodiscard]] std::vector<std::int64_t> unpack(
        const std::vector<std::vector<std::int64_t>>& patches) const;
    [[nodiscard]] std::size_t patch_size() const noexcept { return patch_size_; }
private:
    std::size_t patch_size_;
};

struct MosaicTextModelProfile final {
    std::uint64_t parameter_count{};
    double raw_weight_mib_bf16{};
    double raw_weight_mib_fp32{};
    bool recurrent_depth_parameter_invariant{true};
    [[nodiscard]] JsonValue::Object receipt(const MosaicTextConfig& config) const;
};

[[nodiscard]] MosaicTextModelProfile profile_mosaic_text_model(
    const MosaicTextConfig& config);

}  // namespace swegca::world
