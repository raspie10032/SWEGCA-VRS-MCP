#include "vrs/persistent_connection.hpp"

#include <bit>
#include <cstdarg>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
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

// Fault injection is linked only into this executable, never the runtime.
static unsigned reader_opens=0;
extern "C" int __real_open(const char*,int,...);
extern "C" int __wrap_open(const char* path,int flags,...) {
    int fd;
    if(flags&O_CREAT){va_list args;va_start(args,flags);const auto mode=va_arg(args,int);va_end(args);fd=__real_open(path,flags,mode);}
    else fd=__real_open(path,flags);
    if(fd>=0&&(flags&O_ACCMODE)==O_RDONLY&&!(flags&O_DIRECTORY))++reader_opens;
    return fd;
}
static int writes_before_failure = -1;
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t size, off_t offset) {
    if (writes_before_failure == 0) { writes_before_failure = -1; errno = ENOSPC; return -1; }
    if (writes_before_failure > 0) --writes_before_failure;
    return __real_pwrite(fd, data, size, offset);
}
template<class Exception, class Function> void expect_throw(Function operation) {
    bool caught = false;
    try { operation(); } catch (const Exception&) { caught = true; }
    CHECK(caught);
}
DigestBytes id(unsigned value) { DigestBytes digest{}; digest[0] = std::byte(value); return digest; }
std::span<const std::byte> bytes(const std::string& value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}
bool same_location(const ExperienceLocation& a, const ExperienceLocation& b) {
    return a.block == b.block && a.offset == b.offset && a.bytes == b.bytes && a.digest == b.digest;
}
bool same_bits(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }

void observations(SessionStore& session, PersistentConnection& connection, EvidenceOutcome outcome, bool expires = false) {
    const auto rules = make_evidence_rules(connection.policy());
    for (unsigned axis = 0; axis < connection.policy().axis_count; ++axis)
        for (unsigned group = 0; group < 12; ++group) {
            EvidenceObservation value;
            value.hypothesis = connection.state().identity(); value.axis = axis; value.outcome = outcome;
            value.source = id(group + 1); value.context = id(group + 31); value.producer = id(group + 61);
            value.has_expiry = expires; value.expires_at = 10;
            const std::string content = "recorded observation " + std::to_string(group) + ":" + std::to_string(axis);
            const auto evidence = session.append_evidence(rules,
                {session.original_count(), 0, session.name(), "experiment", "text/plain", bytes(content)}, value);
            connection.append(evidence.original());
        }
}

ExperienceLocation change_refinement(SessionStore& session, const ExperienceLocation& head, unsigned offset) {
    const auto stored = session.read(head, 4096);
    auto record = stored.view();
    std::vector<std::byte> changed(record.content.begin(), record.content.end());
    changed.at(offset) ^= std::byte{1};
    record.content = changed;
    return session.append(record); // fresh, valid outer checksum; inner claim is wrong
}

int main() {
    auto pattern = (fs::temp_directory_path() / "swegca-persistent-XXXXXX").string();
    const auto made = ::mkdtemp(pattern.data()); CHECK(made != nullptr);
    const fs::path root(made);
    MemoryBudget memory(16 << 20);
    EvidencePolicy policy;
    {
        auto session=SessionStore::create(root,id(9),"single-block",1<<20,memory);
        ExperienceLocation saved;
        {
            auto original=PersistentConnection::create(session,id(90),1,policy,memory,4096);
            observations(session,original,EvidenceOutcome::support,true);
            (void)original.refine(12345,10);saved=original.head();
        }
        CHECK(session.block_count()==1);
        const auto before=reader_opens;
        auto restored=PersistentConnection::recover(session,saved,memory,4096);
        CHECK(reader_opens==before+1);
        CHECK(restored.state().experiences().size()==48&&restored.state().strength()==1.01);
    }
    CHECK(memory.used()==0);
    ExperienceLocation head, support_head;
    ConnectionHead support_snapshot,append_snapshot,origin_snapshot;
    std::uint64_t expected_revision = 0;
    double expected_strength = 0;
    {
        auto session = SessionStore::create(root, id(1), "live", 4096, memory);
        {
            auto connection = PersistentConnection::create(session, id(50), 0.999, policy, memory, 4096);
            CHECK(connection.state().strength() == 0.999 && connection.state().revision() == 0);
            origin_snapshot=connection.snapshot();
            observations(session, connection, EvidenceOutcome::support, true);
            append_snapshot=connection.snapshot();
            const auto result = connection.refine(12345, 10);
            CHECK(result.result().verification().judgment().status() == EvidenceStatus::accept);
            CHECK(connection.state().strength() == 0.999 * 1.01);
            CHECK(connection.state().revision() == 49);
            support_head = head = connection.head();
            support_snapshot=connection.snapshot();
        }
        {
            auto connection = PersistentConnection::recover(session, head, memory, 4096);
            CHECK(connection.state().identity() == id(50));
            CHECK(connection.state().experiences().size() == 48);
            CHECK(connection.state().revision() == 49 && same_bits(connection.state().strength(), 0.999 * 1.01));
            CHECK(evidence_policy_digest(connection.policy()) == evidence_policy_digest(policy));
            observations(session, connection, EvidenceOutcome::refute);
            const auto result = connection.refine(999, 11);
            CHECK(result.result().verification().judgment().status() == EvidenceStatus::reject);
            expected_revision = connection.state().revision(); expected_strength = connection.state().strength();
            CHECK(expected_revision == 98);
            CHECK(same_bits(expected_strength, (0.999 * 1.01) * 0.995));
            head = connection.head();
            const auto check_history=[&](const ConnectionHead& expected){
                const auto old=connection.historical_snapshot(expected.record);
                CHECK(old.identity==expected.identity&&old.record==expected.record);
                CHECK(old.ordinal==expected.ordinal&&old.revision==expected.revision);
                CHECK(old.observations==expected.observations&&same_bits(old.strength,expected.strength));
            };
            check_history(connection.snapshot());check_history(support_snapshot);
            check_history(append_snapshot);check_history(origin_snapshot);
            expect_throw<std::invalid_argument>([&]{(void)connection.historical_snapshot({});});
            CHECK(connection.state().revision()==expected_revision&&same_bits(connection.state().strength(),expected_strength));
        }
    }
    CHECK(memory.used() == 0);
    {
        auto session = SessionStore::open(root, id(1), memory);
        auto connection = PersistentConnection::recover(session, head, memory, 4096);
        CHECK(connection.state().revision() == expected_revision);
        const auto old=connection.historical_snapshot(support_snapshot.record);
        CHECK(old.observations==support_snapshot.observations&&old.ordinal==support_snapshot.ordinal);
        CHECK(old.revision==support_snapshot.revision&&same_bits(old.strength,support_snapshot.strength));
        CHECK(same_bits(connection.state().strength(), expected_strength));
        CHECK(connection.state().experiences().size() == 96);
        CHECK(same_location(connection.head(), head));
        const auto good_revision = connection.state().revision();
        const auto good_strength = connection.state().strength();
        // Encoded numerical strength, evidence digest, and seed are each
        // checked against a fresh SWEGCA execution during recovery.
        for (const auto offset : {176U, 184U, 152U, 16U, 128U, 136U, 144U, 9U}) {
            const auto false_head = change_refinement(session, head, offset);
            expect_throw<std::runtime_error>([&] { (void)PersistentConnection::recover(session, false_head, memory, 4096); });
        }
        {
            const auto complete = session.read(head, 4096);
            auto short_record = complete.view();
            short_record.content = short_record.content.first(short_record.content.size() - 1);
            const auto false_head = session.append(short_record);
            expect_throw<std::runtime_error>([&] { (void)PersistentConnection::recover(session, false_head, memory, 4096); });
        }
        CHECK(connection.state().revision() == good_revision && same_bits(connection.state().strength(), good_strength));
        const auto count = session.original_count();
        session.end();
        expect_throw<std::logic_error>([&] { (void)connection.refine(1, 11); });
        CHECK(session.original_count() == count);
        CHECK(connection.state().revision() == good_revision && same_bits(connection.state().strength(), good_strength));
        CHECK(same_location(connection.head(), head));
        session.publish_originals();
    }
    {
        auto published = SessionStore::open(root, id(1), memory);
        auto connection = PersistentConnection::recover(published, head, memory, 4096);
        CHECK(published.phase() == SessionPhase::published);
        CHECK(connection.state().revision() == expected_revision);
        CHECK(same_bits(connection.state().strength(), expected_strength));
        // A historic head can be replayed as a historic version. Selecting a
        // current authoritative head remains Main's graph/root responsibility.
        auto historical = PersistentConnection::recover(published, support_head, memory, 4096);
        CHECK(historical.state().revision() == 49);
        CHECK(same_bits(historical.state().strength(), 0.999 * 1.01));
        expect_throw<std::logic_error>([&] { (void)historical.refine(1, 10); });
    }
    CHECK(memory.used() == 0);
    {
        auto session = SessionStore::create(root, id(2), "abstain", 4096, memory);
        {
            auto connection = PersistentConnection::create(session, id(51), -0.0, policy, memory, 4096);
            const auto result = connection.refine(7, 0);
            CHECK(result.result().verification().judgment().reason() == EvidenceReason::minimum_effective_samples);
            CHECK(connection.state().revision() == 1 && same_bits(connection.state().strength(), -0.0));
            head = connection.head();
        }
        {
            auto connection = PersistentConnection::recover(session, head, memory, 4096);
            CHECK(connection.state().revision() == 1 && same_bits(connection.state().strength(), -0.0));
            EvidenceObservation value;
            value.hypothesis = id(51); value.source = id(1); value.context = id(2); value.producer = id(3);
            value.outcome = EvidenceOutcome::support;
            const auto rules = make_evidence_rules(policy);
            const auto original = session.append_evidence(rules, {0, 0, "abstain", "experiment", "text/plain", {}}, value);
            connection.append(original.original());
            const auto result = connection.refine(17, 0);
            CHECK(result.result().verification().judgment().reason() == EvidenceReason::minimum_effective_samples);
            CHECK(result.result().strength().valid());
            CHECK(connection.state().revision() == 3 && same_bits(connection.state().strength(), -0.0));
            head = connection.head();
        }
        auto recovered = PersistentConnection::recover(session, head, memory, 4096);
        CHECK(recovered.state().revision() == 3 && same_bits(recovered.state().strength(), -0.0));
    }
    CHECK(memory.used() == 0);
    {
        auto session = SessionStore::create(root, id(3), "custom-policy", 4096, memory);
        auto custom = policy;
        custom.axis_count = 2; custom.confidence_level = 0.95; custom.accept_margin = 0.125;
        custom.prior_alpha = 1.25; custom.prior_beta = 0.75; custom.recent_window = 9;
        {
            auto connection = PersistentConnection::create(session, id(52), 2, custom, memory, 4096);
            observations(session, connection, EvidenceOutcome::support);
            (void)connection.refine(23, 0);
            head = connection.head();
        }
        auto connection = PersistentConnection::recover(session, head, memory, 4096);
        CHECK(evidence_policy_digest(connection.policy()) == evidence_policy_digest(custom));
        CHECK(connection.state().revision() == 25 && connection.state().strength() == 2 * 1.01);
    }
    CHECK(memory.used() == 0);
    // A real pwrite failure after a partial record must not update memory or
    // acknowledge a new connection head. Recover only the previously returned
    // head, preserve the partial bytes, and continue in a fresh physical block.
    {
        auto session = SessionStore::create(root, id(4), "write-failure", 1 << 20, memory);
        auto connection = PersistentConnection::create(session, id(53), 1, policy, memory, 4096);
        observations(session, connection, EvidenceOutcome::support);
        head = connection.head();
        expected_revision = connection.state().revision();
        writes_before_failure = 2;
        expect_throw<std::system_error>([&] { (void)connection.refine(3, 0); });
        CHECK(!session.usable());
        CHECK(connection.state().revision() == expected_revision && connection.state().strength() == 1);
        CHECK(same_location(connection.head(), head));
    }
    {
        auto session = SessionStore::open(root, id(4), memory);
        auto connection = PersistentConnection::recover(session, head, memory, 4096);
        CHECK(connection.state().revision() == expected_revision && connection.state().strength() == 1);
        const auto blocks = session.block_count();
        const auto report = connection.refine(3, 0);
        CHECK(report.result().verification().judgment().status() == EvidenceStatus::accept);
        CHECK(connection.state().strength() == 1.01 && connection.state().revision() == expected_revision + 1);
        CHECK(session.block_count() == blocks + 1);
        head = connection.head();
        session.end(); session.publish_originals();
    }
    {
        auto session = SessionStore::open(root, id(4), memory);
        const auto connection = PersistentConnection::recover(session, head, memory, 4096);
        CHECK(connection.state().strength() == 1.01 && connection.state().revision() == expected_revision + 1);
    }
    CHECK(memory.used() == 0);
    {
        auto session = SessionStore::create(root, id(5), "binding", 4096, memory);
        auto connection = PersistentConnection::create(session, id(54), 1, policy, memory, 4096);
        EvidenceObservation value;
        value.hypothesis = id(55); value.source = id(1); value.context = id(2); value.producer = id(3);
        const auto rules = make_evidence_rules(policy);
        const auto wrong = session.append_evidence(rules, {0, 0, "binding", "experiment", "text/plain", {}}, value);
        const auto before = connection.head();
        const auto count = session.original_count();
        expect_throw<std::invalid_argument>([&] { connection.append(wrong.original()); });
        CHECK(connection.state().revision() == 0 && same_location(connection.head(), before));
        CHECK(session.original_count() == count);
        expect_throw<std::invalid_argument>([&] { connection.append(before); }); // a journal is not an observation
        CHECK(connection.state().revision() == 0 && same_location(connection.head(), before));
    }
    CHECK(memory.used() == 0);
    {
        MemoryBudget bounded(64<<10);ExperienceLocation large_head,large_original;
        DigestBytes expected_cue{};
        {
            auto session=SessionStore::create(root,id(6),"large",8<<20,bounded);
            auto connection=PersistentConnection::create(session,id(56),1,policy,bounded,4<<20);
            std::vector<std::byte> payload(2<<20,std::byte{197});
            EvidenceObservation value;value.hypothesis=id(56);value.source=id(1);
            value.context=id(2);value.producer=id(3);
            const auto saved=session.append_evidence(make_evidence_rules(policy),
                {0,0,"large","experiment","application/octet-stream",payload},value);
            large_original=saved.original();expected_cue=saved.cue();
            connection.append(large_original);(void)connection.refine(7,0);large_head=connection.head();
            session.end();session.publish_originals();
        }
        CHECK(bounded.used()==0);
        {
            auto session=SessionStore::open(root,id(6),bounded);
            const auto connection=PersistentConnection::recover(session,large_head,bounded,4<<20);
            CHECK(connection.state().experiences().size()==1);
            CHECK(connection.state().experiences()[0].original()==large_original);
            CHECK(connection.state().experiences()[0].cue()==expected_cue);
            CHECK(connection.head()==large_head&&connection.state().strength()==1);
            // Full Replay still needs a full result buffer. Recovery succeeding
            // here must not be advertised as streaming Replay completion.
            expect_throw<std::bad_alloc>([&]{(void)session.read(large_original,4<<20);});
            CHECK(session.usable());
        }
        CHECK(bounded.used()==0&&bounded.peak_reserved()<=bounded.limit());
    }
    fs::remove_all(root);
    std::printf("PASS: %u persistent connection checks\n", checks);
}
