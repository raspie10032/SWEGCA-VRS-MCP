#include "world/resident_observation_tree.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace swegca::world {
namespace {

constexpr std::size_t frame_bytes = 9;
constexpr std::size_t length_bytes = 4;

void put_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}
void put_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}
std::uint32_t u32(const std::vector<std::byte>& data, const std::size_t at) {
    if (at > data.size() || data.size() - at < 4) throw std::invalid_argument("truncated resident observation key");
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) value = (value << 8U) | std::to_integer<unsigned>(data[at + i]);
    return value;
}
std::uint64_t u64(const std::vector<std::byte>& data, const std::size_t at) {
    if (at > data.size() || data.size() - at < 8) throw std::invalid_argument("truncated resident observation node");
    std::uint64_t value{};
    for (std::size_t i = 0; i < 8; ++i) value = (value << 8U) | std::to_integer<unsigned>(data[at + i]);
    return value;
}

struct Frame final { char tag{}; std::size_t body{}; std::size_t stop{}; };
Frame frame(const std::vector<std::byte>& data, const std::size_t start,
            const std::size_t extent_stop) {
    if (start > extent_stop || extent_stop > data.size() || extent_stop - start < frame_bytes)
        throw std::invalid_argument("truncated resident observation node");
    const auto size = u64(data, start + 1);
    const auto body = start + frame_bytes;
    if (size > extent_stop - body) throw std::invalid_argument("resident observation node exceeds extent");
    return {static_cast<char>(std::to_integer<unsigned char>(data[start])), body,
            body + static_cast<std::size_t>(size)};
}

bool valid_utf8(const std::span<const std::byte> raw) noexcept {
    std::size_t at{};
    while (at < raw.size()) {
        const auto lead = std::to_integer<unsigned char>(raw[at++]);
        std::size_t continuation{};
        std::uint32_t value{};
        std::uint32_t minimum{};
        if (lead <= 0x7fU) continue;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            continuation = 1; value = lead & 0x1fU; minimum = 0x80U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            continuation = 2; value = lead & 0x0fU; minimum = 0x800U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            continuation = 3; value = lead & 0x07U; minimum = 0x10000U;
        } else return false;
        if (continuation > raw.size() - at) return false;
        for (std::size_t i = 0; i < continuation; ++i) {
            const auto byte = std::to_integer<unsigned char>(raw[at++]);
            if ((byte & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (byte & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}

bool canonical_integer(const std::span<const std::byte> raw) noexcept {
    if (raw.empty()) return false;
    std::size_t at{};
    if (raw.front() == std::byte{'-'}) {
        if (raw.size() == 1) return false;
        at = 1;
    }
    if (raw[at] == std::byte{'0'}) return at + 1 == raw.size();
    if (raw[at] < std::byte{'1'} || raw[at] > std::byte{'9'}) return false;
    for (++at; at < raw.size(); ++at)
        if (raw[at] < std::byte{'0'} || raw[at] > std::byte{'9'}) return false;
    return true;
}

std::vector<std::tuple<std::string, std::size_t, std::size_t>> members(
    const std::vector<std::byte>& data, std::size_t body, const std::size_t stop) {
    std::vector<std::tuple<std::string, std::size_t, std::size_t>> result;
    while (body < stop) {
        const auto size = u32(data, body); body += length_bytes;
        if (size > stop - body) throw std::invalid_argument("resident observation key exceeds extent");
        if (!valid_utf8(std::span(data).subspan(body, size)))
            throw std::invalid_argument("invalid UTF-8 resident observation key");
        std::string key(reinterpret_cast<const char*>(data.data() + body), size);
        const auto child = body + size;
        const auto parsed = frame(data, child, stop);
        result.emplace_back(std::move(key), child, parsed.stop);
        body = parsed.stop;
    }
    return result;
}

std::size_t validate(const std::vector<std::byte>& data, const std::size_t start,
                     const std::size_t stop,
                     const std::vector<ResidentObservationExternal>& externals) {
    const auto parsed = frame(data, start, stop);
    const auto size = parsed.stop - parsed.body;
    switch (parsed.tag) {
    case 'N': if (size != 0) throw std::invalid_argument("invalid null node"); break;
    case 'B':
        if (size != 1 || (data[parsed.body] != std::byte{0} && data[parsed.body] != std::byte{1}))
            throw std::invalid_argument("invalid bool node");
        break;
    case 'I': {
        if (!canonical_integer(std::span(data).subspan(parsed.body, size)))
            throw std::invalid_argument("invalid integer node");
        break;
    }
    case 'F': if (size != 8) throw std::invalid_argument("invalid float node"); break;
    case 'S':
        if (!valid_utf8(std::span(data).subspan(parsed.body, size)))
            throw std::invalid_argument("invalid UTF-8 string node");
        break;
    case 'X':
        if (size != 4 || u32(data, parsed.body) >= externals.size())
            throw std::invalid_argument("invalid external observation reference");
        break;
    case 'T': {
        auto at = parsed.body;
        while (at < parsed.stop) at = validate(data, at, parsed.stop, externals);
        break;
    }
    case 'M': {
        std::set<std::string, std::less<>> keys;
        for (const auto& [key, child, child_stop] : members(data, parsed.body, parsed.stop)) {
            if (!keys.insert(key).second) throw std::invalid_argument("duplicate resident observation key");
            if (validate(data, child, child_stop, externals) != child_stop)
                throw std::invalid_argument("invalid resident observation child");
        }
        break;
    }
    default: throw std::invalid_argument("unknown resident observation node");
    }
    return parsed.stop;
}

JsonValue decode(const std::vector<std::byte>& data, const std::size_t start,
                 const std::size_t stop,
                 const std::vector<ResidentObservationExternal>& externals) {
    const auto parsed = frame(data, start, stop);
    switch (parsed.tag) {
    case 'N': return nullptr;
    case 'B': return data[parsed.body] == std::byte{1};
    case 'I': {
        std::string raw(reinterpret_cast<const char*>(data.data() + parsed.body), parsed.stop - parsed.body);
        try { return static_cast<std::int64_t>(std::stoll(raw)); }
        catch (const std::out_of_range&) { return JsonInteger{std::move(raw)}; }
    }
    case 'F': {
        std::uint64_t bits{};
        for (std::size_t i = 0; i < 8; ++i) bits = (bits << 8U) | std::to_integer<unsigned>(data[parsed.body + i]);
        return std::bit_cast<double>(bits);
    }
    case 'S': return std::string(reinterpret_cast<const char*>(data.data() + parsed.body), parsed.stop - parsed.body);
    case 'X': {
        const auto& external = externals.at(u32(data, parsed.body));
        if (const auto* text = std::get_if<std::shared_ptr<const CompressedText>>(&external))
            return (*text)->value();
        JsonValue::Array values;
        for (const auto number : std::get<std::shared_ptr<const PackedFloatTuple>>(external)->value())
            values.emplace_back(number);
        return values;
    }
    case 'T': {
        JsonValue::Array values;
        auto at = parsed.body;
        while (at < parsed.stop) {
            const auto child = frame(data, at, parsed.stop);
            values.push_back(decode(data, at, child.stop, externals));
            at = child.stop;
        }
        return values;
    }
    case 'M': {
        JsonValue::Object values;
        for (const auto& [key, child, child_stop] : members(data, parsed.body, parsed.stop))
            values.emplace(key, decode(data, child, child_stop, externals));
        return values;
    }
    default: throw std::invalid_argument("unknown admitted resident observation node");
    }
}

struct Writer final {
    std::vector<std::byte> output;
    std::vector<ResidentObservationExternal> externals;
    std::optional<CompressionPolicy> policy;

    void write(const JsonValue& value) {
        const auto start = output.size();
        output.resize(start + frame_bytes);
        char tag{};
        if (std::holds_alternative<std::nullptr_t>(value.storage())) tag = 'N';
        else if (const auto* item = std::get_if<bool>(&value.storage())) {
            tag = 'B'; output.push_back(*item ? std::byte{1} : std::byte{0});
        } else if (const auto* item = std::get_if<std::int64_t>(&value.storage())) {
            tag = 'I'; const auto raw = std::to_string(*item);
            output.insert(output.end(), reinterpret_cast<const std::byte*>(raw.data()),
                          reinterpret_cast<const std::byte*>(raw.data() + raw.size()));
        } else if (const auto* item = std::get_if<JsonInteger>(&value.storage())) {
            tag = 'I'; output.insert(output.end(),
                reinterpret_cast<const std::byte*>(item->value.data()),
                reinterpret_cast<const std::byte*>(item->value.data() + item->value.size()));
        } else if (const auto* item = std::get_if<double>(&value.storage())) {
            tag = 'F'; const auto bits = std::bit_cast<std::uint64_t>(*item); put_u64(output, bits);
        } else if (const auto* item = std::get_if<std::string>(&value.storage())) {
            if (policy && item->size() >= policy->minimum_utf8_bytes) {
                const auto bytes = std::as_bytes(std::span(item->data(), item->size()));
                auto blob = LosslessBlob::build(bytes, policy->codec, policy->level, policy->block_bytes);
                if (blob->resident_size_estimate() < item->size()) {
                    tag = 'X'; put_u32(output, static_cast<std::uint32_t>(externals.size()));
                    externals.emplace_back(std::make_shared<const CompressedText>(CompressedText{std::move(blob)}));
                }
            }
            if (!tag) {
                tag = 'S'; output.insert(output.end(),
                    reinterpret_cast<const std::byte*>(item->data()),
                    reinterpret_cast<const std::byte*>(item->data() + item->size()));
            }
        } else if (value.is_array()) {
            const auto& array = value.as_array();
            bool packed{};
            if (policy && array.size() >= 64 && std::ranges::all_of(array, [](const auto& child) {
                    return std::holds_alternative<double>(child.storage());
                })) {
                std::vector<double> numbers;
                for (const auto& child : array) numbers.push_back(std::get<double>(child.storage()));
                auto tuple = std::make_shared<const PackedFloatTuple>(PackedFloatTuple::build(
                    numbers, policy->codec, policy->level, policy->block_bytes));
                if (tuple->blob->resident_size_estimate() < numbers.size() * sizeof(double)) {
                    tag = 'X'; put_u32(output, static_cast<std::uint32_t>(externals.size()));
                    externals.emplace_back(std::move(tuple)); packed = true;
                }
            }
            if (!packed) { tag = 'T'; for (const auto& child : array) write(child); }
        } else {
            tag = 'M';
            for (const auto& [key, child] : value.as_object()) {
                if (key.size() > std::numeric_limits<std::uint32_t>::max())
                    throw std::length_error("resident observation key too large");
                put_u32(output, static_cast<std::uint32_t>(key.size()));
                output.insert(output.end(), reinterpret_cast<const std::byte*>(key.data()),
                              reinterpret_cast<const std::byte*>(key.data() + key.size()));
                write(child);
            }
        }
        const auto size = static_cast<std::uint64_t>(output.size() - start - frame_bytes);
        output[start] = static_cast<std::byte>(tag);
        for (std::size_t i = 0; i < 8; ++i)
            output[start + 1 + i] = static_cast<std::byte>((size >> (56U - 8U * i)) & 0xffU);
    }
};

}  // namespace

PackedResidentObservation::PackedResidentObservation(
    std::shared_ptr<const std::vector<std::byte>> blob,
    std::vector<ResidentObservationExternal> externals,
    const std::size_t start, const std::size_t stop, const bool indexed)
    : PackedResidentObservation(std::move(blob),
          std::make_shared<const std::vector<ResidentObservationExternal>>(std::move(externals)),
          start, stop, indexed) {}

PackedResidentObservation::PackedResidentObservation(
    std::shared_ptr<const std::vector<std::byte>> blob,
    std::shared_ptr<const std::vector<ResidentObservationExternal>> externals,
    const std::size_t start, std::size_t stop, const bool indexed)
    : blob_(std::move(blob)), externals_(std::move(externals)), start_(start), stop_(stop) {
    if (!blob_ || !externals_) throw std::invalid_argument("immutable resident observation storage required");
    for (const auto& external : *externals_)
        std::visit([](const auto& value) {
            if (!value) throw std::invalid_argument("invalid resident observation external");
        }, external);
    if (stop_ == 0) stop_ = blob_->size();
    const auto root = frame(*blob_, start_, stop_);
    if (root.tag != 'M' || root.stop != stop_ ||
        validate(*blob_, start_, stop_, *externals_) != stop_)
        throw std::invalid_argument("resident observation root must be a complete mapping");
    if (indexed) {
        auto values = std::make_shared<std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>>>();
        for (const auto& [key, child, child_stop] : members(*blob_, root.body, root.stop))
            values->emplace(key, std::pair{child, child_stop});
        fields_ = std::move(values);
    }
}

PackedResidentObservation PackedResidentObservation::request_view() const {
    return PackedResidentObservation(blob_, externals_, start_, stop_, true);
}
std::vector<std::string> PackedResidentObservation::keys() const {
    std::vector<std::string> result;
    if (fields_) for (const auto& [key, unused] : *fields_) {
        static_cast<void>(unused); result.push_back(key);
    } else {
        const auto root = frame(*blob_, start_, stop_);
        for (const auto& [key, unused, unused_stop] : members(*blob_, root.body, root.stop)) {
            static_cast<void>(unused); static_cast<void>(unused_stop); result.push_back(key);
        }
    }
    return result;
}
std::size_t PackedResidentObservation::size() const { return keys().size(); }
bool PackedResidentObservation::contains(const std::string_view key) const {
    if (fields_) return fields_->contains(key);
    const auto values = keys();
    return std::ranges::find(values, key) != values.end();
}
JsonValue PackedResidentObservation::value(const std::string_view key) const {
    if (fields_) {
        const auto [start, stop] = fields_->at(key);
        return decode(*blob_, start, stop, *externals_);
    }
    const auto root = frame(*blob_, start_, stop_);
    for (const auto& [candidate, start, stop] : members(*blob_, root.body, root.stop))
        if (candidate == key) return decode(*blob_, start, stop, *externals_);
    throw std::out_of_range(std::string(key));
}
PackedResidentObservation PackedResidentObservation::object_view(const std::string_view key) const {
    std::size_t start{}, stop{};
    if (fields_) std::tie(start, stop) = fields_->at(key);
    else {
        const auto root = frame(*blob_, start_, stop_);
        bool found{};
        for (const auto& [candidate, child, child_stop] : members(*blob_, root.body, root.stop))
            if (candidate == key) { start = child; stop = child_stop; found = true; break; }
        if (!found) throw std::out_of_range(std::string(key));
    }
    if (frame(*blob_, start, stop).tag != 'M') throw std::invalid_argument("resident child is not a mapping");
    return PackedResidentObservation(blob_, externals_, start, stop, true);
}
std::span<const std::byte> PackedResidentObservation::bytes() const noexcept {
    return std::span(*blob_).subspan(start_, stop_ - start_);
}
const std::vector<ResidentObservationExternal>& PackedResidentObservation::externals() const noexcept {
    return *externals_;
}

PackedResidentObservation pack_observation(
    const JsonValue::Object& observation, std::optional<CompressionPolicy> policy) {
    if (policy) policy->validate();
    Writer writer;
    writer.policy = policy;
    writer.write(JsonValue(observation));
    return PackedResidentObservation(
        std::make_shared<const std::vector<std::byte>>(std::move(writer.output)),
        std::move(writer.externals));
}

}  // namespace swegca::world
