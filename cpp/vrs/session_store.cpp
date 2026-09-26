#include "vrs/session_store.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swegca::vrs {
namespace {
using namespace architecture;
using namespace architecture::kernel;
constexpr std::string_view source = "swegca-session";
constexpr std::string_view media = "application/vnd.swegca.session-v1";
constexpr std::string_view tag = "SWGCSES1";
using Metadata = std::array<std::byte, 64>;

[[noreturn]] void io_error(const char* message) { throw std::system_error(errno, std::generic_category(), message); }
void sync_directory(const std::filesystem::path& path) {
    const auto fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) io_error("open session directory");
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    const auto error = errno;
    ::close(fd);
    if (result < 0) { errno = error; io_error("sync session directory"); }
}
std::string hex(const DigestBytes& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = std::to_integer<unsigned>(value[i]);
        result[2 * i] = digits[byte >> 4]; result[2 * i + 1] = digits[byte & 15];
    }
    return result;
}
void put(std::span<std::byte> data, unsigned at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) data[at + i] = std::byte(value >> (8 * i));
}
std::uint64_t get(std::span<const std::byte> data, unsigned at) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(std::to_integer<unsigned>(data[at + i])) << (8 * i);
    return value;
}
DigestBytes block_id(const DigestBytes& session, std::string_view kind, std::uint64_t index) {
    Sha256 hash;
    hash.update("SWEGCA session block v1"); hash.update(session); hash.update(kind);
    std::array<std::byte, 8> serial{};
    put(serial, 0, index); hash.update(serial);
    return hash.finish();
}
Metadata metadata(std::uint64_t capacity, std::uint64_t records = 0, std::uint64_t blocks = 0,
    const DigestBytes& inventory = {}) {
    Metadata data{};
    std::memcpy(data.data(), tag.data(), tag.size());
    put(data, 8, capacity); put(data, 16, records); put(data, 24, blocks);
    std::copy(inventory.begin(), inventory.end(), data.begin() + 32);
    return data;
}
bool is_metadata(const OriginalExperienceView& record) {
    return record.source == source && record.media_type == media && record.content.size() == Metadata{}.size() &&
        std::memcmp(record.content.data(), tag.data(), tag.size()) == 0;
}
std::uint64_t metadata_capacity(std::string_view name) {
    constexpr auto fixed = ExperienceBlock::header_bytes + ExperienceBlock::record_overhead +
        source.size() + media.size() + Metadata{}.size();
    if (name.size() > std::numeric_limits<std::uint64_t>::max() - fixed)
        throw std::length_error("session name too long");
    return fixed + name.size();
}
bool same_file(const std::filesystem::path& left, const std::filesystem::path& right) {
    struct stat a{}, b{};
    if (::lstat(left.c_str(), &a) < 0 || ::lstat(right.c_str(), &b) < 0) io_error("stat session publication");
    return S_ISREG(a.st_mode) && S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
}  // namespace

SessionStore SessionStore::create(const std::filesystem::path& root, const DigestBytes& identity,
    std::string_view name, std::uint64_t block_capacity, MemoryBudget& memory, StorageBudget* storage) {
    return SessionStore(root, identity, memory, true, name, block_capacity, storage);
}
SessionStore SessionStore::open(const std::filesystem::path& root, const DigestBytes& identity, MemoryBudget& memory, StorageBudget* storage) {
    return SessionStore(root, identity, memory, false, {}, 0, storage);
}

SessionStore::SessionStore(const std::filesystem::path& root, const DigestBytes& identity,
    MemoryBudget& memory, bool create, std::string_view name, std::uint64_t block_capacity, StorageBudget* storage)
    : root_(root), directory_(root / "sessions" / hex(identity)), identity_(identity), memory_(memory), storage_(storage),
      name_(name, &memory), block_capacity_(block_capacity), blocks_(&memory) {
    if (!named_digest(identity)) throw std::invalid_argument("empty session identity");
    const auto control_path = directory_ / "control.block";
    if (create) {
        if (!std::filesystem::is_directory(root_) || name.empty() || block_capacity < ExperienceBlock::header_bytes + ExperienceBlock::record_overhead ||
            block_capacity > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            throw std::invalid_argument("invalid session configuration");
        std::filesystem::create_directories(root_ / "sessions");
        std::filesystem::create_directories(root_ / "main");
        if (!std::filesystem::create_directory(directory_)) throw std::runtime_error("session already exists");
        sync_directory(root_); sync_directory(root_ / "sessions");
        control_.emplace(ExperienceBlock::create(control_path, block_id(identity_, "control", 0), metadata_capacity(name_), storage_));
        const auto data = metadata(block_capacity_);
        (void)control_->append({0, 0, name_, source, media, data});
        phase_ = SessionPhase::active;
        return;
    }
    // This lock lasts for the owner's lifetime, including ended sessions.
    control_.emplace(ExperienceBlock::open_writer(control_path, storage_));
    if (control_->identity() != block_id(identity_, "control", 0)) throw std::runtime_error("session identity mismatch");
    const auto stored = control_->read(control_->location_at(ExperienceBlock::header_bytes), memory_.limit(), memory_);
    const auto meta = stored.view();
    const auto control_extent = control_->inspect();
    if (!is_metadata(meta) || meta.sequence != 0 || get(meta.content, 16) != 0 || get(meta.content, 24) != 0 ||
        control_extent.complete_records != 1 || control_extent.unfinished_bytes != 0 ||
        !std::ranges::equal(meta.content.subspan(32), zero_digest_bytes))
        throw std::runtime_error("invalid session control record");
    name_ = meta.session;
    block_capacity_ = get(meta.content, 8);
    if (block_capacity_ < ExperienceBlock::header_bytes + ExperienceBlock::record_overhead ||
        block_capacity_ > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::runtime_error("invalid stored block capacity");
    std::pmr::map<std::uint64_t, DigestBytes> ordered(&memory_);
    for (const auto& file : std::filesystem::directory_iterator(directory_)) {
        const auto filename = file.path().filename().string();
        if (!filename.starts_with("b-")) continue;
        if (filename.size() != 24 || !filename.ends_with(".block")) throw std::runtime_error("invalid session block name");
        std::uint64_t index = 0;
        const auto parsed = std::from_chars(filename.data() + 2, filename.data() + 18, index, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != filename.data() + 18 || block_path(index).filename() != file.path().filename())
            throw std::runtime_error("invalid session block number");
        auto block = ExperienceBlock::open_reader(file.path());
        if (block.identity() != block_id(identity_, "data", index) || block.capacity() != block_capacity_ ||
            !ordered.emplace(index, block.identity()).second)
            throw std::runtime_error("session block lineage mismatch");
        const auto inspected = block.inspect();
        if (inspected.complete_records > std::numeric_limits<std::uint64_t>::max() - records_)
            throw std::overflow_error("session record count overflow");
        records_ += inspected.complete_records;
        blocks_.emplace(block.identity(), BlockState{index, inspected});
    }
    for (const auto& [index, unused] : ordered) {
        (void)unused;
        if (index != next_block_ || next_block_ == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("session block sequence gap");
        ++next_block_;
    }
    phase_ = SessionPhase::active;
    const auto closed = directory_ / "closed.block";
    if (std::filesystem::exists(closed)) {
        auto block = ExperienceBlock::open_reader(closed);
        const auto record = block.read(block.location_at(ExperienceBlock::header_bytes), memory_.limit(), memory_);
        const auto view = record.view();
        const auto actual = metadata(block_capacity_, records_, blocks_.size(), inventory());
        const auto closed_extent = block.inspect();
        if (block.identity() != block_id(identity_, "end", 0) || !is_metadata(view) || view.sequence != 1 ||
            view.session != name_ || !std::ranges::equal(view.content, actual) ||
            closed_extent.complete_records != 1 || closed_extent.unfinished_bytes != 0)
            throw std::runtime_error("session end inventory mismatch");
        SessionPhase next;
        if (!next_session_phase(phase_, SessionOperation::end, next)) throw std::logic_error("invalid recovered session end");
        phase_ = next;
    }
    const auto published = root_ / "main" / (hex(identity_) + ".session");
    if (std::filesystem::exists(published)) {
        SessionPhase next;
        if (!next_session_phase(phase_, SessionOperation::publish, next) || !same_file(published, closed))
            throw std::runtime_error("unclosed or unrelated Main session publication");
        phase_ = next;
    }
    if (phase_ == SessionPhase::active && next_block_ != 0) {
        writer_.emplace(ExperienceBlock::open_writer(block_path(next_block_ - 1), storage_));
        current_records_ = writer_->inspect().complete_records;
    }
}

std::filesystem::path SessionStore::block_path(std::uint64_t index) const {
    std::array<char, 25> filename{};
    std::memcpy(filename.data(), "b-0000000000000000.block", 24);
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) filename[17 - i] = digits[(index >> (4 * i)) & 15];
    return directory_ / filename.data();
}
void SessionStore::require(SessionOperation operation) const {
    SessionPhase next;
    if (!usable_ || !next_session_phase(phase_, operation, next))
        throw std::logic_error("session operation is not permitted by SWEGCA");
}
void SessionStore::verify_name(const OriginalExperienceView& experience) const {
    if (experience.session != name_) throw std::invalid_argument("experience belongs to another session");
}
void SessionStore::next_block() {
    if (next_block_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session block sequence exhausted");
    const auto identity = block_id(identity_, "data", next_block_);
    const auto inserted = blocks_.emplace(identity, BlockState{next_block_}).first;
    try {
        auto next = ExperienceBlock::create(block_path(next_block_), identity, block_capacity_, storage_);
        writer_.emplace(std::move(next));
        ++next_block_;
        current_records_ = 0;
    } catch (...) { blocks_.erase(inserted); throw; }
}

ExperienceLocation SessionStore::append(const OriginalExperienceView& experience) {
    require(SessionOperation::append); verify_name(experience);
    if (records_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session count exhausted");
    try {
        if (!writer_ || !writer_->can_append()) next_block();
        ExperienceLocation location;
        try { location = writer_->append(experience); }
        catch (const std::length_error&) {
            if (current_records_ == 0) throw;
            next_block(); location = writer_->append(experience);
        }
        recorded(location);
        return location;
    } catch (const std::invalid_argument&) { throw; }
      catch (const std::length_error&) { throw; }
      catch (...) { usable_ = false; throw; }
}

ExperienceEvidence SessionStore::append_evidence(const EvidenceRules& rules,
    const OriginalExperienceView& original, const EvidenceObservation& value) {
    require(SessionOperation::append); verify_name(original);
    if (records_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session count exhausted");
    try {
        if (!writer_ || !writer_->can_append()) next_block();
        try {
            auto result = record_evidence(*writer_, rules, original, value, memory_);
            recorded(result.original());
            return result;
        } catch (const std::length_error&) {
            if (current_records_ == 0) throw;
            next_block();
            auto result = record_evidence(*writer_, rules, original, value, memory_);
            recorded(result.original());
            return result;
        }
    } catch (const std::invalid_argument&) { throw; }
      catch (const std::length_error&) { throw; }
      catch (...) { usable_ = false; throw; }
}

StoredExperience SessionStore::read(const ExperienceLocation& location, std::uint64_t limit) const {
    const auto found = blocks_.find(location.block);
    if (found == blocks_.end()) throw std::invalid_argument("experience block does not belong to this session");
    auto block = ExperienceBlock::open_reader(block_path(found->second.index));
    return block.read(location, limit, memory_);
}

void SessionStore::recorded(const ExperienceLocation& location) noexcept {
    auto& extent = blocks_.find(location.block)->second.extent;
    extent.content_digest = extend_experience_digest(extent.content_digest, location);
    ++extent.complete_records;
    extent.complete_bytes = location.offset + location.bytes;
    ++records_; ++current_records_;
}

DigestBytes SessionStore::inventory() const {
    Sha256 hash;
    hash.update("SWEGCA ended session inventory v1"); hash.update(identity_);
    for (const auto& [identity, state] : blocks_) {
        const auto& inspected = state.extent;
        std::array<std::byte, 32> shape{};
        put(shape, 0, state.index); put(shape, 8, inspected.complete_records);
        put(shape, 16, inspected.complete_bytes); put(shape, 24, inspected.unfinished_bytes);
        hash.update(identity); hash.update(shape); hash.update(inspected.content_digest);
    }
    const auto current = directory_ / "connections" / "current.block";
    if (std::filesystem::exists(current)) {
        auto pointer = ExperienceBlock::open_reader(current);
        const auto location = pointer.location_at(ExperienceBlock::header_bytes);
        const auto record = pointer.read(location, memory_.limit(), memory_);
        (void)record;
        const auto extent = pointer.inspect();
        if (extent.complete_records != 1 || extent.unfinished_bytes != 0)
            throw std::runtime_error("invalid ended-session catalog reference");
        hash.update("SWEGCA session catalog reference v1");
        hash.update(pointer.identity()); hash.update(location.digest);
    }
    return hash.finish();
}

void SessionStore::end() {
    require(SessionOperation::end);
    try {
        const auto data = metadata(block_capacity_, records_, blocks_.size(), inventory());
        // Incomplete staging files remain available for diagnosis, never as an
        // end marker. Link only a complete, synced end record into its name.
        std::uint64_t attempt = 0;
        auto staging = directory_ / "ending-0.block";
        while (std::filesystem::exists(staging)) {
            if (attempt == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session end attempts exhausted");
            staging = directory_ / ("ending-" + std::to_string(++attempt) + ".block");
        }
        auto block = ExperienceBlock::create(staging, block_id(identity_, "end", 0), metadata_capacity(name_), storage_);
        (void)block.append({1, 0, name_, source, media, data});
        const auto closed = directory_ / "closed.block";
        if (::link(staging.c_str(), closed.c_str()) < 0) io_error("publish session end marker");
        sync_directory(directory_);
        SessionPhase next;
        if (!next_session_phase(phase_, SessionOperation::end, next)) throw std::logic_error("invalid session end");
        phase_ = next;
        writer_.reset();
    } catch (...) { usable_ = false; throw; }
}

void SessionStore::publish_originals() {
    require(SessionOperation::publish);
    try {
        const auto closed = directory_ / "closed.block";
        const auto published = root_ / "main" / (hex(identity_) + ".session");
        if (::link(closed.c_str(), published.c_str()) < 0 &&
            (errno != EEXIST || !same_file(closed, published))) io_error("publish ended session originals");
        sync_directory(root_ / "main");
        SessionPhase next;
        if (!next_session_phase(phase_, SessionOperation::publish, next)) throw std::logic_error("invalid Main session publication");
        phase_ = next;
    } catch (...) { usable_ = false; throw; }
}

}  // namespace swegca::vrs
