#pragma once

#include "world/lossless_blocks.hpp"
#include "world/cognitive_state.hpp"
#include "world/temporal_axis_concept.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view experience_atoms_source_sha256 =
    "39ffd385f5909f5b1aa5ce3f44014da69d5203e835fcd8499eced748d116a23d";

[[nodiscard]] std::string experience_atom_identifier(const JsonValue& value);

struct AtomParent final {
    std::string parent_experience_id;
    std::string source_address;
    std::string revision;
    std::vector<std::string> provenance;
    std::string outcome;
    std::string modality;
    std::optional<TimePoint> observed_at;
    std::string representation{"canonical_source"};
    AtomParent(std::string parent_experience_id, std::string source_address,
               std::string revision, std::vector<std::string> provenance,
               std::string outcome, std::string modality,
               std::optional<TimePoint> observed_at = std::nullopt,
               std::string representation = "canonical_source");
    friend bool operator==(const AtomParent&, const AtomParent&) = default;
};

struct ExperienceAtom final {
    std::string atom_id;
    std::size_t index{};
    std::pair<std::size_t, std::size_t> byte_span;
    std::string content_digest;
    ExperienceAtom(std::string atom_id, std::size_t index,
                   std::pair<std::size_t, std::size_t> byte_span,
                   std::string content_digest);
    friend bool operator==(const ExperienceAtom&, const ExperienceAtom&) = default;
};

struct PreparedAtom final {
    ExperienceAtom atom;
    AtomParent parent;
    std::string source_hash;
    std::vector<std::string> neighbor_atoms;
    std::vector<std::byte> data;
};

class AtomizedExperience final {
public:
    AtomizedExperience(AtomParent parent, std::shared_ptr<const LosslessBlob> blob,
                       std::vector<ExperienceAtom> atoms);
    [[nodiscard]] static AtomizedExperience build(
        AtomParent parent, std::span<const std::byte> raw,
        std::span<const std::pair<std::size_t, std::size_t>> spans,
        LosslessBlockCodec codec = LosslessBlockCodec::zlib,
        std::size_t block_bytes = default_lossless_block_bytes);
    [[nodiscard]] std::string_view source_hash() const noexcept;
    [[nodiscard]] std::vector<std::string> neighbors(std::size_t index) const;
    [[nodiscard]] PreparedAtom prepare(std::size_t index) const;
    [[nodiscard]] std::vector<std::byte> reconstruct_cold() const;

    const AtomParent parent;
    const std::shared_ptr<const LosslessBlob> blob;
    const std::vector<ExperienceAtom> atoms;
};

[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> text_spans(
    std::span<const std::byte> raw, std::size_t maximum_bytes = 4096);

}  // namespace swegca::world
