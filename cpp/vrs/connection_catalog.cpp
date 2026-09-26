#include "vrs/connection_catalog.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swegca::vrs {
namespace {
using namespace architecture;
using namespace architecture::kernel;
constexpr std::string_view source = "swegca-catalog";
constexpr std::string_view media = "application/vnd.swegca.catalog-v1";
constexpr std::size_t prefix_size = 104, row_size = 224, pointer_size = 96;
struct ReplacedPointer {
    int fd=-1;
    ReplacedPointer(const std::filesystem::path& path,bool enabled) {
        if(!enabled)return;
        fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0&&errno!=ENOENT)throw std::system_error(errno,std::generic_category(),"open replaced catalog pointer");
    }
    ~ReplacedPointer(){if(fd>=0)::close(fd);}
    ReplacedPointer(const ReplacedPointer&)=delete;
    ReplacedPointer& operator=(const ReplacedPointer&)=delete;
    std::uint64_t removed_bytes() noexcept {
        struct stat value{};
        if(fd<0||::fstat(fd,&value)<0||!S_ISREG(value.st_mode)||value.st_nlink!=0||value.st_size<0)return 0;
        const auto bytes=static_cast<std::uint64_t>(value.st_size);
        const auto old=fd;fd=-1;
        if(::close(old)<0)return 0;
        return bytes;
    }
};
struct Row { ConnectionHead head; ExperienceLocation expected; };

void put(std::span<std::byte> data, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) data[at + i] = std::byte(value >> (8 * i));
}
std::uint64_t get(std::span<const std::byte> data, std::size_t at) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(std::to_integer<unsigned>(data[at + i])) << (8 * i);
    return value;
}
void put_digest(std::span<std::byte> data, std::size_t at, const DigestBytes& value) { std::copy(value.begin(), value.end(), data.begin() + at); }
DigestBytes get_digest(std::span<const std::byte> data, std::size_t at) {
    DigestBytes result; std::copy_n(data.begin() + at, result.size(), result.begin()); return result;
}
void put_address(std::span<std::byte> data, std::size_t at, const ExperienceLocation& address) {
    put_digest(data, at, address.block); put(data, at + 32, address.offset);
    put(data, at + 40, address.bytes); put_digest(data, at + 48, address.digest);
}
ExperienceLocation get_address(std::span<const std::byte> data, std::size_t at) {
    return {get_digest(data, at), get(data, at + 32), get(data, at + 40), get_digest(data, at + 48)};
}
void put_row(std::span<std::byte> data, std::size_t at, const Row& row) {
    put_digest(data, at, row.head.identity); put_address(data, at + 32, row.head.record);
    put(data, at + 112, row.head.revision); put(data, at + 120, row.head.ordinal);
    put(data, at + 128, row.head.observations); put(data, at + 136, std::bit_cast<std::uint64_t>(row.head.strength));
    put_address(data, at + 144, row.expected);
}
Row get_row(std::span<const std::byte> data, std::size_t at) {
    return {{get_digest(data, at), get_address(data, at + 32), get(data, at + 112), get(data, at + 120),
        get(data, at + 128), std::bit_cast<double>(get(data, at + 136))}, get_address(data, at + 144)};
}
[[noreturn]] void io_error(const char* message) { throw std::system_error(errno, std::generic_category(), message); }
void sync_directory(const std::filesystem::path& path) {
    const auto fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) io_error("open catalog directory");
    int result; do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    const auto error = errno; ::close(fd);
    if (result < 0) { errno = error; io_error("sync catalog directory"); }
}
DigestBytes pointer_id(std::string_view session) {
    Sha256 hash; hash.update("SWEGCA connection catalog pointer v1"); hash.update(session); return hash.finish();
}
void validate_delta(const StoredExperience& record, std::string_view name, std::uint64_t generation) {
    const auto view = record.view();
    const auto data = view.content;
    if (view.session != name || view.source != source || view.media_type != media || view.sequence != generation ||
        view.observed_at_ns != 0 || data.size() < prefix_size || std::memcmp(data.data(), "SWGCCAT1", 8) != 0 ||
        get(data, 8) != generation || (data.size() - prefix_size) % row_size != 0 ||
        get(data, 96) != (data.size() - prefix_size) / row_size)
        throw std::runtime_error("invalid connection catalog delta");
}
bool same_snapshot(const ConnectionHead& a, const ConnectionHead& b) {
    return a.identity == b.identity && a.record == b.record && a.revision == b.revision && a.ordinal == b.ordinal &&
        a.observations == b.observations && std::bit_cast<std::uint64_t>(a.strength) == std::bit_cast<std::uint64_t>(b.strength);
}
}  // namespace

ConnectionCatalog::ConnectionCatalog(SessionStore& session, MemoryBudget& memory, std::uint64_t original_read_limit)
    : session_(session), memory_(memory), original_read_limit_(original_read_limit),
      directory_(session.directory_ / "connections"), heads_(&memory) {
    if (!session.usable() || original_read_limit == 0) throw std::invalid_argument("invalid catalog owner or read limit");
    if (!std::filesystem::exists(directory_)) {
        SessionPhase next;
        if (!next_session_phase(session_.phase(), SessionOperation::append, next)) return;
        session_.require(SessionOperation::append);
        std::filesystem::create_directory(directory_);
        sync_directory(session.directory_);
    }
    lock_ = ::open((directory_ / "owner.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0) io_error("open catalog lock");
    try {
        struct stat info{};
        if (::fstat(lock_, &info) < 0) io_error("stat catalog lock");
        if (!S_ISREG(info.st_mode)) throw std::runtime_error("catalog lock is not a regular file");
        if (::flock(lock_, LOCK_EX | LOCK_NB) < 0) io_error("lock catalog owner");
        restore();
    } catch (...) { ::close(lock_); lock_ = -1; throw; }
}
ConnectionCatalog::~ConnectionCatalog() { if (lock_ >= 0) ::close(lock_); }

const ConnectionHead* ConnectionCatalog::find(const DigestBytes& identity) const {
    if (!usable_ || !session_.usable()) throw std::logic_error("catalog must be reopened after storage failure");
    const auto at = heads_.find(identity); return at == heads_.end() ? nullptr : &at->second;
}

void ConnectionCatalog::restore() {
    const auto current_path = directory_ / "current.block";
    if (!std::filesystem::exists(current_path)) return;
    auto pointer = ExperienceBlock::open_reader(current_path, session_.storage_);
    const auto frame = pointer.location_at(ExperienceBlock::header_bytes);
    const auto fixed_limit = ExperienceBlock::record_overhead + session_.name().size() + source.size() + media.size() + pointer_size;
    const auto record = pointer.read(frame, fixed_limit, memory_);
    const auto view = record.view();
    if (pointer.identity() != pointer_id(session_.name()) || view.session != session_.name() ||
        view.source != source || view.media_type != media || view.content.size() != pointer_size ||
        std::memcmp(view.content.data(), "SWGCREF1", 8) != 0 || view.observed_at_ns != 0)
        throw std::runtime_error("invalid connection catalog pointer");
    generation_ = get(view.content, 8); root_ = get_address(view.content, 16);
    if (generation_ == 0 || generation_ != view.sequence || !head_address_valid(root_))
        throw std::runtime_error("invalid catalog root generation");
    const auto extent = pointer.inspect();
    if (extent.complete_records != 1 || extent.unfinished_bytes != 0) throw std::runtime_error("catalog pointer has extra records");
    std::pmr::vector<ExperienceLocation> chain(&memory_);
    auto cursor = root_;
    for (auto expected = generation_; expected > 0; --expected) {
        const auto delta = session_.read(cursor, session_.block_capacity_);
        validate_delta(delta, session_.name(), expected);
        chain.push_back(cursor);
        cursor = get_address(delta.view().content, 16);
        if ((expected == 1 && cursor != ExperienceLocation{}) || (expected > 1 && !head_address_valid(cursor)))
            throw std::runtime_error("catalog root parent mismatch");
    }
    std::uint64_t expected_generation = 1;
    for (auto at = chain.rbegin(); at != chain.rend(); ++at, ++expected_generation) {
        const auto delta = session_.read(*at, session_.block_capacity_);
        validate_delta(delta, session_.name(), expected_generation);
        const auto data = delta.view().content;
        std::pmr::set<DigestBytes> targets(&memory_);
        for (std::size_t index = prefix_size; index < data.size(); index += row_size) {
            const auto row = get_row(data, index);
            if (!targets.insert(row.head.identity).second) throw std::runtime_error("duplicate catalog target");
            const auto current = heads_.find(row.head.identity);
            const auto previous = current == heads_.end() ? nullptr : &current->second;
            const auto lineage = previous && PersistentConnection::verifies_extension(session_, row.head, previous->record);
            if (assess_head_publication(previous, row.expected, row.head, lineage) != HeadPublication::publish)
                throw std::runtime_error("catalog delta rejected by SWEGCA");
            heads_.insert_or_assign(row.head.identity, row.head);
        }
    }
    // Metadata/ancestry checks above are provisional until the latest heads
    // have reproduced their actual SWEGCA decisions from original experience.
    for (const auto& [identity, snapshot] : heads_) {
        (void)identity;
        const auto connection = PersistentConnection::recover(session_, snapshot.record, memory_, original_read_limit_);
        if (!same_snapshot(connection.snapshot(), snapshot)) throw std::runtime_error("catalog head differs from core recovery");
    }
}

void ConnectionCatalog::publish(std::span<const HeadUpdate> changes) {
    if (!usable_) throw std::logic_error("catalog must be reopened after storage failure");
    session_.require(SessionOperation::append);
    if (!catalog_root_ready(session_.original_count() != 0, generation_))
        throw std::logic_error("catalog needs its first experience or has exhausted generations");
    std::pmr::vector<Row> rows(&memory_);
    std::pmr::set<DigestBytes> targets(&memory_);
    std::pmr::map<DigestBytes, ConnectionHead> new_nodes(&memory_);
    for (const auto& change : changes) {
        if (!change.connection || &change.connection->session_ != &session_)
            throw std::invalid_argument("catalog connection belongs to another session");
        const auto candidate = change.connection->snapshot();
        if (!targets.insert(candidate.identity).second) throw std::invalid_argument("duplicate catalog target");
        const auto current = find(candidate.identity);
        const auto lineage = current && change.connection->contains_history(std::span(&current->record, 1));
        const auto result = assess_head_publication(current, change.expected, candidate, lineage);
        if (result == HeadPublication::unchanged) continue;
        if (result != HeadPublication::publish) throw std::invalid_argument("head publication rejected by SWEGCA");
        rows.push_back({candidate, change.expected});
        if (!current) new_nodes.emplace(candidate.identity, candidate);
    }
    if (rows.empty() && generation_ != 0) return;
    if (rows.size() > (std::numeric_limits<std::size_t>::max() - prefix_size) / row_size)
        throw std::length_error("catalog delta size overflow");
    std::pmr::vector<std::byte> data(&memory_);
    data.resize(prefix_size + row_size * rows.size());
    std::memcpy(data.data(), "SWGCCAT1", 8); put(data, 8, generation_ + 1);
    put_address(data, 16, root_); put(data, 96, rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) put_row(data, prefix_size + i * row_size, rows[i]);
    try {
        const auto root = session_.append({generation_ + 1, 0, session_.name(), source, media, data});
        std::array<std::byte, pointer_size> pointer_data{};
        std::memcpy(pointer_data.data(), "SWGCREF1", 8); put(pointer_data, 8, generation_ + 1); put_address(pointer_data, 16, root);
        std::uint64_t attempt = 0;
        auto staging = directory_ / "pointer-stage-0.block";
        while (std::filesystem::exists(staging)) {
            if (attempt == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("catalog pointer attempts exhausted");
            staging = directory_ / ("pointer-stage-" + std::to_string(++attempt) + ".block");
        }
        const auto capacity = ExperienceBlock::header_bytes + ExperienceBlock::record_overhead + session_.name().size() + source.size() + media.size() + pointer_size;
        auto pointer = ExperienceBlock::create(staging, pointer_id(session_.name()), capacity, session_.storage_);
        (void)pointer.append({generation_ + 1, 0, session_.name(), source, media, pointer_data});
        const auto current = directory_ / "current.block";
        ReplacedPointer replaced(current,session_.storage_!=nullptr);
        if (::rename(staging.c_str(), current.c_str()) < 0) io_error("publish catalog pointer");
        sync_directory(directory_);
        // Rename and directory durability succeeded. Retain the old inode's
        // descriptor until its link count proves no retained name remains.
        if(session_.storage_)session_.storage_->reclaim_removed(replaced.removed_bytes());
        // All allocations preceded durable publication. Matching allocators
        // transfer the prepared nodes; assigning existing fixed-width values
        // cannot fail. Main serializes this owner and the session writer.
        heads_.merge(new_nodes);
        for (const auto& row : rows) heads_.find(row.head.identity)->second = row.head;
        root_ = root; ++generation_;
    } catch (...) { usable_ = false; throw; }
}

PersistentConnection ConnectionCatalog::recover(const DigestBytes& identity) const {
    const auto head = find(identity);
    if (!head) throw std::out_of_range("connection is absent from current catalog");
    return PersistentConnection::recover(session_, head->record, memory_, original_read_limit_);
}

}  // namespace swegca::vrs
