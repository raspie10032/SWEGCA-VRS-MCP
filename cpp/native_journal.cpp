#include "native_journal.hpp"

#include "json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace swegca::vrs {
namespace {

constexpr std::string_view store_schema = "swegca-vrs2-native-store-v1";
constexpr std::string_view manifest_name = "vrs-store.json";
constexpr std::string_view journal_name = "journal";
constexpr std::string_view head_name = "head.vrsj";
constexpr std::array<char, 8> file_magic{'V','R','S','2','J','N','L','1'};

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void make_private_directory(const std::filesystem::path& directory,
                            bool require_new = false) {
    const bool created = std::filesystem::create_directories(directory);
    if (require_new && !created)
        throw std::runtime_error("native_vrs_generation_exists");
    if (created)
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:99-108
std::string uuid4(bool hyphenated) {
    std::random_device random;
    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(hyphenated ? 36 : 32);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (hyphenated && (i == 4 || i == 6 || i == 8 || i == 10)) out.push_back('-');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 15]);
    }
    return out;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:70-81
Json read_manifest(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_vrs_manifest_invalid");
    const std::string content(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("native_vrs_manifest_invalid");
    try { return Json::parse(content); }
    catch (...) { throw std::runtime_error("native_vrs_manifest_invalid"); }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:70-81
bool has_schema_and_identity(const Json& manifest) {
    try {
        if (!std::holds_alternative<std::string>(manifest.at("schema").data) ||
            !std::holds_alternative<std::string>(manifest.at("identity").data))
            return false;
        return manifest.at("schema").string() == store_schema;
    } catch (...) { return false; }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:99-108
void write_new_manifest(const std::filesystem::path& path,
                        std::string_view identity, std::string_view generation) {
    Json::Object manifest;
    manifest.emplace("schema", Json(std::string(store_schema)));
    manifest.emplace("identity", Json(std::string(identity)));
    manifest.emplace("generation", Json(std::string(generation)));
    const auto bytes = Json(std::move(manifest)).canonical();
    write_atomic_file(path, std::as_bytes(std::span(bytes)));
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:70-81
bool is_native_store(const std::filesystem::path& directory) {
    try {
        const auto manifest_path = directory / manifest_name;
        if (!std::filesystem::is_regular_file(manifest_path)) return false;
        return has_schema_and_identity(read_manifest(manifest_path));
    }
    catch (...) { return false; }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
NativeJournal::NativeJournal(std::filesystem::path directory, bool create,
                             bool writable, OwnerLock* owner_lock)
    : directory_(std::filesystem::weakly_canonical(std::move(directory))),
      writable_(writable), owner_lock_(owner_lock) {
    if (create || writable_) require_write_owner();
    const auto manifest_path = directory_ / manifest_name;
    const auto journal_root = directory_ / journal_name;
    if (!std::filesystem::exists(manifest_path)) {
        if (!create) throw std::runtime_error("native_vrs_store_missing");
        make_private_directory(directory_);
        identity_ = uuid4(true);
        generation_ = "g-" + uuid4(false);
        const auto target = journal_root / generation_;
        make_private_directory(target, true);
        write_atomic_file(target / head_name, std::as_bytes(std::span(file_magic)));
        write_new_manifest(manifest_path, identity_, generation_);
    }
    const auto manifest = read_manifest(manifest_path);
    if (!has_schema_and_identity(manifest) ||
        !manifest.contains("generation") ||
        !std::holds_alternative<std::string>(manifest.at("generation").data))
        throw std::runtime_error("native_vrs_manifest_invalid");
    identity_ = manifest.at("identity").string();
    generation_ = manifest.at("generation").string();
    path_ = journal_root / generation_;
    if (!std::filesystem::is_directory(path_))
        throw std::runtime_error("native_vrs_generation_missing");
    const auto head_path = path_ / head_name;
    if (!std::filesystem::exists(head_path)) {
        if (!writable_) throw std::runtime_error("native_vrs_head_missing");
        write_atomic_file(head_path, std::as_bytes(std::span(file_magic)));
    }
    scan_ = visit_journal_files(path_, writable_, [](JournalRow&&) {});
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:863-884
void NativeJournal::require_write_owner() const {
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:196-204
void NativeJournal::refresh_head() {
    scan_ = visit_journal_files(path_, false, [](JournalRow&&) {});
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:196-204
std::optional<std::pair<std::int64_t, std::string>> NativeJournal::head() const {
    if (scan_.row_count == 0) return std::nullopt;
    return std::pair{scan_.last_sequence, scan_.last_pair};
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:196-204
std::uint64_t NativeJournal::row_count() const { return scan_.row_count; }

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:206-214
void NativeJournal::visit_rows(std::int64_t after, std::optional<std::int64_t> upto,
                               const std::function<void(JournalRow&&)>& visit) const {
    visit_journal_rows_until(path_, [&](JournalRow&& row) {
        if (upto && row.sequence > *upto) return false;
        if (row.sequence > after) visit(std::move(row));
        return true;
    });
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:216-221
std::optional<std::string> NativeJournal::pair(std::int64_t sequence) const {
    if (sequence <= 0) return std::nullopt;
    std::optional<std::string> found;
    visit_journal_rows_until(path_, [&](JournalRow&& row) {
        if (row.sequence < sequence) return true;
        if (row.sequence == sequence) found = std::move(row.pair_id);
        return false;
    });
    return found;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-247
std::vector<std::int64_t> NativeJournal::append(std::span<const PendingJournalRow> rows) {
    if (!writable_) throw std::runtime_error("native_vrs_journal_read_only");
    require_write_owner();
    return append_journal_rows(path_, scan_, rows);
}

}  // namespace swegca::vrs
