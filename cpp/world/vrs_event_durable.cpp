#include "world/vrs_event_durable.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <stdexcept>

namespace swegca::world {
namespace {

constexpr std::string_view magic = "RZEVT001";

std::string hex(const architecture::DigestBytes& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(value.size() * 2, '0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = std::to_integer<unsigned>(value[i]);
        result[2 * i] = digits[byte >> 4U];
        result[2 * i + 1] = digits[byte & 15U];
    }
    return result;
}

std::string digest(const std::span<const std::byte> bytes) {
    return hex(architecture::Sha256::of(bytes));
}

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void put_u64(std::vector<std::byte>& out, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

void put_text(std::vector<std::byte>& out, const std::string_view value) {
    put_u64(out, value.size());
    const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::uint64_t take_u64(const std::span<const std::byte> data, std::size_t& offset) {
    if (offset > data.size() || data.size() - offset < 8)
        throw CorruptVrsBlock("truncated numeric event report");
    std::uint64_t value{};
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(data[offset++])) << shift;
    return value;
}

std::string take_text(const std::span<const std::byte> data, std::size_t& offset) {
    const auto size = take_u64(data, offset);
    if (size > data.size() - offset)
        throw CorruptVrsBlock("truncated numeric event report");
    const auto* begin = reinterpret_cast<const char*>(data.data() + offset);
    offset += static_cast<std::size_t>(size);
    return {begin, static_cast<std::size_t>(size)};
}

std::vector<std::byte> report_wire(
    const BoundEventHotSource& hot, const std::string_view next_bundle,
    const PreparedEventSignalStorage& candidate) {
    std::vector<std::byte> out;
    const auto magic_bytes = std::as_bytes(std::span(magic.data(), magic.size()));
    out.insert(out.end(), magic_bytes.begin(), magic_bytes.end());
    put_text(out, hot.pair.snapshot_id);
    put_text(out, hot.pair.memory->snapshot_id());
    put_text(out, hot.pair.vrs_snapshot_id);
    put_text(out, hot.source->snapshot_id());
    put_text(out, next_bundle);
    put_u64(out, candidate.proposal->signal.rounds);
    put_u64(out, candidate.proposal->signal.node_evaluations);
    put_u64(out, candidate.proposal->signal.edge_evaluations);
    put_u64(out, candidate.proposal->signal.scores.size());
    put_u64(out, candidate.proposal->signal.strengths.size());
    put_u64(out, candidate.promotions.size());
    for (const auto& row : candidate.promotions) {
        put_text(out, row.snapshot_id);
        put_text(out, row.connection_id);
        const auto previous = std::bit_cast<std::uint64_t>(row.previous_strength);
        const auto current = std::bit_cast<std::uint64_t>(row.current_strength);
        put_u64(out, previous);
        put_u64(out, current);
        put_text(out, row.action);
        put_u64(out, row.promoted ? 1 : 0);
        put_u64(out, row.semantic_evidence_allowed ? 1 : 0);
    }
    return out;
}

struct ParsedReport final { std::string bundle_sha256; };

ParsedReport parse_report(const BoundEventHotSource& hot,
                          const std::span<const std::byte> data) {
    if (data.size() < magic.size() ||
        std::memcmp(data.data(), magic.data(), magic.size()) != 0)
        throw CorruptVrsBlock("numeric event report schema changed");
    std::size_t offset = magic.size();
    if (take_text(data, offset) != hot.pair.snapshot_id ||
        take_text(data, offset) != hot.pair.memory->snapshot_id() ||
        take_text(data, offset) != hot.pair.vrs_snapshot_id ||
        take_text(data, offset) != hot.source->snapshot_id())
        throw CorruptVrsBlock("numeric event report parent changed");
    ParsedReport result{take_text(data, offset)};
    if (!digest_id(result.bundle_sha256))
        throw CorruptVrsBlock("numeric event bundle identity changed");
    (void)take_u64(data, offset);
    (void)take_u64(data, offset);
    (void)take_u64(data, offset);
    (void)take_u64(data, offset);
    (void)take_u64(data, offset);
    const auto promotions = take_u64(data, offset);
    for (std::uint64_t i = 0; i < promotions; ++i) {
        (void)take_text(data, offset);
        (void)take_text(data, offset);
        (void)take_u64(data, offset);
        (void)take_u64(data, offset);
        (void)take_text(data, offset);
        const auto promoted = take_u64(data, offset);
        const auto semantic = take_u64(data, offset);
        if (promoted > 1 || semantic > 1)
            throw CorruptVrsBlock("numeric event promotion flags changed");
    }
    if (offset != data.size()) throw CorruptVrsBlock("numeric event report has trailing bytes");
    return result;
}

bool same_generation(const VrsArrayBlocks& left, const VrsArrayBlocks& right) {
    if (left.dtype != right.dtype || left.shape != right.shape ||
        left.data->block_bytes != right.data->block_bytes ||
        left.data->raw_size != right.data->raw_size ||
        left.data->blocks.size() != right.data->blocks.size()) return false;
    for (std::size_t i = 0; i < left.data->blocks.size(); ++i)
        if (left.data->blocks[i]->digest != right.data->blocks[i]->digest) return false;
    return true;
}

}  // namespace

DurableEventBinding::DurableEventBinding(
    std::shared_ptr<const BoundEventHotSource> hot_value,
    std::shared_ptr<VrsGenerationBlockStore> store_value,
    std::string bundle)
    : hot(std::move(hot_value)), store(std::move(store_value)),
      bundle_sha256(std::move(bundle)) {
    if (!hot || !store || !digest_id(bundle_sha256))
        throw std::invalid_argument("durable event binding is incomplete");
}

std::shared_ptr<const DurableEventBinding> DurableEventBinding::bootstrap(
    std::shared_ptr<const BoundEventHotSource> hot,
    std::shared_ptr<VrsGenerationBlockStore> store) {
    if (!hot || !store) throw std::invalid_argument("bound main parent and generation store required");
    VrsArrayBundle bundle({{"score", hot->storage->scores},
                           {"strength", hot->storage->strengths}});
    auto saved = bundle.save(*store);
    return std::shared_ptr<const DurableEventBinding>(new DurableEventBinding(
        std::move(hot), std::move(store), std::move(saved.bundle_sha256)));
}

std::shared_ptr<const DurableEventBinding> DurableEventBinding::cold_bind(
    std::shared_ptr<const BoundEventHotSource> hot,
    std::shared_ptr<VrsGenerationBlockStore> store,
    std::string bundle_sha256,
    const std::size_t maximum_raw_bytes) {
    if (!hot || !store) throw std::invalid_argument("bound main parent and generation store required");
    auto loaded = VrsArrayBundle::load(*store, bundle_sha256, maximum_raw_bytes);
    if (loaded.arrays.size() != 2 || !loaded.arrays.contains("score") ||
        !loaded.arrays.contains("strength") ||
        !same_generation(*loaded.arrays.at("score"), *hot->storage->scores) ||
        !same_generation(*loaded.arrays.at("strength"), *hot->storage->strengths))
        throw CorruptVrsBlock("cold parent numeric bundle differs");
    return std::shared_ptr<const DurableEventBinding>(new DurableEventBinding(
        std::move(hot), std::move(store), std::move(bundle_sha256)));
}

PreparedDurableEvent DurableEventBinding::prepare(
    const PreparedEventSignalStorage& candidate,
    const std::size_t maximum_report_bytes) const {
    if (!maximum_report_bytes || candidate.parent.get() != hot->storage.get() ||
        candidate.proposal->inputs_owner.get() != hot->storage->inputs.get())
        throw std::invalid_argument("durable event requires unchanged bound topology");
    VrsArrayBundle bundle({{"score", candidate.scores}, {"strength", candidate.strengths}});
    auto saved = bundle.save(*store);
    const auto wire = report_wire(*hot, saved.bundle_sha256, candidate);
    if (wire.size() > maximum_report_bytes)
        throw std::invalid_argument("event report exceeds capacity; candidate retained without commit");
    const auto report_sha = digest(wire);
    const auto name = report_sha + ".event.bin";
    store->publish_immutable(name, wire);
    auto prepared_hot = hot->prepare(candidate, report_sha);
    auto next = std::shared_ptr<const DurableEventBinding>(new DurableEventBinding(
        prepared_hot.next_binding, store, saved.bundle_sha256));
    return {std::move(prepared_hot), std::move(next), report_sha,
        store->root() / name, wire.size(), std::move(saved), false};
}

PreparedDurableEvent DurableEventBinding::restore(
    std::string report_sha256, const std::size_t maximum_raw_bytes,
    const std::size_t maximum_report_bytes,
    const std::size_t maximum_manifest_bytes) const {
    if (!digest_id(report_sha256) || !maximum_report_bytes)
        throw std::invalid_argument("bounded cold report capacity required");
    const auto name = report_sha256 + ".event.bin";
    auto wire = store->read_immutable(name, maximum_report_bytes);
    if (wire.size() > maximum_report_bytes || digest(wire) != report_sha256)
        throw CorruptVrsBlock("event report size or digest mismatch");
    const auto report = parse_report(*hot, wire);
    auto bundle = VrsArrayBundle::load(*store, report.bundle_sha256,
        maximum_raw_bytes, maximum_manifest_bytes);
    if (bundle.arrays.size() != 2 || !bundle.arrays.contains("score") ||
        !bundle.arrays.contains("strength"))
        throw CorruptVrsBlock("event array set changed");
    auto storage = hot->storage->restore_numeric_snapshot(report_sha256,
        bundle.arrays.at("score"), bundle.arrays.at("strength"));
    auto prepared_hot = hot->prepare_storage(std::move(storage), report_sha256);
    auto next = std::shared_ptr<const DurableEventBinding>(new DurableEventBinding(
        prepared_hot.next_binding, store, report.bundle_sha256));
    return {std::move(prepared_hot), std::move(next), report_sha256,
        store->root() / name, wire.size(), std::nullopt, true};
}

}  // namespace swegca::world
