#include "world/experience_atoms.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

using architecture::Sha256;

bool nonblank(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char c) { return c > 0x20U; });
}
std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i != digest.size(); ++i) {
        const auto v = std::to_integer<unsigned>(digest[i]);
        result[2 * i] = digits[v >> 4U]; result[2 * i + 1] = digits[v & 15U];
    }
    return result;
}
std::string digest(const std::span<const std::byte> data) { return hex(Sha256::of(data)); }
bool digest_text(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array out; for (const auto& value : values) out.emplace_back(value); return out;
}
JsonValue parent_json(const AtomParent& parent) {
    JsonValue time(nullptr);
    if (parent.observed_at) time = JsonValue::Object{
        {"clock", parent.observed_at->clock}, {"ns", parent.observed_at->ns},
        {"uncertainty_ns", parent.observed_at->uncertainty_ns}};
    return JsonValue::Object{{"modality", parent.modality}, {"observed_at", std::move(time)},
        {"outcome", parent.outcome}, {"parent_experience_id", parent.parent_experience_id},
        {"provenance", strings(parent.provenance)}, {"representation", parent.representation},
        {"revision", parent.revision}, {"source_address", parent.source_address}};
}
std::string atom_identifier(const AtomParent& parent, const std::string_view source_hash,
                            const std::pair<std::size_t, std::size_t> span,
                            const std::string_view content_digest) {
    JsonValue::Array binding{parent_json(parent), JsonValue(source_hash)};
    JsonValue::Array range{JsonValue(static_cast<std::int64_t>(span.first)),
                           JsonValue(static_cast<std::int64_t>(span.second))};
    JsonValue::Array payload{JsonValue(std::move(binding)), JsonValue(std::move(range)),
                             JsonValue(content_digest)};
    const auto wire = semantic_canonical_json(JsonValue(std::move(payload)));
    return "experience-atom:" + hex(Sha256::of(std::as_bytes(std::span(wire.data(), wire.size()))));
}
bool valid_utf8(const std::span<const std::byte> raw) {
    std::size_t i = 0;
    while (i < raw.size()) {
        const auto c = std::to_integer<unsigned char>(raw[i++]);
        if (c < 0x80U) continue;
        unsigned count = c >= 0xf0U ? 3 : c >= 0xe0U ? 2 : c >= 0xc2U ? 1 : 99;
        if (count == 99 || i + count > raw.size()) return false;
        for (unsigned n = 0; n != count; ++n)
            if ((std::to_integer<unsigned char>(raw[i++]) & 0xc0U) != 0x80U) return false;
    }
    return true;
}

}  // namespace

AtomParent::AtomParent(
    std::string parent_id, std::string address, std::string revision_value,
    std::vector<std::string> provenance_value, std::string outcome_value,
    std::string modality_value, std::optional<TimePoint> observed,
    std::string representation_value)
    : parent_experience_id(std::move(parent_id)), source_address(std::move(address)),
      revision(std::move(revision_value)), provenance(std::move(provenance_value)),
      outcome(std::move(outcome_value)), modality(std::move(modality_value)),
      observed_at(std::move(observed)), representation(std::move(representation_value)) {
    if (!nonblank(parent_experience_id) || !nonblank(source_address) || !nonblank(revision) ||
        provenance.empty() || !std::ranges::all_of(provenance, nonblank))
        throw std::invalid_argument("nonempty identity/provenance required");
    static const std::set<std::string, std::less<>> outcomes{
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    static const std::set<std::string, std::less<>> modalities{
        "text", "code", "image", "screen", "audio", "video", "record"};
    if (!outcomes.contains(outcome)) throw std::invalid_argument("explicit historical outcome required");
    if (!modalities.contains(modality)) throw std::invalid_argument("unsupported modality");
    if (representation != "canonical_source" && representation != "retained_derived_artifact")
        throw std::invalid_argument("explicit source representation required");
}

ExperienceAtom::ExperienceAtom(
    std::string id, const std::size_t index_value,
    const std::pair<std::size_t, std::size_t> span, std::string digest_value)
    : atom_id(std::move(id)), index(index_value), byte_span(span),
      content_digest(std::move(digest_value)) {
    if (byte_span.first >= byte_span.second || !digest_text(content_digest) ||
        !atom_id.starts_with("experience-atom:") || !digest_text(atom_id.substr(16)))
        throw std::invalid_argument("invalid immutable atom address");
}

AtomizedExperience::AtomizedExperience(
    AtomParent parent_value, std::shared_ptr<const LosslessBlob> blob_value,
    std::vector<ExperienceAtom> atom_values)
    : parent(std::move(parent_value)), blob(std::move(blob_value)), atoms(std::move(atom_values)) {
    if (!blob) throw std::invalid_argument("immutable canonical atom generation required");
}

AtomizedExperience AtomizedExperience::build(
    AtomParent parent, const std::span<const std::byte> raw,
    const std::span<const std::pair<std::size_t, std::size_t>> spans,
    const LosslessBlockCodec codec, const std::size_t block_bytes) {
    std::size_t cursor = 0;
    for (const auto& span : spans) {
        if (span.first != cursor || span.first >= span.second || span.second > raw.size())
            throw std::invalid_argument("spans must exactly partition canonical bytes");
        cursor = span.second;
    }
    if (cursor != raw.size() || (raw.empty() && !spans.empty()))
        throw std::invalid_argument("spans omit canonical source bytes");
    auto blob = LosslessBlob::build(raw, codec, 3, block_bytes);
    std::vector<ExperienceAtom> atoms;
    for (std::size_t index = 0; index != spans.size(); ++index) {
        const auto content = digest(raw.subspan(spans[index].first,
                                                spans[index].second - spans[index].first));
        atoms.emplace_back(atom_identifier(parent, blob->content_sha256, spans[index], content),
                           index, spans[index], content);
    }
    return AtomizedExperience(std::move(parent), std::move(blob), std::move(atoms));
}

std::string_view AtomizedExperience::source_hash() const noexcept { return blob->content_sha256; }

std::vector<std::string> AtomizedExperience::neighbors(const std::size_t index) const {
    if (index >= atoms.size()) throw std::out_of_range("atom index outside canonical revision");
    std::vector<std::string> result;
    if (index) result.push_back(atoms[index - 1].atom_id);
    if (index + 1 < atoms.size()) result.push_back(atoms[index + 1].atom_id);
    return result;
}

PreparedAtom AtomizedExperience::prepare(const std::size_t index) const {
    if (index >= atoms.size()) throw std::out_of_range("atom index outside canonical revision");
    const auto& atom = atoms[index];
    if (atom.index != index || atom.atom_id != atom_identifier(parent, source_hash(), atom.byte_span, atom.content_digest))
        throw std::invalid_argument("atom provenance binding changed");
    auto data = blob->read(atom.byte_span.first, atom.byte_span.second).data;
    if (digest(data) != atom.content_digest) throw std::invalid_argument("atom content digest changed");
    return {atom, parent, std::string(source_hash()), neighbors(index), std::move(data)};
}

std::vector<std::byte> AtomizedExperience::reconstruct_cold() const {
    blob->verify_cold();
    std::vector<std::byte> raw;
    std::size_t cursor = 0;
    for (std::size_t index = 0; index != atoms.size(); ++index) {
        const auto& atom = atoms[index];
        if (atom.index != index || atom.byte_span.first != cursor || atom.byte_span.first >= atom.byte_span.second)
            throw std::invalid_argument("atom coverage or ordering changed");
        auto item = prepare(index);
        raw.insert(raw.end(), item.data.begin(), item.data.end());
        cursor = atom.byte_span.second;
    }
    if (cursor != blob->raw_size || digest(raw) != source_hash())
        throw std::invalid_argument("atom reconstruction changed source");
    return raw;
}

std::vector<std::pair<std::size_t, std::size_t>> text_spans(
    const std::span<const std::byte> raw, const std::size_t maximum_bytes) {
    if (maximum_bytes < 4 || !valid_utf8(raw))
        throw std::invalid_argument("immutable UTF-8 bytes and maximum_bytes >= 4 required");
    std::set<std::size_t> boundaries;
    for (std::size_t i = 0; i < raw.size();) {
        const bool line_start = i == 0 || raw[i - 1] == std::byte{'\n'};
        if (line_start && raw[i] == std::byte{'#'}) {
            auto j = i; while (j < raw.size() && raw[j] == std::byte{'#'} && j - i < 6) ++j;
            if (j > i && j - i <= 6 && j < raw.size() && (raw[j] == std::byte{' '} || raw[j] == std::byte{'\t'})) boundaries.insert(i);
        }
        if (raw[i] == std::byte{'\n'}) {
            auto j = i + 1; while (j < raw.size() && (raw[j] == std::byte{' '} || raw[j] == std::byte{'\t'})) ++j;
            if (j < raw.size() && raw[j] == std::byte{'\r'}) ++j;
            if (j < raw.size() && raw[j] == std::byte{'\n'}) boundaries.insert(j + 1);
        }
        ++i;
    }
    std::vector<std::pair<std::size_t, std::size_t>> result;
    for (std::size_t start = 0; start < raw.size();) {
        const auto limit = std::min(raw.size(), start + maximum_bytes);
        auto stop = limit;
        if (limit != raw.size()) {
            const auto at = boundaries.upper_bound(limit);
            if (at != boundaries.begin()) { const auto value = *std::prev(at); if (value > start) stop = value; }
            while (stop > start && (std::to_integer<unsigned char>(raw[stop]) & 0xc0U) == 0x80U) --stop;
        }
        result.emplace_back(start, stop); start = stop;
    }
    return result;
}

}  // namespace swegca::world
