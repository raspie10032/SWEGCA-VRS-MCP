#include "journal_frame.hpp"

#include "digest.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <zlib.h>

namespace swegca::vrs {
namespace {

constexpr std::uint64_t maximum_payload_bytes = 64ULL * 1024 * 1024;
constexpr std::size_t checksum_bytes = 32;
constexpr std::size_t header_bytes = 8;

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:31-34
std::uint64_t little_endian_length(std::span<const std::byte> frame) {
    if (frame.size() < header_bytes) throw std::runtime_error("native_vrs_journal_truncated");
    std::uint64_t size = 0;
    for (std::size_t i = 0; i < header_bytes; ++i)
        size |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(frame[i])) << (i * 8);
    return size;
}

class FrameDeflater {
public:
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
    FrameDeflater() : output_(header_bytes, std::byte{0}) {
        if (deflateInit(&stream_, 3) != Z_OK)
            throw std::runtime_error("native_vrs_frame_invalid");
    }

    FrameDeflater(const FrameDeflater&) = delete;
    FrameDeflater& operator=(const FrameDeflater&) = delete;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
    ~FrameDeflater() { deflateEnd(&stream_); }

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
    void write(std::string_view bytes) {
        if (bytes.size() > std::numeric_limits<uInt>::max())
            throw std::runtime_error("native_vrs_frame_too_large");
        stream_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
        stream_.avail_in = static_cast<uInt>(bytes.size());
        while (stream_.avail_in) {
            stream_.next_out = reinterpret_cast<Bytef*>(buffer_.data());
            stream_.avail_out = static_cast<uInt>(buffer_.size());
            if (deflate(&stream_, Z_NO_FLUSH) != Z_OK)
                throw std::runtime_error("native_vrs_frame_invalid");
            append_output();
        }
    }

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
    std::vector<std::byte> finish() {
        int code = Z_OK;
        do {
            stream_.next_out = reinterpret_cast<Bytef*>(buffer_.data());
            stream_.avail_out = static_cast<uInt>(buffer_.size());
            code = deflate(&stream_, Z_FINISH);
            if (code != Z_OK && code != Z_STREAM_END)
                throw std::runtime_error("native_vrs_frame_invalid");
            append_output();
        } while (code != Z_STREAM_END);
        const auto payload_size = output_.size() - header_bytes;
        for (std::size_t i = 0; i < header_bytes; ++i)
            output_[i] = static_cast<std::byte>((payload_size >> (i * 8)) & 0xff);
        const auto digest = hash_.finish();
        output_.insert(output_.end(), digest.begin(), digest.end());
        return std::move(output_);
    }

private:
    z_stream stream_{};
    std::array<std::byte, 65536> buffer_{};
    std::vector<std::byte> output_;
    Sha256 hash_;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
    void append_output() {
        const auto count = buffer_.size() - stream_.avail_out;
        if (output_.size() - header_bytes + count > maximum_payload_bytes)
            throw std::runtime_error("native_vrs_frame_too_large");
        const auto produced = std::span<const std::byte>(buffer_.data(), count);
        hash_.update(produced);
        output_.insert(output_.end(), produced.begin(), produced.end());
    }
};

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:166-176
JournalRow parse_journal_row(std::string_view encoded) {
    Json value;
    try {
        value = Json::parse(encoded);
    } catch (const std::exception&) {
        throw std::runtime_error("native_vrs_frame_invalid");
    }
    const auto* fields = std::get_if<Json::Array>(&value.data);
    if (!fields || fields->size() != 5)
        throw std::runtime_error("native_vrs_row_invalid");
    try {
        return JournalRow{fields->at(0).integer(), fields->at(1).string(),
            fields->at(2).string(), fields->at(3).string(), fields->at(4).string()};
    } catch (const std::exception&) {
        throw std::runtime_error("native_vrs_row_invalid");
    }
}

class FrameRowParser {
public:
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:166-177
    explicit FrameRowParser(const std::function<void(JournalRow&&)>* visitor)
        : visitor_(visitor) {}

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:166-177
    void feed(std::string_view bytes) {
        for (const char byte : bytes) consume(byte);
    }

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:166-177
    void finish() const {
        if (state_ != State::complete)
            throw std::runtime_error("native_vrs_frame_invalid");
    }

private:
    enum class State { prefix, row_or_close, row, after_row, suffix, complete };
    static constexpr std::string_view prefix = "{\"rows\":[";
    static constexpr std::string_view suffix = "],\"schema\":\"swegca-vrs2-frame-v1\"}";
    State state_ = State::prefix;
    const std::function<void(JournalRow&&)>* visitor_;
    std::size_t position_ = 0;
    std::size_t bracket_depth_ = 0;
    bool allow_close_ = true;
    bool in_string_ = false;
    bool escaped_ = false;
    std::string row_;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:166-177
    void consume(char byte) {
        switch (state_) {
            case State::prefix:
                if (byte != prefix[position_++])
                    throw std::runtime_error("native_vrs_frame_invalid");
                if (position_ == prefix.size()) {
                    position_ = 0;
                    state_ = State::row_or_close;
                }
                return;
            case State::row_or_close:
                if (byte == ']' && allow_close_) {
                    state_ = State::suffix;
                    position_ = 1;
                    return;
                }
                if (byte != '[') throw std::runtime_error("native_vrs_frame_invalid");
                row_.clear();
                row_.push_back(byte);
                bracket_depth_ = 1;
                in_string_ = false;
                escaped_ = false;
                state_ = State::row;
                return;
            case State::row:
                row_.push_back(byte);
                if (in_string_) {
                    if (escaped_) escaped_ = false;
                    else if (byte == '\\') escaped_ = true;
                    else if (byte == '"') in_string_ = false;
                } else if (byte == '"') {
                    in_string_ = true;
                } else if (byte == '[') {
                    ++bracket_depth_;
                } else if (byte == ']') {
                    if (--bracket_depth_ == 0) {
                        auto row = parse_journal_row(row_);
                        if (visitor_) (*visitor_)(std::move(row));
                        row_.clear();
                        state_ = State::after_row;
                    }
                }
                return;
            case State::after_row:
                if (byte == ',') {
                    allow_close_ = false;
                    state_ = State::row_or_close;
                } else if (byte == ']') {
                    state_ = State::suffix;
                    position_ = 1;
                } else throw std::runtime_error("native_vrs_frame_invalid");
                return;
            case State::suffix:
                if (position_ >= suffix.size() || byte != suffix[position_++])
                    throw std::runtime_error("native_vrs_frame_invalid");
                if (position_ == suffix.size()) state_ = State::complete;
                return;
            case State::complete:
                throw std::runtime_error("native_vrs_frame_invalid");
        }
    }
};

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:166-177
void inflate_rows(std::span<const std::byte> compressed,
                  const std::function<void(JournalRow&&)>* visitor) {
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) throw std::runtime_error("native_vrs_frame_invalid");
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    FrameRowParser parser(visitor);
    std::array<char, 65536> buffer{};
    int result = Z_OK;
    try {
        do {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            const auto prior_input = stream.avail_in;
            result = inflate(&stream, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END)
                throw std::runtime_error("native_vrs_frame_invalid");
            parser.feed(std::string_view(buffer.data(), buffer.size() - stream.avail_out));
            if (result == Z_OK && stream.avail_out == buffer.size() &&
                stream.avail_in == prior_input)
                throw std::runtime_error("native_vrs_frame_invalid");
        } while (result != Z_STREAM_END);
        parser.finish();
    } catch (...) {
        inflateEnd(&stream);
        throw;
    }
    inflateEnd(&stream);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:153-177
std::span<const std::byte> checked_payload(std::span<const std::byte> frame) {
    const auto length = little_endian_length(frame);
    if (length > maximum_payload_bytes) throw std::runtime_error("native_vrs_frame_too_large");
    if (frame.size() != header_bytes + length + checksum_bytes)
        throw std::runtime_error("native_vrs_journal_truncated");
    const auto payload = frame.subspan(header_bytes, static_cast<std::size_t>(length));
    Sha256 hash;
    hash.update(payload);
    const auto digest = hash.finish();
    if (!std::equal(digest.begin(), digest.end(), frame.begin() + header_bytes + length))
        throw std::runtime_error("native_vrs_frame_integrity_failed");
    return payload;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-134
std::vector<std::byte> encode_journal_frame(std::span<const JournalRow> rows) {
    FrameDeflater writer;
    writer.write("{\"rows\":[");
    bool first = true;
    for (const auto& row : rows) {
        if (!first) writer.write(",");
        first = false;
        const auto encoded = Json(Json::Array{
            Json(row.sequence), Json(row.request_id), Json(row.body),
            Json(row.fingerprint), Json(row.pair_id)}).canonical();
        writer.write(encoded);
    }
    writer.write("],\"schema\":\"swegca-vrs2-frame-v1\"}");
    return writer.finish();
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-234
std::vector<std::byte> encode_journal_frame(
    std::int64_t first_sequence, std::span<const PendingJournalRow> rows) {
    if (first_sequence < 1 || rows.size() > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - first_sequence + 1))
        throw std::runtime_error("native_vrs_sequence_invalid");
    FrameDeflater writer;
    writer.write("{\"rows\":[");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) writer.write(",");
        const auto encoded = Json(Json::Array{
            Json(first_sequence + static_cast<std::int64_t>(i)),
            Json(rows[i].request_id), Json(rows[i].body),
            Json(rows[i].fingerprint), Json(rows[i].pair_id)}).canonical();
        writer.write(encoded);
    }
    writer.write("],\"schema\":\"swegca-vrs2-frame-v1\"}");
    return writer.finish();
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-177
void visit_journal_frame(std::span<const std::byte> frame,
                         const std::function<void(JournalRow&&)>& visit) {
    const auto payload = checked_payload(frame);
    // The schema follows the rows in the canonical frame. Validate every row
    // and the final schema before exposing any row to a main-generation caller.
    inflate_rows(payload, nullptr);
    inflate_rows(payload, &visit);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-177
void visit_journal_frame_with_span(
    std::span<const std::byte> frame,
    const std::function<void(JournalRow&&, std::int64_t, std::int64_t)>& visit) {
    (void)visit_journal_frame_with_span_until(frame,
        [&](JournalRow&& row, std::int64_t first, std::int64_t last) {
            visit(std::move(row), first, last);
            return true;
        });
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:206-221
bool visit_journal_frame_with_span_until(
    std::span<const std::byte> frame,
    const std::function<bool(JournalRow&&, std::int64_t, std::int64_t)>& visit) {
    const auto payload = checked_payload(frame);
    std::int64_t first = 0;
    std::int64_t last = 0;
    const std::function<void(JournalRow&&)> inspect = [&](JournalRow&& row) {
        if (first == 0) {
            if (row.sequence < 1) throw std::runtime_error("native_vrs_sequence_invalid");
            first = row.sequence;
        } else if (last == std::numeric_limits<std::int64_t>::max() ||
                   row.sequence != last + 1) {
            throw std::runtime_error("native_vrs_sequence_invalid");
        }
        last = row.sequence;
    };
    inflate_rows(payload, &inspect);
    if (first == 0) return true;
    struct StopVisit {};
    const std::function<void(JournalRow&&)> deliver = [&](JournalRow&& row) {
        if (!visit(std::move(row), first, last)) throw StopVisit{};
    };
    try {
        inflate_rows(payload, &deliver);
    } catch (const StopVisit&) {
        return false;
    }
    return true;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:206-221
bool visit_journal_frame_until(std::span<const std::byte> frame,
                               const std::function<bool(JournalRow&&)>& visit) {
    struct StopVisit {};
    try {
        visit_journal_frame(frame, [&](JournalRow&& row) {
            if (!visit(std::move(row))) throw StopVisit{};
        });
    } catch (const StopVisit&) {
        return false;
    }
    return true;
}

}  // namespace swegca::vrs
