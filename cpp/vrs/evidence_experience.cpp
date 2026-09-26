#include "vrs/evidence_experience.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace swegca::vrs {
namespace {
using namespace architecture::kernel;
constexpr std::string_view format = "application/vnd.swegca.evidence-v1";
constexpr std::string_view magic = "SWGCEVD1";
constexpr std::size_t prefix_bytes = 176;

void put(std::span<std::byte> data, std::size_t offset, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i) data[offset + i] = std::byte((value >> (8 * i)) & 255);
}
std::uint64_t get(std::span<const std::byte> data, std::size_t offset, unsigned size = 8) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(std::to_integer<unsigned>(data[offset + i])) << (8 * i);
    return value;
}
void put_digest(std::span<std::byte> data, std::size_t offset, const Digest& value) {
    std::copy(value.begin(), value.end(), data.begin() + offset);
}
Digest get_digest(std::span<const std::byte> data, std::size_t offset) {
    Digest value;
    std::copy_n(data.begin() + offset, value.size(), value.begin());
    return value;
}
std::string_view text(std::span<const std::byte> data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}
OriginalExperienceView parse_payload(const StoredExperience& stored) {
    auto original = stored.view();
    const auto data = original.content;
    if (original.media_type != format || data.size() < prefix_bytes || text(data.first(8)) != magic ||
        get(data, 158, 2) != 0 || get(data, 157, 1) > 1)
        throw std::invalid_argument("invalid SWEGCA observation encoding");
    const auto media_bytes = get(data, 160), payload_bytes = get(data, 168);
    if (media_bytes == 0 || media_bytes > data.size() - prefix_bytes ||
        payload_bytes != data.size() - prefix_bytes - media_bytes)
        throw std::invalid_argument("invalid SWEGCA observation lengths");
    original.media_type = text(data.subspan(prefix_bytes, media_bytes));
    original.content = data.subspan(prefix_bytes + media_bytes);
    return original;
}
}  // namespace

ExperienceEvidence record_evidence(ExperienceBlock& block, const EvidenceRules& rules,
    const OriginalExperienceView& original, const EvidenceObservation& value, MemoryBudget& memory) {
    if (!observation_values_valid(rules, value.hypothesis, value) || named_digest(value.address) ||
        value.observed_at != original.observed_at_ns || original.media_type.empty())
        throw std::invalid_argument("invalid recorded SWEGCA observation");
    if (original.media_type.size() > std::numeric_limits<std::size_t>::max() - prefix_bytes ||
        original.content.size() > std::numeric_limits<std::size_t>::max() - prefix_bytes - original.media_type.size())
        throw std::overflow_error("SWEGCA observation size overflow");
    std::pmr::vector<std::byte> encoded(&memory);
    encoded.resize(prefix_bytes + original.media_type.size() + original.content.size());
    for (std::size_t i = 0; i < magic.size(); ++i) encoded[i] = std::byte(magic[i]);
    put_digest(encoded, 8, value.hypothesis);
    put_digest(encoded, 40, value.source);
    put_digest(encoded, 72, value.context);
    put_digest(encoded, 104, value.producer);
    put(encoded, 136, value.expires_at);
    put(encoded, 144, std::bit_cast<std::uint64_t>(value.producer_confidence));
    put(encoded, 152, value.axis, 4);
    put(encoded, 156, static_cast<std::uint8_t>(value.outcome), 1);
    put(encoded, 157, value.has_expiry, 1);
    put(encoded, 160, original.media_type.size());
    put(encoded, 168, original.content.size());
    for (std::size_t i = 0; i < original.media_type.size(); ++i)
        encoded[prefix_bytes + i] = std::byte(original.media_type[i]);
    std::copy(original.content.begin(), original.content.end(),
        encoded.begin() + prefix_bytes + original.media_type.size());
    auto wrapped = original;
    wrapped.media_type = format;
    wrapped.content = encoded;
    const auto location = block.append(wrapped);
    auto bound = value;
    bound.address = location.digest;
    return ExperienceEvidence(location, bound);
}

ExperienceEvidence decode_evidence(const EvidenceRules& rules, const StoredExperience& stored) {
    const auto original = parse_payload(stored);
    const auto data = stored.view().content;
    EvidenceObservation value;
    value.hypothesis = get_digest(data, 8);
    value.source = get_digest(data, 40);
    value.context = get_digest(data, 72);
    value.producer = get_digest(data, 104);
    value.address = stored.location().digest;
    value.observed_at = original.observed_at_ns;
    value.expires_at = get(data, 136);
    value.producer_confidence = std::bit_cast<double>(get(data, 144));
    value.axis = static_cast<std::uint32_t>(get(data, 152, 4));
    value.outcome = static_cast<EvidenceOutcome>(get(data, 156, 1));
    value.has_expiry = get(data, 157, 1) != 0;
    if (admit_observation(rules, value.hypothesis, value, value.observed_at, false) == ObservationUse::invalid)
        throw std::invalid_argument("invalid stored SWEGCA observation");
    return ExperienceEvidence(stored.location(), value);
}

OriginalExperienceView evidence_payload(const StoredExperience& stored) { return parse_payload(stored); }
}  // namespace swegca::vrs
