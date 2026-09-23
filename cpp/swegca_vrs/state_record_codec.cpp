#include "swegca_vrs/state_record_codec.hpp"

#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/journal_format.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace swegca::vrs {

namespace {

using Bytes = part_tree::Bytes;

constexpr std::array<std::byte, 4> root_magic{std::byte{'S'}, std::byte{'W'}, std::byte{'S'},
                                              std::byte{'R'}};
constexpr std::uint16_t root_version = 1;
constexpr std::uint64_t no_limit = std::numeric_limits<std::uint64_t>::max();
// The tensor header's byte count, the last of its four u64 fields.
constexpr std::size_t tensor_byte_count_offset = 3 + 3 * 8;

// The emitter's marker order: each section's marker, and inside each tensor
// section the `tensor_chunks` marker after its header.
constexpr std::array<StateContentSection, 11> marker_order{
    StateContentSection::prefix,          StateContentSection::semantic_tensor,
    StateContentSection::tensor_chunks,   StateContentSection::executive_tensor,
    StateContentSection::tensor_chunks,   StateContentSection::scratch_tensor,
    StateContentSection::tensor_chunks,   StateContentSection::entities,
    StateContentSection::relations,       StateContentSection::evidence,
    StateContentSection::final_fields,
};

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
[[noreturn]] void fail(const char* what) { throw std::invalid_argument(what); }

// Lowercase hex after a prefix, without allocating.
// Lineage: native mechanism — a content address is its record kind's prefix
// and the lowercase hex of a SHA-256 digest, as experience parts are.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
template <std::size_t N>
std::array<char, N> address_of(std::string_view prefix, const DigestBytes& digest) noexcept {
    constexpr std::string_view hex = "0123456789abcdef";
    std::array<char, N> out{};
    std::copy(prefix.begin(), prefix.end(), out.begin());
    auto at = prefix.size();
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        out[at++] = hex[value >> 4];
        out[at++] = hex[value & 0x0f];
    }
    return out;
}

// SWEGCA: user@2026-09-22:60-61
void put_u64(std::byte* out, std::uint64_t value) noexcept {
    for (std::size_t at = 0; at < 8; ++at)
        out[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
}

// SWEGCA: user@2026-09-22:60-61
std::uint64_t get_u64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t at = 0; at < 8; ++at)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[at])) << (8 * at);
    return value;
}

// A tensor section's length: its header plus its data, without overflow.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
std::uint64_t section_length(StateSectionForm form, std::uint64_t data_bytes) {
    if (form == StateSectionForm::plain) return data_bytes;
    if (data_bytes > no_limit - state_tensor_header_bytes) fail("state_codec_section_too_long");
    return data_bytes + state_tensor_header_bytes;
}

// A tensor header as the native tensor constructor accepts it: this
// section's partition, a known scalar type, little-endian order, nonzero
// slots and width (batches may be zero), and a byte count equal to the
// checked product of the shape and the scalar width. Returns that byte
// count, so malformed metadata stops before any part is read.
// Lineage: direct — the same checks as checked_elements/checked_bytes
// (native_tensor.cpp), applied to the stored header.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
std::uint64_t check_tensor_header(std::span<const std::byte> header, std::size_t section) {
    const auto at = [header](std::size_t offset) {
        return get_u64(header.subspan(offset, 8));
    };
    const auto partition = std::to_integer<std::size_t>(header[0]);
    const auto scalar = std::to_integer<std::uint8_t>(header[1]);
    const auto order = std::to_integer<std::uint8_t>(header[2]);
    const auto batches = at(3);
    const auto slots = at(11);
    const auto width = at(19);
    const auto byte_count = at(tensor_byte_count_offset);
    if (partition != section ||
        scalar < static_cast<std::uint8_t>(ScalarType::bfloat16) ||
        scalar > static_cast<std::uint8_t>(ScalarType::float64) ||
        order != static_cast<std::uint8_t>(ByteOrder::little_endian) || slots == 0 || width == 0 ||
        batches > no_limit / slots || batches * slots > no_limit / width)
        fail("state_codec_tensor_header_invalid");
    const auto elements = batches * slots * width;
    const auto scalar_bytes = scalar_width(static_cast<ScalarType>(scalar));
    if (elements > std::numeric_limits<std::size_t>::max() / scalar_bytes ||
        elements * scalar_bytes != byte_count)
        fail("state_codec_tensor_header_invalid");
    return byte_count;
}

// The root's consistency: a tensor section holds at least its header, and
// the section lengths sum to the stream length without overflow.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void check_root(const StateRootFields& root) {
    if (root.content_digest == DigestBytes{}) fail("state_codec_root_invalid");
    std::uint64_t total = 0;
    for (std::size_t section = 0; section < state_section_count; ++section) {
        const auto length = root.sections[section].section_bytes;
        if (state_section_form(section) == StateSectionForm::tensor &&
            length < state_tensor_header_bytes)
            fail("state_codec_root_invalid");
        if (length > no_limit - total) fail("state_codec_root_invalid");
        total += length;
    }
    if (total != root.stream_bytes) fail("state_codec_root_invalid");
}

// Cuts the canonical stream as the emitter marks it. A tensor section's bytes
// after its header are always parted; a plain section's bytes are one blob
// field, inline up to the inline size, otherwise parted. Parted bytes are cut
// at fixed part-size offsets from the section's data start; the last part may
// be shorter. A buffer holds a part only while it is incomplete, so a plain
// section emits nothing until it passes one part and an inline one never
// does; a tensor's stored chunks arrive whole at part boundaries and are cut
// without a copy.
// Lineage: native mechanism — the plan's part tree over the canonical stream
// the author's state hash binds; section boundaries are C++ storage choices.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
class SectionSplitter final {
public:
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    SectionSplitter(const AllocationContext& memory, StatePartSink emit)
        : memory_(memory), emit_(emit), buffer_(memory.allocator<std::byte>()),
          lists_(memory.allocator<Bytes>()) {}

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    void mark(StateContentSection marker) {
        if (next_marker_ == marker_order.size() || marker_order[next_marker_] != marker)
            fail("state_codec_section_order");
        ++next_marker_;
        if (marker == StateContentSection::tensor_chunks) {
            start_tensor_data();
            return;
        }
        if (section_) close();
        open(closed_);
    }

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    void write(std::span<const std::byte> bytes) {
        if (!section_) fail("state_codec_section_order");
        if (!in_data_) {
            if (bytes.size() > state_tensor_header_bytes - header_size_)
                fail("state_codec_tensor_header_invalid");
            std::copy(bytes.begin(), bytes.end(), header_.begin() + header_size_);
            header_size_ += bytes.size();
            return;
        }
        if (bytes.size() > limit_ - data_bytes_) fail("state_codec_section_too_long");
        if (whole_) whole_->update(bytes);
        while (!bytes.empty()) {
            const auto whole = std::min<std::uint64_t>(part_tree::part_bytes, limit_ - data_bytes_);
            if (buffer_.empty() && bytes.size() >= whole) {
                emit_part(bytes.first(static_cast<std::size_t>(whole)));
                bytes = bytes.subspan(static_cast<std::size_t>(whole));
                data_bytes_ += whole;
                continue;
            }
            const auto take = std::min(part_tree::part_bytes - buffer_.size(), bytes.size());
            buffer_.insert(buffer_.end(), bytes.begin(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(take));
            bytes = bytes.subspan(take);
            data_bytes_ += take;
            if (buffer_.size() == part_tree::part_bytes) flush();
        }
    }

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    StateRootFields finish(const DigestBytes& content_digest) {
        if (next_marker_ != marker_order.size() || !section_) fail("state_codec_section_order");
        close();
        root_.content_digest = content_digest;
        std::uint64_t total = 0;
        for (const auto& entry : root_.sections) {
            if (entry.section_bytes > no_limit - total) fail("state_codec_section_too_long");
            total += entry.section_bytes;
        }
        root_.stream_bytes = total;
        check_root(root_);
        return root_;
    }

private:
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    void open(std::size_t section) {
        section_ = section;
        in_data_ = state_section_form(section) == StateSectionForm::plain;
        header_size_ = 0;
        data_bytes_ = 0;
        limit_ = no_limit;
        lists_.clear();
        lists_.emplace_back(memory_.allocator<std::byte>());
        // A plain section's parted field names the digest of all its bytes.
        if (in_data_)
            whole_.emplace();
        else
            whole_.reset();
    }

    // The header is complete and names this section's partition; its byte
    // count bounds the data that follows.
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    void start_tensor_data() {
        if (!section_ || in_data_ || header_size_ != state_tensor_header_bytes)
            fail("state_codec_tensor_header_invalid");
        in_data_ = true;
        limit_ = check_tensor_header(header_, *section_);
        lists_.back().reserve(static_cast<std::size_t>(
            part_tree::part_levels(limit_).counts[0] * digest256_width));
    }

    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    void emit_part(std::span<const std::byte> part) {
        const auto digest = Sha256::of(part);
        lists_.back().insert(lists_.back().end(), digest.begin(), digest.end());
        emit_(digest, part);
    }

    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    void flush() {
        emit_part(buffer_);
        buffer_.clear();
    }

    // Emits the section's last part, its upper digest lists and its
    // descriptor, and records the descriptor in the root. A plain section of
    // at most the inline size is one inline blob field and emits no part.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    void close() {
        const auto section = *section_;
        const auto form = state_section_form(section);
        if (!in_data_) fail("state_codec_tensor_header_invalid");
        if (form == StateSectionForm::tensor && data_bytes_ != limit_)
            fail("state_codec_tensor_length_mismatch");

        Bytes descriptor(memory_.allocator<std::byte>());
        journal::ByteWriter writer(descriptor);
        if (form == StateSectionForm::plain && data_bytes_ <= part_tree::inline_top_bytes) {
            if (!lists_.back().empty() || buffer_.size() != data_bytes_)
                fail("state_codec_part_count_mismatch");
            descriptor.reserve(blob_field_inline_encoded_bytes - part_tree::inline_top_bytes +
                               buffer_.size());
            write_inline_blob_field(writer, buffer_);
            buffer_.clear();
        } else {
            if (!buffer_.empty()) flush();
            const auto levels = part_tree::part_levels(data_bytes_);
            if (lists_.back().size() != levels.counts[0] * digest256_width)
                fail("state_codec_part_count_mismatch");
            const auto top = part_tree::append_upper_levels(
                memory_, levels, lists_,
                [this](const DigestBytes& digest, std::span<const std::byte> part) {
                    emit_(digest, part);
                });
            if (form == StateSectionForm::plain) {
                descriptor.reserve(blob_field_parted_encoded_bytes -
                                   part_tree::max_top_digests * digest256_width + top.size());
                write_parted_blob_field(writer, data_bytes_, whole_->finish(), levels.depth, top);
            } else {
                descriptor.reserve(1 + 8 + state_tensor_header_bytes + 1 + 4 + top.size());
                writer.u8(static_cast<std::uint8_t>(StateSectionForm::tensor));
                writer.u64(data_bytes_);
                writer.raw(header_);
                writer.u8(levels.depth);
                writer.u32(static_cast<std::uint32_t>(top.size() / digest256_width));
                writer.raw(top);
            }
        }
        const auto digest = Sha256::of(descriptor);
        emit_(digest, descriptor);

        root_.sections[section] = StateSectionEntry{section_length(form, data_bytes_), digest};
        closed_ = section + 1;
        section_.reset();
        lists_.clear();
    }

    AllocationContext memory_;
    StatePartSink emit_;
    std::size_t next_marker_ = 0;
    std::optional<std::size_t> section_;
    std::size_t closed_ = 0;
    bool in_data_ = false;
    std::array<std::byte, state_tensor_header_bytes> header_{};
    std::size_t header_size_ = 0;
    std::uint64_t data_bytes_ = 0;
    std::uint64_t limit_ = no_limit;
    Bytes buffer_;
    part_tree::OwnedLists lists_;
    std::optional<Sha256> whole_;
    StateRootFields root_{};
};

}  // namespace

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
std::array<char, state_part_address_bytes> state_part_address(const DigestBytes& digest) noexcept {
    return address_of<state_part_address_bytes>(state_part_address_prefix, digest);
}

// A root is addressed by the content digest it reconstructs, so every
// publication of identical content names one root.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
std::array<char, state_root_address_bytes> state_root_address(
    const DigestBytes& content_digest) noexcept {
    return address_of<state_root_address_bytes>(state_root_address_prefix, content_digest);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
StateRootFields split_state_content(const CognitiveState& state, const AllocationContext& memory,
                                    StatePartSink emit) {
    SectionSplitter splitter(memory, emit);
    auto on_bytes = [&splitter](std::span<const std::byte> bytes) { splitter.write(bytes); };
    auto on_section = [&splitter](StateContentSection section) { splitter.mark(section); };
    state.for_each_content_chunk(StateContentSink(on_bytes), StateContentSectionSink(on_section));
    return splitter.finish(state.content_digest().bytes());
}

// Lineage: native mechanism — a fixed-size root binding the eight section
// descriptors to the content digest; fixed-width little-endian fields.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
std::array<std::byte, state_root_payload_bytes> encode_state_root(const StateRootFields& root) {
    check_root(root);
    std::array<std::byte, state_root_payload_bytes> out{};
    auto* at = out.data();
    at = std::copy(root_magic.begin(), root_magic.end(), at);
    *at++ = static_cast<std::byte>(root_version & 0xff);
    *at++ = static_cast<std::byte>(root_version >> 8);
    at = std::copy(root.content_digest.begin(), root.content_digest.end(), at);
    put_u64(at, root.stream_bytes);
    at += 8;
    for (const auto& entry : root.sections) {
        put_u64(at, entry.section_bytes);
        at += 8;
        at = std::copy(entry.descriptor.begin(), entry.descriptor.end(), at);
    }
    return out;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
StateRootFields decode_state_root(std::span<const std::byte> payload) {
    if (payload.size() != state_root_payload_bytes) fail("state_codec_root_invalid");
    journal::ByteReader reader(payload);
    const auto magic = reader.raw(root_magic.size());
    if (!std::equal(magic.begin(), magic.end(), root_magic.begin()) ||
        reader.u16() != root_version)
        fail("state_codec_root_invalid");
    StateRootFields root;
    root.content_digest = reader.digest();
    root.stream_bytes = reader.u64();
    for (auto& entry : root.sections) {
        entry.section_bytes = reader.u64();
        entry.descriptor = reader.digest();
    }
    check_root(root);
    return root;
}

// Lineage: native mechanism — reads a descriptor only in its one form, with
// its depth and top count those its length gives, so any other encoding
// fails before a part is read.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
StateSectionDescriptor decode_state_descriptor(std::span<const std::byte> payload,
                                               std::size_t section, std::uint64_t section_bytes) {
    constexpr const char* invalid = "state_codec_descriptor_invalid";
    if (section >= state_section_count || payload.size() > state_descriptor_max_bytes) fail(invalid);
    journal::ByteReader reader(payload);
    StateSectionDescriptor out;
    out.form = state_section_form(section);
    if (out.form == StateSectionForm::plain) {
        out.data = read_blob_field(reader, invalid);
    } else {
        if (reader.u8() != static_cast<std::uint8_t>(StateSectionForm::tensor)) fail(invalid);
        out.data.size = reader.u64();
        out.tensor_header = reader.raw(state_tensor_header_bytes);
        if (check_tensor_header(out.tensor_header, section) != out.data.size) fail(invalid);
        out.data.depth = reader.u8();
        const auto count = reader.u32();
        const auto levels = part_tree::part_levels(out.data.size);
        if (out.data.depth != levels.depth || count != levels.counts[levels.depth - 1] ||
            reader.remaining() != static_cast<std::size_t>(count) * digest256_width)
            fail(invalid);
        out.data.top_digests = reader.raw(reader.remaining());
    }
    if (reader.remaining() != 0 || section_length(out.form, out.data.size) != section_bytes)
        fail(invalid);
    return out;
}

// Lineage: native mechanism — the length each part must have from its place
// in the tree, so a part of any other size fails.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
std::uint64_t state_part_length(std::uint64_t data_bytes, std::uint8_t level, std::uint64_t index) {
    const auto levels = part_tree::part_levels(data_bytes);
    if (level >= levels.depth || index >= levels.counts[level]) fail("state_codec_part_invalid");
    return blob_part_length(levels, data_bytes, level, index);
}

}  // namespace swegca::vrs
