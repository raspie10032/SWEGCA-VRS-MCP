#include "vrs/evidence_experience.hpp"
#include "swegca_architecture/input_cue.hpp"

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
void validate_encoding(std::span<const std::byte> data,std::uint64_t total) {
    if (data.size() < prefix_bytes || total < prefix_bytes || text(data.first(8)) != magic ||
        get(data, 158, 1) > 1 || get(data, 159, 1) != 0 || get(data, 157, 1) > 1)
        throw std::invalid_argument("invalid SWEGCA observation encoding");
    const auto media_bytes = get(data, 160), payload_bytes = get(data, 168);
    const auto header = prefix_bytes + (get(data,158,1) ? 32 : 0);
    if (total < header || media_bytes == 0 || media_bytes > total - header ||
        payload_bytes != total - header - media_bytes)
        throw std::invalid_argument("invalid SWEGCA observation lengths");
}
EvidenceObservation decode_observation(const EvidenceRules& rules,std::span<const std::byte> data,
    const ExperienceLocation& location,std::uint64_t observed_at) {
    EvidenceObservation value;
    value.hypothesis = get_digest(data, 8);
    value.source = get_digest(data, 40);
    value.context = get_digest(data, 72);
    value.producer = get_digest(data, 104);
    value.address = location.digest;
    value.observed_at = observed_at;
    value.expires_at = get(data, 136);
    value.producer_confidence = std::bit_cast<double>(get(data, 144));
    value.axis = static_cast<std::uint32_t>(get(data, 152, 4));
    value.outcome = static_cast<EvidenceOutcome>(get(data, 156, 1));
    value.has_expiry = get(data, 157, 1) != 0;
    if (admit_observation(rules, value.hypothesis, value, value.observed_at, false) == ObservationUse::invalid)
        throw std::invalid_argument("invalid stored SWEGCA observation");
    return value;
}
OriginalExperienceView parse_payload(const StoredExperience& stored) {
    auto original = stored.view();const auto data=original.content;
    if(original.media_type!=format)throw std::invalid_argument("invalid SWEGCA observation encoding");
    validate_encoding(data,data.size());const auto media_bytes=get(data,160);
    const auto header=prefix_bytes+(get(data,158,1)?32:0);
    original.media_type = text(data.subspan(header, media_bytes));
    original.content = data.subspan(header + media_bytes);
    return original;
}
}  // namespace

ExperienceEvidence record_evidence(ExperienceBlock& block, const EvidenceRules& rules,
    const OriginalExperienceView& original, const EvidenceObservation& value, std::optional<Digest> input_key) {
    if (!observation_values_valid(rules, value.hypothesis, value) || named_digest(value.address) ||
        value.observed_at != original.observed_at_ns || original.media_type.empty())
        throw std::invalid_argument("invalid recorded SWEGCA observation");
    const auto header = prefix_bytes + (input_key ? 32 : 0);
    if (original.media_type.size() > std::numeric_limits<std::size_t>::max() - header ||
        original.content.size() > std::numeric_limits<std::size_t>::max() - header - original.media_type.size())
        throw std::overflow_error("SWEGCA observation size overflow");
    std::array<std::byte, prefix_bytes> encoded{};
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
    put(encoded, 158, input_key.has_value(), 1);
    put(encoded, 160, original.media_type.size());
    put(encoded, 168, original.content.size());
    auto wrapped = original;
    wrapped.media_type = format;
    wrapped.content = {};
    const std::array parts{std::span<const std::byte>(encoded),
        input_key ? std::span<const std::byte>(*input_key) : std::span<const std::byte>{},
        std::as_bytes(std::span(original.media_type)),original.content};
    const auto location = block.append_parts(wrapped,parts);
    auto bound = value;
    bound.address = location.digest;
    return ExperienceEvidence(location, bound, input_key ? *input_key : architecture::input_cue(original.media_type, original.content));
}

ExperienceEvidence decode_evidence(const EvidenceRules& rules, const StoredExperience& stored) {
    const auto original = parse_payload(stored);
    const auto data = stored.view().content;
    const auto value=decode_observation(rules,data,stored.location(),original.observed_at_ns);
    return ExperienceEvidence(stored.location(), value, get(data,158,1) ? get_digest(data,prefix_bytes) : architecture::input_cue(original.media_type, original.content));
}

// This decoder alone sees provisional chunks. No callback or byte range is
// exposed until the block trailer, address digest and core admission all pass.
class EvidenceReader final {
public:
    static ExperienceEvidence read(const EvidenceRules& rules, const ExperienceBlock& block,
        const ExperienceLocation& location, std::uint64_t limit,
        std::pmr::vector<std::byte>* payload = nullptr, std::uint64_t offset = 0,
        std::uint64_t count = 0, std::uint64_t* total = nullptr) {
        struct Decoder {
            std::array<std::byte,prefix_bytes> prefix;
            architecture::Sha256 cue;
            std::optional<Digest> input_key;
            std::pmr::vector<std::byte>* payload;
            std::uint64_t offset, count, total = 0, payload_begin = 0;
            bool seen = false;
        } decoder{{}, {}, {}, payload, offset, count};
        const auto consume=[](void* opaque, bool media, std::span<const std::byte> data,
                              std::uint64_t position, std::uint64_t length) {
            auto& state=*static_cast<Decoder*>(opaque);
            if(media) {
                if(length!=format.size() || text(data)!=format.substr(static_cast<std::size_t>(position),data.size()))
                    throw std::invalid_argument("invalid SWEGCA observation encoding");
                return;
            }
            if(position==0) {
                validate_encoding(data,length);
                std::copy_n(data.begin(),prefix_bytes,state.prefix.begin());
                state.seen=true;
                const auto media_bytes=get(state.prefix,160);
                state.total=get(state.prefix,168);
                const auto header=prefix_bytes+(get(state.prefix,158,1)?32:0);
                if(data.size()<header)throw std::invalid_argument("truncated input key");
                if(get(state.prefix,158,1))state.input_key=get_digest(data,prefix_bytes);
                state.payload_begin=header+media_bytes;
                state.cue=architecture::input_cue_prefix(media_bytes);
                if(state.payload) {
                    if(state.offset>state.total || state.count>state.total-state.offset ||
                       state.count>std::numeric_limits<std::size_t>::max())
                        throw std::invalid_argument("original payload range outside record");
                    state.payload->resize(static_cast<std::size_t>(state.count));
                }
                data=data.subspan(header);
                position+=header;
            }
            if(!state.input_key)state.cue.update(data);
            if(state.payload && state.count) {
                const auto first=state.payload_begin+state.offset;
                const auto begin=std::max(first,position);
                const auto end=std::min(first+state.count,position+data.size());
                if(begin<end)
                    std::copy_n(data.begin()+static_cast<std::size_t>(begin-position),
                        static_cast<std::size_t>(end-begin),
                        state.payload->begin()+static_cast<std::size_t>(begin-first));
            }
        };
        const auto observed=block.visit_evidence(location,limit,&decoder,consume);
        if(!decoder.seen)throw std::invalid_argument("missing SWEGCA observation encoding");
        const auto value=decode_observation(rules,decoder.prefix,location,observed);
        if(total)*total=decoder.total;
        return ExperienceEvidence(location,value,decoder.input_key ? *decoder.input_key : decoder.cue.finish());
    }
};

ExperienceEvidence read_evidence(const EvidenceRules& rules,const ExperienceBlock& block,
    const ExperienceLocation& location,std::uint64_t limit) {
    return EvidenceReader::read(rules,block,location,limit);
}

EvidencePayloadSlice read_evidence_slice(const EvidenceRules& rules,const ExperienceBlock& block,
    const ExperienceLocation& location,std::uint64_t limit,
    std::uint64_t offset,std::uint64_t count,MemoryBudget& memory) {
    std::pmr::vector<std::byte> payload(&memory);
    std::uint64_t total=0;
    auto evidence=EvidenceReader::read(rules,block,location,limit,&payload,offset,count,&total);
    return EvidencePayloadSlice(std::move(evidence),offset,total,std::move(payload));
}

OriginalExperienceView evidence_payload(const StoredExperience& stored) { return parse_payload(stored); }
}  // namespace swegca::vrs
