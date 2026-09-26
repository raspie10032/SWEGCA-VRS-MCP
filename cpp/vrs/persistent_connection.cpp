#include "vrs/persistent_connection.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace swegca::vrs {
namespace {
using namespace architecture;
using namespace architecture::kernel;
constexpr std::string_view magic = "SWGCCON1";
constexpr std::string_view source = "swegca-connection";
constexpr std::string_view media = "application/vnd.swegca.connection-v1";
constexpr std::size_t header_size = 152;
enum class EventKind : std::uint8_t { create = 1, append = 2, refine = 3 };

void put(std::span<std::byte> data, std::size_t at, std::uint64_t value, unsigned width = 8) {
    for (unsigned i = 0; i < width; ++i) data[at + i] = std::byte(value >> (8 * i));
}
std::uint64_t get(std::span<const std::byte> data, std::size_t at, unsigned width = 8) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(std::to_integer<unsigned>(data[at + i])) << (8 * i);
    return value;
}
void put_digest(std::span<std::byte> data, std::size_t at, const DigestBytes& value) {
    std::copy(value.begin(), value.end(), data.begin() + at);
}
DigestBytes get_digest(std::span<const std::byte> data, std::size_t at) {
    DigestBytes result; std::copy_n(data.begin() + at, result.size(), result.begin()); return result;
}
void put_location(std::span<std::byte> data, std::size_t at, const ExperienceLocation& location) {
    put_digest(data, at, location.block); put(data, at + 32, location.offset);
    put(data, at + 40, location.bytes); put_digest(data, at + 48, location.digest);
}
ExperienceLocation get_location(std::span<const std::byte> data, std::size_t at) {
    return {get_digest(data, at), get(data, at + 32), get(data, at + 40), get_digest(data, at + 48)};
}
bool empty_location(const ExperienceLocation& value) {
    return value.block == zero_digest_bytes && value.digest == zero_digest_bytes && value.offset == 0 && value.bytes == 0;
}
bool same_bits(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }

struct Event {
    EventKind kind{};
    DigestBytes identity{};
    ExperienceLocation parent{}, original{};
    std::uint64_t ordinal = 0, before = 0, after = 0, seed = 0, step = 0;
    double previous_strength = 0, next_strength = 0;
    EvidencePolicy policy;
    DigestBytes refinement{};
};
struct EncodedEvent {
    std::array<std::byte, 256> bytes{};
    std::size_t size = 0;
    std::span<const std::byte> view() const { return std::span(bytes).first(size); }
};

void put_policy(std::span<std::byte> data, std::size_t at, const EvidencePolicy& policy) {
    for (const auto value : {policy.chance_rate, policy.accept_margin, policy.confidence_level, policy.prior_alpha, policy.prior_beta}) {
        put(data, at, std::bit_cast<std::uint64_t>(value)); at += 8;
    }
    for (const auto value : {policy.minimum_effective_samples_per_axis, policy.minimum_source_diversity,
            policy.minimum_axis_source_diversity, policy.minimum_context_diversity, policy.recent_window, policy.minimum_recent_samples}) {
        put(data, at, value, 4); at += 4;
    }
    put(data, at, std::bit_cast<std::uint64_t>(policy.regime_change_threshold));
    put(data, at + 8, policy.axis_count, 4);
}
EvidencePolicy get_policy(std::span<const std::byte> data, std::size_t at) {
    EvidencePolicy policy;
    for (auto* target : {&policy.chance_rate, &policy.accept_margin, &policy.confidence_level, &policy.prior_alpha, &policy.prior_beta}) {
        *target = std::bit_cast<double>(get(data, at)); at += 8;
    }
    for (auto* target : {&policy.minimum_effective_samples_per_axis, &policy.minimum_source_diversity,
            &policy.minimum_axis_source_diversity, &policy.minimum_context_diversity, &policy.recent_window, &policy.minimum_recent_samples}) {
        *target = static_cast<std::uint32_t>(get(data, at, 4)); at += 4;
    }
    policy.regime_change_threshold = std::bit_cast<double>(get(data, at));
    policy.axis_count = static_cast<std::uint32_t>(get(data, at + 8, 4));
    return policy;
}

EncodedEvent encode(const Event& event) {
    EncodedEvent result;
    auto& data = result.bytes;
    std::memcpy(data.data(), magic.data(), magic.size()); put(data, 8, static_cast<unsigned>(event.kind), 1);
    put_digest(data, 16, event.identity); put_location(data, 48, event.parent);
    put(data, 128, event.ordinal); put(data, 136, event.before); put(data, 144, event.after);
    switch (event.kind) {
    case EventKind::create:
        put(data, header_size, std::bit_cast<std::uint64_t>(event.previous_strength));
        put_policy(data, header_size + 8, event.policy); result.size = header_size + 8 + 76;
        break;
    case EventKind::append:
        put_location(data, header_size, event.original); result.size = header_size + 80;
        break;
    case EventKind::refine:
        put(data, header_size, event.seed); put(data, header_size + 8, event.step);
        put(data, header_size + 16, std::bit_cast<std::uint64_t>(event.previous_strength));
        put(data, header_size + 24, std::bit_cast<std::uint64_t>(event.next_strength));
        put_digest(data, header_size + 32, event.refinement); result.size = header_size + 64;
        break;
    }
    return result;
}

Event decode(const StoredExperience& stored, std::string_view session) {
    const auto record = stored.view();
    const auto data = record.content;
    if (record.session != session || record.source != source || record.media_type != media ||
        data.size() < header_size || std::memcmp(data.data(), magic.data(), magic.size()) != 0 ||
        !std::all_of(data.begin() + 9, data.begin() + 16, [](auto byte) { return byte == std::byte{}; }))
        throw std::runtime_error("invalid VRS connection record");
    Event event;
    event.kind = static_cast<EventKind>(get(data, 8, 1));
    event.identity = get_digest(data, 16); event.parent = get_location(data, 48);
    event.ordinal = get(data, 128); event.before = get(data, 136); event.after = get(data, 144);
    if (!named_digest(event.identity) || record.sequence != event.ordinal)
        throw std::runtime_error("invalid VRS connection record identity");
    switch (event.kind) {
    case EventKind::create:
        if (data.size() != header_size + 84 || event.ordinal != 0 || event.before != 0 || event.after != 0 ||
            !empty_location(event.parent) || record.observed_at_ns != 0)
            throw std::runtime_error("invalid VRS connection origin");
        event.previous_strength = std::bit_cast<double>(get(data, header_size));
        event.policy = get_policy(data, header_size + 8);
        break;
    case EventKind::append:
        if (data.size() != header_size + 80 || event.ordinal == 0 || empty_location(event.parent))
            throw std::runtime_error("invalid VRS connection append record");
        event.original = get_location(data, header_size);
        event.step = record.observed_at_ns;
        break;
    case EventKind::refine:
        if (data.size() != header_size + 64 || event.ordinal == 0 || empty_location(event.parent))
            throw std::runtime_error("invalid VRS connection refinement record");
        event.seed = get(data, header_size); event.step = get(data, header_size + 8);
        event.previous_strength = std::bit_cast<double>(get(data, header_size + 16));
        event.next_strength = std::bit_cast<double>(get(data, header_size + 24));
        event.refinement = get_digest(data, header_size + 32);
        if (record.observed_at_ns != event.step) throw std::runtime_error("VRS refinement time mismatch");
        break;
    default: throw std::runtime_error("unknown VRS connection record kind");
    }
    return event;
}

DigestBytes refinement_digest(const ConnectionRefinement& report) {
    Sha256 hash;
    hash.update("SWEGCA connection refinement v1"); hash.update(report.connection());
    const auto number = [&](std::uint64_t value) {
        std::array<std::byte, 8> bytes{}; put(bytes, 0, value); hash.update(bytes);
    };
    const auto real = [&](double value) { number(std::bit_cast<std::uint64_t>(value)); };
    number(report.before_revision()); number(report.after_revision()); number(report.seed()); number(report.current_step());
    const auto& tally = report.evidence();
    for (const auto value : tally.axis_support) real(value);
    for (const auto value : tally.axis_refute) real(value);
    for (const auto value : tally.axis_source_diversity) number(value);
    number(tally.source_diversity); number(tally.context_diversity); number(tally.recent_count);
    real(tally.recent_sum); number(tally.revision);
    const auto& judgment = report.result().verification().judgment();
    number(static_cast<unsigned>(judgment.status())); number(static_cast<unsigned>(judgment.reason()));
    for (const auto value : {judgment.posterior_mean(), judgment.causal_lower_bound(), judgment.overall_upper_bound(),
            judgment.effective_sample_size(), judgment.regime_change_score()}) real(value);
    number(judgment.source_diversity()); number(judgment.context_diversity()); number(judgment.revision());
    number(report.result().strength().valid()); real(report.result().strength().previous()); real(report.result().strength().current());
    number(report.samples().size());
    for (const auto& sample : report.samples()) { number(sample.experience_index); number(static_cast<unsigned>(sample.use)); }
    return hash.finish();
}
Event read_event(SessionStore& session, const ExperienceLocation& location) {
    // Fixed schema plus session metadata bounds the read before allocation.
    const auto limit = ExperienceBlock::record_overhead + session.name().size() + source.size() + media.size() + 256;
    return decode(session.read(location, limit), session.name());
}
ExperienceLocation write_event(SessionStore& session, const Event& event, std::uint64_t observed_at) {
    const auto encoded = encode(event);
    return session.append({event.ordinal, observed_at, session.name(), source, media, encoded.view()});
}
}  // namespace

PersistentConnection PersistentConnection::create(SessionStore& session, const DigestBytes& identity,
    double initial_strength, const EvidencePolicy& policy, MemoryBudget& memory, std::uint64_t original_read_limit) {
    return PersistentConnection(session, identity, initial_strength, policy, memory, original_read_limit);
}
PersistentConnection PersistentConnection::recover(SessionStore& session, const ExperienceLocation& head,
    MemoryBudget& memory, std::uint64_t original_read_limit) {
    return PersistentConnection(session, head, memory, original_read_limit);
}

PersistentConnection::PersistentConnection(SessionStore& session, const DigestBytes& identity, double initial_strength,
    const EvidencePolicy& policy, MemoryBudget& memory, std::uint64_t original_read_limit)
    : session_(session), memory_(memory), original_read_limit_(original_read_limit), policy_(policy),
      rules_(make_evidence_rules(policy)) {
    if (original_read_limit == 0) throw std::invalid_argument("zero original read limit");
    state_.emplace(identity, initial_strength, *rules_, memory_);
    Event event;
    event.kind = EventKind::create; event.identity = identity; event.previous_strength = initial_strength; event.policy = policy;
    head_ = write_event(session_, event, 0);
}

PersistentConnection::PersistentConnection(SessionStore& session, const ExperienceLocation& head,
    MemoryBudget& memory, std::uint64_t original_read_limit)
    : session_(session), memory_(memory), original_read_limit_(original_read_limit), head_(head) {
    if (original_read_limit == 0) throw std::invalid_argument("zero original read limit");
    std::pmr::vector<ExperienceLocation> chain(&memory_);
    auto cursor = head;
    DigestBytes identity{};
    std::uint64_t expected = 0;
    for (;;) {
        const auto event = read_event(session_, cursor);
        if (chain.empty()) { identity = event.identity; expected = ordinal_ = event.ordinal; }
        if (event.identity != identity || event.ordinal != expected)
            throw std::runtime_error("VRS connection parent lineage mismatch");
        chain.push_back(cursor);
        if (event.kind == EventKind::create) {
            policy_ = event.policy; rules_.emplace(make_evidence_rules(policy_));
            state_.emplace(identity, event.previous_strength, *rules_, memory_);
            break;
        }
        if (expected == 0) throw std::runtime_error("VRS connection has no origin");
        --expected; cursor = event.parent;
    }
    // Recompute from original observations and the recorded rule configuration.
    // An encoded status or strength never bypasses SWEGCA.
    for (auto i = chain.size() - 1; i > 0; --i) {
        const auto event = read_event(session_, chain[i - 1]);
        if (state_->revision() != event.before) throw std::runtime_error("VRS connection revision mismatch");
        if (event.kind == EventKind::append) {
            const auto record = session_.read(event.original, original_read_limit_);
            const auto evidence = decode_evidence(*rules_, record);
            if (evidence.value().observed_at != event.step) throw std::runtime_error("VRS observation time mismatch");
            state_->append(evidence);
        } else if (event.kind == EventKind::refine) {
            const auto report = state_->prepare_refinement(event.seed, event.step);
            const auto next_strength = report.result().strength().valid() ? report.result().strength().current() : state_->strength();
            if (!same_bits(state_->strength(), event.previous_strength) || !same_bits(next_strength, event.next_strength) ||
                refinement_digest(report) != event.refinement || report.after_revision() != event.after)
                throw std::runtime_error("stored VRS result disagrees with SWEGCA replay");
            state_->commit_refinement(report);
        } else throw std::runtime_error("duplicate VRS connection origin");
        if (state_->revision() != event.after) throw std::runtime_error("VRS connection resulting revision mismatch");
    }
}

void PersistentConnection::append(const ExperienceLocation& original) {
    if (ordinal_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("VRS connection record sequence exhausted");
    const auto record = session_.read(original, original_read_limit_);
    const auto evidence = decode_evidence(*rules_, record);
    state_->prepare_append(evidence);
    Event event;
    event.kind = EventKind::append; event.identity = state_->identity(); event.parent = head_; event.ordinal = ordinal_ + 1;
    event.before = state_->revision(); event.after = event.before + 1; event.original = original;
    const auto saved = write_event(session_, event, evidence.value().observed_at);
    state_->commit_append(evidence); head_ = saved; ordinal_ = event.ordinal;
}

ConnectionRefinement PersistentConnection::refine(std::uint64_t seed, std::uint64_t current_step) {
    if (ordinal_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("VRS connection record sequence exhausted");
    auto report = state_->prepare_refinement(seed, current_step);
    Event event;
    event.kind = EventKind::refine; event.identity = state_->identity(); event.parent = head_; event.ordinal = ordinal_ + 1;
    event.before = report.before_revision(); event.after = report.after_revision(); event.seed = seed; event.step = current_step;
    event.previous_strength = state_->strength();
    event.next_strength = report.result().strength().valid() ? report.result().strength().current() : state_->strength();
    event.refinement = refinement_digest(report);
    const auto saved = write_event(session_, event, current_step);
    state_->commit_refinement(report); head_ = saved; ordinal_ = event.ordinal;
    return report;
}

ConnectionHead PersistentConnection::snapshot() const noexcept {
    return {state_->identity(), head_, state_->revision(), ordinal_, state_->experiences().size(), state_->strength()};
}

bool PersistentConnection::verifies_extension(SessionStore& session, const ConnectionHead& candidate,
    const ExperienceLocation& previous) {
    auto cursor = candidate.record;
    auto remaining = candidate.ordinal;
    for (;;) {
        const auto event = read_event(session, cursor);
        if (event.identity != candidate.identity || event.ordinal != remaining)
            throw std::runtime_error("VRS extension identity mismatch");
        if (cursor == previous) return true;
        if (event.kind == EventKind::create || remaining == 0) return false;
        --remaining; cursor = event.parent;
    }
}

bool PersistentConnection::contains_history(std::span<const ExperienceLocation> addresses) const {
    if (addresses.empty()) return true;
    auto cursor = head_;
    auto remaining = ordinal_;
    std::size_t found = 0;
    for (;;) {
        if (cursor == addresses[found] && ++found == addresses.size()) return true;
        const auto event = read_event(session_, cursor);
        if (event.identity != state_->identity() || event.ordinal != remaining)
            throw std::runtime_error("VRS history identity mismatch");
        if (event.kind == EventKind::create || remaining == 0) return false;
        --remaining; cursor = event.parent;
    }
}

}  // namespace swegca::vrs
