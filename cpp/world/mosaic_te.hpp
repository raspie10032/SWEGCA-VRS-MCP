#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_te_source_sha256 =
    "cac6526f98e5d07133b32aef672de3acf21438c7f83793b81bab4a859836e630";

struct MosaicTEConfig final {
    std::size_t patch_size{4}, max_bytes{256}, model_dim{96}, conditioning_dim{128};
    std::size_t attention_heads{4}, ffn_dim{192}, local_layers{2};
    std::size_t slot_count{8}, recurrent_rounds{2};
    void validate() const;
};

struct MosaicTETensor final {
    std::vector<std::size_t> shape;
    std::vector<float> values;
    MosaicTETensor() = default;
    MosaicTETensor(std::vector<std::size_t>, std::vector<float>);
};

struct MosaicTEMask final {
    std::vector<std::size_t> shape;
    std::vector<std::uint8_t> values;
    MosaicTEMask() = default;
    MosaicTEMask(std::vector<std::size_t>, std::vector<std::uint8_t>);
};

struct MosaicTEByteIds final {
    std::size_t batch{}, bytes{};
    std::vector<std::int64_t> values;
    MosaicTEByteIds() = default;
    MosaicTEByteIds(std::size_t, std::size_t, std::vector<std::int64_t>);
};

struct MosaicTEOutput final {
    MosaicTETensor sequence_states;
    MosaicTETensor global_slots;
    MosaicTETensor pooled_state;
    MosaicTEMask attention_mask;
    std::vector<std::int64_t> byte_spans; // [batch, patches, 2]
};

class SharedSlotCell final {
public:
    explicit SharedSlotCell(const MosaicTEConfig&, std::uint64_t seed = 47);
    ~SharedSlotCell();
    SharedSlotCell(SharedSlotCell&&) noexcept;
    SharedSlotCell& operator=(SharedSlotCell&&) noexcept;
    SharedSlotCell(const SharedSlotCell&) = delete;
    SharedSlotCell& operator=(const SharedSlotCell&) = delete;
    [[nodiscard]] MosaicTETensor forward(const MosaicTETensor& slots,
        const MosaicTETensor& sequence, const MosaicTEMask& sequence_mask) const;
    [[nodiscard]] std::size_t parameter_count() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class MosaicTextEncoderProbe final {
public:
    explicit MosaicTextEncoderProbe(MosaicTEConfig, std::uint64_t seed = 47);
    ~MosaicTextEncoderProbe();
    MosaicTextEncoderProbe(MosaicTextEncoderProbe&&) noexcept;
    MosaicTextEncoderProbe& operator=(MosaicTextEncoderProbe&&) noexcept;
    MosaicTextEncoderProbe(const MosaicTextEncoderProbe&) = delete;
    MosaicTextEncoderProbe& operator=(const MosaicTextEncoderProbe&) = delete;
    [[nodiscard]] MosaicTEOutput forward(const MosaicTEByteIds&,
        std::span<const std::int64_t> byte_lengths,
        std::optional<std::size_t> rounds = std::nullopt) const;
    [[nodiscard]] MosaicTEOutput encode(const std::vector<std::string>& prompts,
        std::optional<std::size_t> rounds = std::nullopt) const;
    [[nodiscard]] const MosaicTEConfig& config() const noexcept;
    [[nodiscard]] std::size_t parameter_count() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] MosaicTEByteIds encode_mosaic_te_texts(
    const std::vector<std::string>&, std::size_t max_bytes, std::size_t patch_size);

// Dependency-free CPU counterpart of the Python torch.device probe.
[[nodiscard]] JsonValue::Object run_mosaic_te_probe(
    const MosaicTEConfig&, std::string_view device, std::size_t repeats);

} // namespace swegca::world
