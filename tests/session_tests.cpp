#include "swegca_architecture/evidence_rules.hpp"
#include "vrs/connection.hpp"
#include "vrs/session_store.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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
template<class Exception, class Function> void expect_throw(Function operation) {
    bool caught = false;
    try { operation(); } catch (const Exception&) { caught = true; }
    CHECK(caught);
}
DigestBytes id(unsigned value) { DigestBytes digest{}; digest[0] = std::byte(value); return digest; }
std::span<const std::byte> bytes(const std::string& value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}
std::string text(std::span<const std::byte> data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}
fs::path session_path(const fs::path& root, unsigned identity) {
    std::array<char, 3> prefix{};
    std::snprintf(prefix.data(), prefix.size(), "%02x", identity);
    return root / "sessions" / (std::string(prefix.data()) + std::string(62, '0'));
}
std::pair<unsigned, std::uint64_t> data_files(const fs::path& directory) {
    unsigned files = 0; std::uint64_t size = 0;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().starts_with("b-")) { ++files; size += entry.file_size(); }
    return {files, size};
}

int main() {
    for (unsigned phase = 0; phase < 5; ++phase)
        for (unsigned operation = 0; operation < 5; ++operation) {
            SessionPhase next = SessionPhase::published;
            const bool allowed = (phase == 1 && (operation == 1 || operation == 2)) || (phase == 2 && operation == 3);
            CHECK(next_session_phase(static_cast<SessionPhase>(phase), static_cast<SessionOperation>(operation), next) == allowed);
            CHECK(next == (allowed ? static_cast<SessionPhase>(operation == 1 ? 1 : operation) : SessionPhase::invalid));
        }
    auto pattern = (fs::temp_directory_path() / "swegca-session-XXXXXX").string();
    const auto made = ::mkdtemp(pattern.data()); CHECK(made != nullptr);
    const fs::path root(made);
    MemoryBudget memory(16 << 20);
    const auto rules = make_evidence_rules(EvidencePolicy{});
    const std::string raw("user\0목적과 반증을 보존", sizeof("user\0목적과 반증을 보존") - 1);
    std::vector<ExperienceLocation> addresses;
    {
        auto session = SessionStore::create(root, id(1), "session-a", 1024, memory);
        CHECK(session.phase() == SessionPhase::active && session.original_count() == 0);
        CHECK(fs::is_empty(root / "main"));
        expect_throw<std::system_error>([&] { (void)SessionStore::open(root, id(1), memory); });
        expect_throw<std::logic_error>([&] { session.publish_originals(); });
        CHECK(session.usable() && session.phase() == SessionPhase::active);
        for (unsigned i = 0; i < 12; ++i)
            addresses.push_back(session.append({i, 100 + i, "session-a", i % 2 ? "assistant" : "user", "application/octet-stream", bytes(raw)}));
        CHECK(session.block_count() > 1 && session.original_count() == 12);
        CHECK(fs::is_empty(root / "main"));
        const auto read = session.read(addresses.front(), 4096);
        CHECK(text(read.view().content) == raw);
        CHECK(read.view().source == "user" && read.view().observed_at_ns == 100);
        auto foreign = addresses[0]; foreign.block = id(98);
        expect_throw<std::invalid_argument>([&] { (void)session.read(foreign, 4096); });
        expect_throw<std::invalid_argument>([&] { (void)session.append({0, 0, "other-session", "user", "text/plain", bytes(raw)}); });
        CHECK(session.original_count() == 12);
    }
    CHECK(memory.used() == 0);
    {
        auto resumed = SessionStore::open(root, id(1), memory);
        CHECK(resumed.phase() == SessionPhase::active);
        CHECK(resumed.original_count() == 12 && resumed.name() == "session-a");
        addresses.push_back(resumed.append({12, 112, "session-a", "tool", "application/octet-stream", bytes(raw)}));
        const auto before = data_files(session_path(root, 1));
        resumed.end();
        CHECK(resumed.phase() == SessionPhase::ended);
        CHECK(fs::is_empty(root / "main"));
        CHECK(data_files(session_path(root, 1)) == before);
        expect_throw<std::logic_error>([&] { (void)resumed.append({13, 0, "session-a", "user", "text/plain", bytes(raw)}); });
        expect_throw<std::logic_error>([&] { resumed.end(); });
        CHECK(resumed.original_count() == 13);
    }
    {
        auto ended = SessionStore::open(root, id(1), memory);
        CHECK(ended.phase() == SessionPhase::ended && ended.original_count() == 13);
        const auto before = data_files(session_path(root, 1));
        ended.publish_originals();
        CHECK(ended.phase() == SessionPhase::published);
        CHECK(!fs::is_empty(root / "main"));
        CHECK(data_files(session_path(root, 1)) == before);
        for (const auto& location : addresses) CHECK(text(ended.read(location, 4096).view().content) == raw);
        expect_throw<std::logic_error>([&] { ended.publish_originals(); });
    }
    {
        auto published = SessionStore::open(root, id(1), memory);
        CHECK(published.phase() == SessionPhase::published);
        CHECK(text(published.read(addresses.back(), 4096).view().content) == raw);
        expect_throw<std::logic_error>([&] { (void)published.append({0, 0, "session-a", "user", "text/plain", bytes(raw)}); });
    }
    CHECK(memory.used() == 0);
    {
        // Live session originals -> typed observation -> shuffle -> core ->
        // same connection. A previously published session remains readable.
        auto session = SessionStore::create(root, id(2), "session-b", 2048, memory);
        auto main = SessionStore::open(root, id(1), memory);
        Connection connection(id(88), 1, rules, memory);
        for (unsigned axis = 0; axis < 4; ++axis)
            for (unsigned group = 0; group < 12; ++group) {
                EvidenceObservation value;
                value.hypothesis = id(88); value.axis = axis; value.outcome = EvidenceOutcome::support;
                value.source = id(group + 1); value.context = id(group + 21); value.producer = id(group + 41);
                const auto saved = session.append_evidence(rules,
                    {axis * 12 + group, 0, "session-b", "experiment", "text/plain", bytes(raw)}, value);
                const auto stored = session.read(saved.original(), 4096);
                CHECK(text(evidence_payload(stored).content) == raw);
                const auto part=session.read_payload_slice(rules,saved.original(),4096,2,7);
                CHECK(text(part.content())==raw.substr(2,7));
                CHECK(part.evidence().original()==saved.original() && part.total_bytes()==raw.size());
                CHECK(session.phase()==SessionPhase::active);
                expect_throw<std::invalid_argument>([&]{(void)main.read_payload_slice(rules,saved.original(),4096,0,1);});
                connection.append(decode_evidence(rules, stored));
            }
        const auto result = connection.refine(20260926, 0);
        CHECK(result.result().verification().judgment().status() == EvidenceStatus::accept);
        CHECK(connection.strength() == 1.01);
        CHECK(session.original_count() == 48 && session.block_count() > 1);
        CHECK(text(main.read(addresses.front(), 4096).view().content) == raw);
        expect_throw<std::logic_error>([&] { session.publish_originals(); });
        session.end(); session.publish_originals();
    }
    CHECK(memory.used() == 0);
    // Preserve an interrupted data tail, then continue in a new block.
    ExperienceLocation before_tail;
    {
        auto session = SessionStore::create(root, id(3), "tail", 1024, memory);
        before_tail = session.append({0, 0, "tail", "user", "text/plain", bytes(raw)});
    }
    const auto tail_path = session_path(root, 3) / "b-0000000000000000.block";
    const auto complete_size = fs::file_size(tail_path);
    { std::ofstream file(tail_path, std::ios::binary | std::ios::app); file << "torn"; }
    {
        auto recovered = SessionStore::open(root, id(3), memory);
        CHECK(recovered.original_count() == 1 && recovered.phase() == SessionPhase::active);
        const auto after_tail = recovered.append({1, 1, "tail", "tool", "text/plain", bytes(raw)});
        CHECK(recovered.block_count() == 2 && after_tail.block != before_tail.block);
        CHECK(fs::file_size(tail_path) == complete_size + 4);
        CHECK(text(recovered.read(before_tail, 4096).view().content) == raw);
        recovered.end(); recovered.publish_originals();
    }
    {
        auto recovered = SessionStore::open(root, id(3), memory);
        CHECK(recovered.phase() == SessionPhase::published && recovered.original_count() == 2);
        CHECK(fs::file_size(tail_path) == complete_size + 4);
    }
    // An unfinished end staging file never ends or publishes a session.
    {
        auto session = SessionStore::create(root, id(4), "interrupted-end", 1024, memory);
        (void)session.append({0, 0, "interrupted-end", "user", "text/plain", bytes(raw)});
    }
    const auto staging = session_path(root, 4) / "ending-0.block";
    { std::ofstream file(staging); file << 'x'; }
    {
        auto session = SessionStore::open(root, id(4), memory);
        CHECK(session.phase() == SessionPhase::active);
        expect_throw<std::logic_error>([&] { session.publish_originals(); });
        session.end();
        CHECK(session.phase() == SessionPhase::ended && fs::file_size(staging) == 1);
        CHECK(fs::exists(session_path(root, 4) / "ending-1.block"));
    }
    // The selected physical block bound is explicit; oversized input fails
    // without truncating it or changing the previously stored originals.
    {
        auto session = SessionStore::create(root, id(5), "bounded", 512, memory);
        const auto first = session.append({0, 0, "bounded", "user", "text/plain", bytes(raw)});
        const std::string large(2048, 'x');
        expect_throw<std::length_error>([&] { (void)session.append({1, 1, "bounded", "tool", "text/plain", bytes(large)}); });
        CHECK(session.usable() && session.original_count() == 1);
        CHECK(text(session.read(first, 4096).view().content) == raw);
        (void)session.append({2, 2, "bounded", "tool", "text/plain", bytes(raw)});
        CHECK(session.original_count() == 2);
    }
    CHECK(memory.used() == 0);
    {
        auto a = SessionStore::create(root, id(6), "parallel-a", 1024, memory);
        auto b = SessionStore::create(root, id(7), "parallel-b", 1024, memory);
        std::thread one([&] { for (unsigned i = 0; i < 12; ++i) (void)a.append({i, i, "parallel-a", "user", "text/plain", bytes(raw)}); a.end(); a.publish_originals(); });
        std::thread two([&] { for (unsigned i = 0; i < 12; ++i) (void)b.append({i, i, "parallel-b", "tool", "text/plain", bytes(raw)}); b.end(); b.publish_originals(); });
        one.join(); two.join();
        CHECK(a.original_count() == 12 && b.original_count() == 12);
        CHECK(a.phase() == SessionPhase::published && b.phase() == SessionPhase::published);
    }
    CHECK(memory.used() == 0);
    {
        // A valid re-encoded record with the same length/count still differs
        // from the ended inventory; body checksum alone must not hide this.
        ExperienceLocation location;
        {
            auto session = SessionStore::create(root, id(8), "bound", 1024, memory);
            location = session.append({0, 0, "bound", "user", "text/plain", bytes(raw)});
            session.end();
        }
        const auto directory = session_path(root, 8);
        const auto replacement = directory / "replacement.block";
        {
            auto block = ExperienceBlock::create(replacement, location.block, 1024);
            std::string changed = raw; changed[0] = 'X';
            const auto new_location = block.append({0, 0, "bound", "user", "text/plain", bytes(changed)});
            CHECK(new_location.bytes == location.bytes && new_location.digest != location.digest);
        }
        fs::rename(replacement, directory / "b-0000000000000000.block");
        expect_throw<std::runtime_error>([&] { (void)SessionStore::open(root, id(8), memory); });
    }
    CHECK(memory.used() == 0);
    {
        ExperienceLocation input;
        const std::string metadata=R"({"memory":{"completed":true,"original":null}})";
        const std::string updated=R"({"memory":{"completed":true,"step":3}})";
        DigestBytes revision{},later{},wide_revision{};
        const std::string wide(131073,'w');
        StorageBudget storage(1<<20);
        {
            auto session=SessionStore::create(root,id(9),"cognition",1024,memory,&storage);
            input=session.append({0,0,"cognition","user","text/plain",bytes(raw)});
            CHECK(!session.read_cognition(input));
            auto forged=input;forged.digest=id(99);
            expect_throw<std::invalid_argument>([&]{session.save_cognition(forged,bytes(metadata));});
            CHECK(!session.read_cognition(forged));
            const auto count=session.original_count(),before=storage.used();
            session.save_cognition(input,bytes(metadata));
            CHECK(storage.used()>before && session.original_count()==count);
            const auto used=storage.used();
            session.save_cognition(input,bytes(metadata));
            CHECK(storage.used()==used && session.original_count()==count);
            CHECK(text(session.read_cognition(input)->view().content)==metadata);
            revision=session.save_cognition_revision(input,bytes(updated));
            const auto revision_used=storage.used();CHECK(revision_used>used);
            CHECK(session.save_cognition_revision(input,bytes(updated))==revision);
            CHECK(storage.used()==revision_used&&session.original_count()==count);
            later=session.save_cognition_revision(input,bytes("later core assessment"));
            CHECK(later!=revision&&session.original_count()==count);
            wide_revision=session.save_cognition_revision(input,bytes(wide));
            CHECK(text(session.read_cognition_revision(input,wide_revision)->view().content)==wide);
            const auto remaining=memory.limit()-memory.used();auto* held=memory.allocate(remaining);
            expect_throw<std::bad_alloc>([&]{(void)session.read_cognition_revision(input,wide_revision);});
            memory.deallocate(held,remaining);
            CHECK(session.usable());
            CHECK(text(session.read_cognition_revision(input,wide_revision)->view().content)==wide);
            CHECK(text(session.read_cognition_revision(input,revision)->view().content)==updated);
            CHECK(text(session.read_cognition(input)->view().content)==metadata);
            CHECK(!session.read_cognition_revision(forged,revision));
            CHECK(!session.read_cognition_revision(input,id(99)));
            expect_throw<std::invalid_argument>([&]{(void)session.save_cognition_revision(forged,bytes(updated));});
            expect_throw<std::length_error>([&]{(void)session.save_cognition_revision(input,{});});
            expect_throw<std::length_error>([&]{(void)session.save_cognition_revision(input,bytes(std::string(memory.limit(),'x')));});
            expect_throw<std::invalid_argument>([&]{session.save_cognition(input,bytes("different"));});
            expect_throw<std::length_error>([&]{session.save_cognition(input,bytes(std::string(memory.limit(),'x')));});
            auto foreign=input;foreign.block=id(99);
            expect_throw<std::invalid_argument>([&]{session.save_cognition(foreign,bytes(metadata));});
            CHECK(session.usable());
        }
        {
            auto session=SessionStore::open(root,id(9),memory,&storage);
            CHECK(text(session.read_cognition(input)->view().content)==metadata);
            CHECK(text(session.read_cognition_revision(input,revision)->view().content)==updated);
            CHECK(text(session.read_cognition_revision(input,later)->view().content)=="later core assessment");
            CHECK(text(session.read_cognition_revision(input,wide_revision)->view().content)==wide);
            // An interrupted staging write is not a committed receipt.
            const auto directory=session_path(root,9)/"cognition";
            std::ofstream(directory/"staging-interrupted.block")<<"incomplete";
            session.end();
            expect_throw<std::logic_error>([&]{session.save_cognition(input,bytes(metadata));});
            expect_throw<std::logic_error>([&]{(void)session.save_cognition_revision(input,bytes(updated));});
        }
        {
            auto session=SessionStore::open(root,id(9),memory,&storage);
            CHECK(session.phase()==SessionPhase::ended);
            CHECK(text(session.read_cognition(input)->view().content)==metadata);
            CHECK(text(session.read_cognition_revision(input,revision)->view().content)==updated);
            session.publish_originals();
        }
        // Removing a committed receipt cannot silently alter an ended session.
        for(const auto& entry:fs::directory_iterator(session_path(root,9)/"cognition"))
            if(!entry.path().filename().string().starts_with("staging-"))fs::remove(entry.path());
        expect_throw<std::runtime_error>([&]{(void)SessionStore::open(root,id(9),memory);});
    }
    CHECK(memory.used()==0);
    {
        ExperienceLocation input;
        {
            auto session=SessionStore::create(root,id(10),"corrupt-cognition",1024,memory);
            input=session.append({0,0,"corrupt-cognition","user","text/plain",bytes(raw)});
            session.save_cognition(input,bytes("immutable result"));
        }
        for(const auto& entry:fs::directory_iterator(session_path(root,10)/"cognition"))
            if(!entry.path().filename().string().starts_with("staging-"))fs::resize_file(entry.path(),81);
        auto session=SessionStore::open(root,id(10),memory);
        expect_throw<std::runtime_error>([&]{(void)session.read_cognition(input);});
    }
    CHECK(memory.used()==0);
    fs::remove_all(root);
    std::printf("PASS: %u session checks\n", checks);
}
