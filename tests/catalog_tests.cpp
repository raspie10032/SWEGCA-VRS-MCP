#include "vrs/connection_catalog.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <unistd.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs = std::filesystem;
static unsigned checks = 0;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        std::abort(); \
    } \
} while (false)
static bool fail_rename = false;
static int syncs_before_failure = -1;
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    if (fail_rename) { fail_rename = false; errno = EIO; return -1; }
    return __real_rename(from, to);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (syncs_before_failure == 0) { syncs_before_failure = -1; errno = EIO; return -1; }
    if (syncs_before_failure > 0) --syncs_before_failure;
    return __real_fsync(fd);
}
template<class Exception, class Function> void expect_throw(Function operation) {
    bool caught = false;
    try { operation(); } catch (const Exception&) { caught = true; }
    CHECK(caught);
}
DigestBytes id(unsigned value) { DigestBytes result{}; result[0] = std::byte(value); return result; }
fs::path directory(const fs::path& root, unsigned session) {
    char first[3]; std::snprintf(first, sizeof(first), "%02x", session);
    return root / "sessions" / (std::string(first) + std::string(62, '0')) / "connections";
}
void publish(ConnectionCatalog& catalog, const PersistentConnection& connection, ExperienceLocation expected = {}) {
    const HeadUpdate change{&connection, expected}; catalog.publish(std::span(&change, 1));
}
void observe(SessionStore& session, PersistentConnection& connection, unsigned serial) {
    const auto rules = make_evidence_rules(connection.policy());
    EvidenceObservation value;
    value.hypothesis = connection.state().identity(); value.source = id(serial + 1); value.context = id(serial + 21);
    value.producer = id(serial + 41); value.outcome = EvidenceOutcome::support;
    const auto original = session.append_evidence(rules, {serial, 0, session.name(), "experiment", "text/plain", {}}, value);
    connection.append(original.original()); (void)connection.refine(serial, 0);
}

int main() {
    ConnectionHead current{id(1), {id(2), 80, 300, id(3)}, 2, 2, 1, 0.5};
    auto next = current; next.record.digest = id(4); ++next.revision; ++next.ordinal;
    CHECK(assess_head_publication(nullptr, {}, current, false) == HeadPublication::publish);
    CHECK(assess_head_publication(&current, current.record, current, false) == HeadPublication::unchanged);
    CHECK(assess_head_publication(&current, {}, next, true) == HeadPublication::stale);
    CHECK(assess_head_publication(&current, current.record, next, false) == HeadPublication::unrelated_lineage);
    CHECK(assess_head_publication(&current, current.record, next, true) == HeadPublication::publish);
    auto invalid = next; invalid.strength = std::numeric_limits<double>::infinity();
    CHECK(assess_head_publication(&current, current.record, invalid, true) == HeadPublication::invalid);
    invalid = current; invalid.strength = 0.25;
    CHECK(assess_head_publication(&current, current.record, invalid, true) == HeadPublication::invalid);
    invalid = next; invalid.observations = 0;
    CHECK(assess_head_publication(nullptr, {}, invalid, false) == HeadPublication::invalid);
    CHECK(!catalog_root_ready(false, 0));
    CHECK(catalog_root_ready(true, 0));
    CHECK(!catalog_root_ready(true, std::numeric_limits<std::uint64_t>::max()));

    auto pattern = (fs::temp_directory_path() / "swegca-catalog-XXXXXX").string();
    const auto made = ::mkdtemp(pattern.data()); CHECK(made != nullptr);
    const fs::path root(made);
    MemoryBudget memory(32 << 20);
    const EvidencePolicy policy;
    ExperienceLocation latest_a, latest_b;
    std::uint64_t final_generation = 0;
    {
        auto session = SessionStore::create(root, id(1), "catalog-session", 8192, memory);
        ConnectionCatalog catalog(session, memory, 4096);
        CHECK(catalog.generation() == 0 && catalog.size() == 0);
        CHECK(!fs::exists(directory(root, 1) / "current.block"));
        expect_throw<std::logic_error>([&] { catalog.publish({}); });
        expect_throw<std::system_error>([&] { ConnectionCatalog duplicate(session, memory, 4096); });
        (void)session.append({0, 0, session.name(), "user", "text/plain", {}});
        catalog.publish({});
        CHECK(catalog.generation() == 1 && catalog.size() == 0);
        const auto empty_root = catalog.root();
        catalog.publish({}); CHECK(catalog.root() == empty_root && catalog.generation() == 1);
        fs::copy_file(directory(root, 1) / "current.block", root / "old-pointer.block");

        auto a = PersistentConnection::create(session, id(51), 0.75, policy, memory, 4096);
        expect_throw<std::invalid_argument>([&] { publish(catalog, a); }); // no original observation yet
        observe(session, a, 0);
        auto b = PersistentConnection::create(session, id(52), 0.25, policy, memory, 4096);
        observe(session, b, 1);
        const std::array<HeadUpdate, 2> batch{{{&a, {}}, {&b, {}}}};
        catalog.publish(batch);
        CHECK(catalog.generation() == 2 && catalog.size() == 2);
        CHECK(catalog.find(id(51))->record == a.head());
        CHECK(catalog.find(id(52))->record == b.head());
        CHECK(catalog.find(id(99)) == nullptr);
        expect_throw<std::out_of_range>([&] { (void)catalog.recover(id(99)); });

        const auto first_a = a.head(), first_b = b.head();
        auto stale = PersistentConnection::recover(session, first_a, memory, 4096);
        observe(session, a, 2); publish(catalog, a, first_a);
        CHECK(catalog.generation() == 3);
        observe(session, stale, 3);
        expect_throw<std::invalid_argument>([&] { publish(catalog, stale, first_a); });
        observe(session, stale, 5);
        CHECK(stale.snapshot().ordinal > a.snapshot().ordinal);
        // Even lying about the expected head cannot turn a fork into ancestry.
        expect_throw<std::invalid_argument>([&] { publish(catalog, stale, a.head()); });
        CHECK(catalog.find(id(51))->record == a.head() && catalog.generation() == 3);
        observe(session, b, 4);
        const std::array<HeadUpdate, 2> wrong_batch{{{&b, first_b}, {&stale, first_a}}};
        expect_throw<std::invalid_argument>([&] { catalog.publish(wrong_batch); });
        CHECK(catalog.find(id(52))->record == first_b && catalog.generation() == 3);
        publish(catalog, b, first_b);
        const std::array<HeadUpdate, 2> duplicate_batch{{{&b, b.head()}, {&b, b.head()}}};
        expect_throw<std::invalid_argument>([&] { catalog.publish(duplicate_batch); });
        const auto before = catalog.generation();
        publish(catalog, b, b.head()); CHECK(catalog.generation() == before);
        latest_a = a.head(); latest_b = b.head(); final_generation = catalog.generation();
        auto recovered = catalog.recover(id(51));
        CHECK(recovered.head() == latest_a && recovered.state().revision() == a.state().revision());
        CHECK(recovered.state().strength() == a.state().strength());
        session.end();
        expect_throw<std::logic_error>([&] { catalog.publish({}); });
        session.publish_originals();
    }
    CHECK(memory.used() == 0);
    {
        auto session = SessionStore::open(root, id(1), memory);
        ConnectionCatalog catalog(session, memory, 4096);
        CHECK(catalog.generation() == final_generation && catalog.size() == 2);
        CHECK(catalog.find(id(51))->record == latest_a && catalog.find(id(52))->record == latest_b);
        const auto recovered = catalog.recover(id(52));
        CHECK(recovered.head() == latest_b);
        CHECK(session.phase() == SessionPhase::published);
    }
    CHECK(memory.used() == 0);
    {
        // A closed snapshot binds the exact current pointer, even when an
        // older pointer is individually well-formed with valid checksums.
        fs::copy_file(root / "old-pointer.block", directory(root, 1) / "current.block", fs::copy_options::overwrite_existing);
        expect_throw<std::runtime_error>([&] { (void)SessionStore::open(root, id(1), memory); });
    }
    {
        auto session = SessionStore::create(root, id(2), "atomic-root", 1 << 20, memory);
        auto a = PersistentConnection::create(session, id(53), 0.5, policy, memory, 4096);
        observe(session, a, 0);
        ExperienceLocation published;
        {
            ConnectionCatalog catalog(session, memory, 4096); publish(catalog, a);
            published = a.head(); observe(session, a, 1);
            fail_rename = true;
            expect_throw<std::system_error>([&] { publish(catalog, a, published); });
            CHECK(!catalog.usable());
            expect_throw<std::logic_error>([&] { (void)catalog.find(id(53)); });
        }
        {
            ConnectionCatalog recovered(session, memory, 4096);
            CHECK(recovered.generation() == 1 && recovered.find(id(53))->record == published);
            publish(recovered, a, published);
            CHECK(recovered.generation() == 2 && recovered.find(id(53))->record == a.head());
            published = a.head(); observe(session, a, 2);
            // Pointer create sync is first; publication directory sync is second.
            syncs_before_failure = 1;
            expect_throw<std::system_error>([&] { publish(recovered, a, published); });
            CHECK(!recovered.usable());
        }
        {
            ConnectionCatalog recovered(session, memory, 4096);
            CHECK(recovered.generation() == 3 && recovered.find(id(53))->record == a.head());
            expect_throw<std::invalid_argument>([&] { publish(recovered, a, published); });
            publish(recovered, a, a.head()); CHECK(recovered.generation() == 3);
        }
    }
    CHECK(memory.used() == 0);
    {
        auto first = SessionStore::create(root, id(3), "one-session", 4096, memory);
        auto other = SessionStore::create(root, id(4), "other-session", 4096, memory);
        ConnectionCatalog catalog(first, memory, 4096);
        (void)first.append({0, 0, first.name(), "user", "text/plain", {}});
        auto foreign = PersistentConnection::create(other, id(54), 0.5, policy, memory, 4096);
        observe(other, foreign, 0);
        expect_throw<std::invalid_argument>([&] { publish(catalog, foreign); });
        CHECK(catalog.generation() == 0 && catalog.size() == 0);
    }
    {
        auto session = SessionStore::create(root, id(5), "no-connections", 4096, memory);
        (void)session.append({0, 0, session.name(), "user", "text/plain", {}});
        session.end(); session.publish_originals();
        ConnectionCatalog empty(session, memory, 4096);
        CHECK(empty.generation() == 0 && empty.size() == 0);
        CHECK(!fs::exists(directory(root, 5)));
    }
    {
        // A checksummed catalog row can still lie about core-derived strength.
        // Recovery must check its latest connection, not just delta framing.
        auto session = SessionStore::create(root, id(6), "forged-catalog", 8192, memory);
        ExperienceLocation catalog_root;
        {
            auto connection = PersistentConnection::create(session, id(55), 0.5, policy, memory, 4096);
            observe(session, connection, 0);
            ConnectionCatalog catalog(session, memory, 4096); publish(catalog, connection);
            catalog_root = catalog.root();
        }
        const auto delta = session.read(catalog_root, 4096);
        auto delta_view = delta.view();
        std::vector<std::byte> changed(delta_view.content.begin(), delta_view.content.end());
        changed.at(104 + 136) ^= std::byte{1};
        delta_view.content = changed;
        const auto wrong_root = session.append(delta_view);
        const auto path = directory(root, 6) / "current.block";
        auto pointer = ExperienceBlock::open_reader(path);
        const auto pointer_record = pointer.read(pointer.location_at(ExperienceBlock::header_bytes), 4096, memory);
        auto pointer_view = pointer_record.view();
        std::vector<std::byte> wrong(pointer_view.content.begin(), pointer_view.content.end());
        std::copy(wrong_root.block.begin(), wrong_root.block.end(), wrong.begin() + 16);
        for (unsigned i = 0; i < 8; ++i) {
            wrong[48 + i] = std::byte(wrong_root.offset >> (8 * i));
            wrong[56 + i] = std::byte(wrong_root.bytes >> (8 * i));
        }
        std::copy(wrong_root.digest.begin(), wrong_root.digest.end(), wrong.begin() + 64);
        pointer_view.content = wrong;
        const auto forged = directory(root, 6) / "forged-pointer.block";
        {
            auto output = ExperienceBlock::create(forged, pointer.identity(), pointer.capacity());
            (void)output.append(pointer_view);
        }
        fs::rename(forged, path);
        expect_throw<std::runtime_error>([&] { ConnectionCatalog rejected(session, memory, 4096); });
    }
    CHECK(memory.used() == 0);
    fs::remove_all(root);
    std::printf("PASS: %u catalog checks\n", checks);
}
