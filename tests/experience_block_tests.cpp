#include "vrs/experience_block.hpp"
#include "vrs/evidence_experience.hpp"
#include "swegca_architecture/evidence_rules.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <thread>
#include <cerrno>
#include <unistd.h>
#include <vector>

using namespace swegca::vrs;
namespace fs = std::filesystem;
static int writes_left=-1;
static int reads_left=-1;static std::size_t largest_read=0;
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t size,off_t offset){
    largest_read=std::max(largest_read,size);
    if(reads_left==0){reads_left=-1;errno=EIO;return -1;}
    if(reads_left>0)--reads_left;
    return __real_pread(fd,data,size,offset);
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t size,off_t offset){
    if(writes_left==0){writes_left=-1;errno=ENOSPC;return -1;}
    if(writes_left>0)--writes_left;
    return __real_pwrite(fd,data,size,offset);
}
static unsigned checks = 0;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        std::abort(); \
    } \
} while (false)

template<class Exception, class Function>
void expect_throw(Function operation) {
    bool expected = false;
    try { operation(); } catch (const Exception&) { expected = true; }
    CHECK(expected);
}

std::span<const std::byte> bytes(const std::string& value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

int main() {
    static_assert(!std::is_copy_constructible_v<StoredExperience>);
    static_assert(!std::is_copy_assignable_v<StoredExperience>);
    std::array<char, 64> pattern{};
    const std::string base = (fs::temp_directory_path() / "swegca-block-test-XXXXXX").string();
    CHECK(base.size() < pattern.size());
    std::copy(base.begin(), base.end(), pattern.begin());
    const auto made = ::mkdtemp(pattern.data());
    CHECK(made != nullptr);
    const fs::path directory(made);
    const auto path = directory / "original.block";
    swegca::architecture::DigestBytes identity{};
    identity[0] = std::byte{1};
    const auto capacity = std::uint64_t{1} << 20;
    MemoryBudget memory(capacity);
    const std::string raw("첫 대화\0반박과 불확실성", sizeof("첫 대화\0반박과 불확실성") - 1);
    const OriginalExperienceView first{0, 1234, "session-1", "user-input", "text/plain", bytes(raw)};
    std::string binary(100000, '\0');
    for (std::size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<char>(i % 251);
    const OriginalExperienceView second{1, 1235, "session-1", "tool-output", "application/octet-stream", bytes(binary)};
    ExperienceLocation first_location, second_location, empty_location;
    {
        auto block = ExperienceBlock::create(path, identity, capacity);
        CHECK(block.can_append());
        first_location = block.append(first);
        second_location = block.append(second);
        empty_location = block.append({2, 1236, "session-1", "assistant", "text/plain", {}});
        CHECK(first_location.offset == ExperienceBlock::header_bytes);
        CHECK(second_location.offset == first_location.offset + first_location.bytes);
        CHECK(empty_location.offset == second_location.offset + second_location.bytes);
        CHECK(fs::file_size(path) == empty_location.offset + empty_location.bytes);
        expect_throw<std::system_error>([&] { (void)ExperienceBlock::open_writer(path); });
        expect_throw<std::system_error>([&] { (void)ExperienceBlock::create(path, identity, capacity); });
        auto reader = ExperienceBlock::open_reader(path);
        CHECK(!reader.can_append());
        expect_throw<std::logic_error>([&] { (void)reader.append(first); });
        auto original = reader.read(first_location, capacity, memory);
        CHECK(memory.used() == first_location.bytes);
        CHECK(original.view().sequence == 0);
        CHECK(original.view().observed_at_ns == 1234);
        CHECK(original.view().session == first.session);
        CHECK(original.view().source == first.source);
        CHECK(original.view().media_type == first.media_type);
        CHECK(std::ranges::equal(original.view().content, bytes(raw)));
        CHECK(reader.read(empty_location, capacity, memory).view().content.empty());
        auto moved = std::move(original);
        CHECK(std::ranges::equal(moved.view().content, bytes(raw)));
        CHECK(memory.used() == first_location.bytes);
        const auto inspected = reader.inspect();
        CHECK(inspected.complete_records == 3);
        CHECK(inspected.complete_bytes == fs::file_size(path));
        CHECK(inspected.unfinished_bytes == 0);
        expect_throw<std::invalid_argument>([&] { (void)reader.read(first_location, first_location.bytes - 1, memory); });
        auto wrong = first_location;
        wrong.block[1] = std::byte{7};
        expect_throw<std::invalid_argument>([&] { (void)reader.read(wrong, capacity, memory); });
        wrong = first_location;
        wrong.offset = std::numeric_limits<std::uint64_t>::max();
        expect_throw<std::invalid_argument>([&] { (void)reader.read(wrong, capacity, memory); });
        wrong = first_location;
        wrong.bytes = std::numeric_limits<std::uint64_t>::max();
        expect_throw<std::invalid_argument>([&] { (void)reader.read(wrong, capacity, memory); });
        wrong = first_location;
        wrong.digest[0] ^= std::byte{1};
        expect_throw<std::runtime_error>([&] { (void)reader.read(wrong, capacity, memory); });
        wrong = first_location;
        ++wrong.bytes;
        expect_throw<std::runtime_error>([&] { (void)reader.read(wrong, capacity, memory); });
    }
    CHECK(memory.used() == 0);
    {
        auto reader = ExperienceBlock::open_reader(path);
        MemoryBudget exactly_one(first_location.bytes);
        {
            auto held = reader.read(first_location, capacity, exactly_one);
            CHECK(exactly_one.used() == first_location.bytes);
            expect_throw<std::bad_alloc>([&] { (void)reader.read(first_location, capacity, exactly_one); });
            CHECK(exactly_one.used() == first_location.bytes);
            CHECK(std::ranges::equal(held.view().content, bytes(raw)));
        }
        CHECK(exactly_one.used() == 0);
        const auto next = reader.read(first_location, capacity, exactly_one);
        CHECK(next.view().sequence == 0);
    }
    // Writer reopen validates previous records with bounded scratch, then
    // appends without changing the old bytes or locations.
    {
        auto block = ExperienceBlock::open_writer(path);
        CHECK(block.can_append());
        auto moved = std::move(block);
        CHECK(!block.can_append());
        const auto appended = moved.append({3, 1237, "session-1", "user-input", "text/plain", bytes(raw)});
        CHECK(appended.offset == empty_location.offset + empty_location.bytes);
        CHECK(moved.inspect().complete_records == 4);
        const auto saved = moved.read(second_location, capacity, memory);
        CHECK(std::ranges::equal(saved.view().content, bytes(binary)));
    }
    // Short last record at three different physical boundaries. Keep the
    // complete prefix readable and leave every unfinished byte untouched.
    unsigned variant = 0;
    for (const auto tail : std::array<std::uint64_t, 3>{17, 80, second_location.bytes - 3}) {
        const auto torn_path = directory / ("torn-" + std::to_string(variant++));
        fs::copy_file(path, torn_path);
        fs::resize_file(torn_path, second_location.offset + tail);
        const auto before = fs::file_size(torn_path);
        auto torn = ExperienceBlock::open_writer(torn_path);
        CHECK(!torn.can_append());
        const auto recovered = torn.inspect();
        CHECK(recovered.complete_records == 1);
        CHECK(recovered.complete_bytes == second_location.offset);
        CHECK(recovered.unfinished_bytes == tail);
        const auto intact = torn.read(first_location, capacity, memory);
        CHECK(std::ranges::equal(intact.view().content, bytes(raw)));
        expect_throw<std::logic_error>([&] { (void)torn.append(first); });
        expect_throw<std::runtime_error>([&] { (void)torn.read(second_location, capacity, memory); });
        CHECK(fs::file_size(torn_path) == before);
    }
    // Completed record corruption must throw, not masquerade as an unfinished
    // tail. A selected read of an earlier intact record does not scan the bad one.
    const auto corrupt_path = directory / "corrupt.block";
    fs::copy_file(path, corrupt_path);
    {
        std::fstream damaged(corrupt_path, std::ios::in | std::ios::out | std::ios::binary);
        damaged.seekp(static_cast<std::streamoff>(second_location.offset + 64));
        damaged.put('X');
        CHECK(bool(damaged));
    }
    {
        auto corrupt = ExperienceBlock::open_reader(corrupt_path);
        const auto intact = corrupt.read(first_location, capacity, memory);
        CHECK(std::ranges::equal(intact.view().content, bytes(raw)));
        expect_throw<std::runtime_error>([&] { (void)corrupt.read(second_location, capacity, memory); });
        CHECK(memory.used() == first_location.bytes);
        expect_throw<std::runtime_error>([&] { (void)corrupt.inspect(); });
        expect_throw<std::runtime_error>([&] { (void)ExperienceBlock::open_writer(corrupt_path); });
    }
    const auto malformed_path = directory / "malformed.block";
    fs::copy_file(path, malformed_path);
    {
        std::fstream damaged(malformed_path, std::ios::in | std::ios::out | std::ios::binary);
        damaged.seekp(static_cast<std::streamoff>(first_location.offset + 32));
        const std::array<char, 8> impossible_length{char(0xff), char(0xff), char(0xff), char(0xff),
                                                  char(0xff), char(0xff), char(0xff), char(0xff)};
        damaged.write(impossible_length.data(), impossible_length.size());
        CHECK(bool(damaged));
    }
    {
        auto malformed = ExperienceBlock::open_reader(malformed_path);
        expect_throw<std::runtime_error>([&] { (void)malformed.read(first_location, capacity, memory); });
        expect_throw<std::runtime_error>([&] { (void)malformed.inspect(); });
    }
    const auto short_header = directory / "short-header.block";
    fs::copy_file(path, short_header);
    fs::resize_file(short_header, 40);
    expect_throw<std::runtime_error>([&] { (void)ExperienceBlock::open_reader(short_header); });
    CHECK(fs::file_size(short_header) == 40);
    const auto bounded_path = directory / "bounded.block";
    {
        auto bounded = ExperienceBlock::create(bounded_path, identity, ExperienceBlock::header_bytes + first_location.bytes);
        const auto before = fs::file_size(bounded_path);
        auto no_source = first;
        no_source.source = {};
        expect_throw<std::invalid_argument>([&] { (void)bounded.append(no_source); });
        expect_throw<std::length_error>([&] { (void)bounded.append(second); });
        CHECK(fs::file_size(bounded_path) == before);
        CHECK(bounded.can_append());
        const auto exact = bounded.append(first);
        CHECK(exact.bytes == first_location.bytes);
        CHECK(fs::file_size(bounded_path) == bounded.capacity());
        expect_throw<std::length_error>([&] { (void)bounded.append(first); });
        CHECK(bounded.inspect().complete_records == 1);
    }
    {
        const auto one_size=first_location.bytes;
        StorageBudget storage(2*ExperienceBlock::header_bytes+one_size);
        const auto left_path=directory/"quota-left.block",right_path=directory/"quota-right.block";
        {
            auto left=ExperienceBlock::create(left_path,identity,capacity,&storage);
            auto right=ExperienceBlock::create(right_path,identity,capacity,&storage);
            CHECK(storage.used()==2*ExperienceBlock::header_bytes);
            // Failed exclusive creation returns its reservation.
            expect_throw<std::system_error>([&]{(void)ExperienceBlock::create(left_path,identity,capacity,&storage);});
            CHECK(storage.used()==2*ExperienceBlock::header_bytes);
            auto moved=std::move(left);
            (void)moved.append(first);
            CHECK(storage.used()==storage.limit());
            expect_throw<StorageLimit>([&]{(void)right.append(first);});
            CHECK(right.can_append());
            CHECK(fs::file_size(right_path)==ExperienceBlock::header_bytes);
            expect_throw<StorageLimit>([&]{(void)ExperienceBlock::create(directory/"over-quota.block",identity,capacity,&storage);});
            CHECK(!fs::exists(directory/"over-quota.block"));
        }
        // Closing a handle never frees persistent storage accounting.
        CHECK(storage.used()==storage.limit());
        auto reopened=ExperienceBlock::open_writer(right_path,&storage);
        expect_throw<StorageLimit>([&]{(void)reopened.append(first);});
        CHECK(storage.used()==storage.limit());
        // A fresh owner starts from the actual existing extent; opening does
        // not double-charge it. Increasing the configured resource is allowed.
        StorageBudget larger(storage.limit()+one_size,storage.used());
        auto old_left=ExperienceBlock::open_writer(left_path,&larger);
        (void)old_left.append(first);
        CHECK(larger.used()==larger.limit());
    }
    {
        const auto torn_path=directory/"quota-torn.block";
        StorageBudget storage(ExperienceBlock::header_bytes+first_location.bytes);
        auto torn=ExperienceBlock::create(torn_path,identity,capacity,&storage);
        writes_left=1; // The prefix reaches disk; the following write fails.
        expect_throw<std::system_error>([&]{(void)torn.append(first);});
        CHECK(!torn.can_append());
        CHECK(storage.used()==storage.limit());
        CHECK(fs::file_size(torn_path)>ExperienceBlock::header_bytes);
        CHECK(fs::file_size(torn_path)<storage.used());
        CHECK(torn.inspect().unfinished_bytes>0);
        expect_throw<StorageLimit>([&]{StorageBudget::Reservation r(&storage,1);});
    }
    {
        StorageBudget shared(1000);
        std::atomic<unsigned> successes{0};
        const auto worker=[&]{for(unsigned n=0;n<1000;++n){
            try{StorageBudget::Reservation r(&shared,1);r.retain();++successes;}
            catch(const StorageLimit&){}
        }};
        std::thread a(worker),b(worker);a.join();b.join();
        CHECK(shared.used()==1000&&successes==1000);
        StorageBudget edge(std::numeric_limits<std::uint64_t>::max(),std::numeric_limits<std::uint64_t>::max()-1);
        {StorageBudget::Reservation r(&edge,1);r.retain();}
        expect_throw<StorageLimit>([&]{StorageBudget::Reservation r(&edge,1);});
        expect_throw<StorageLimit>([]{StorageBudget invalid(1,2);});
    }
    {
        using namespace swegca::architecture;
        using namespace swegca::architecture::kernel;
        const auto rules=make_evidence_rules(EvidencePolicy{});
        EvidenceObservation value;value.hypothesis=identity;value.source=identity;
        value.context=identity;value.producer=identity;value.observed_at=first.observed_at_ns;
        // The segmented envelope has eight nonempty writes. Failure at any
        // boundary must preserve the previous record and poison this writer.
        for(int point=0;point<8;++point){
            auto block=ExperienceBlock::create(directory/("parts-fail-"+std::to_string(point)),identity,capacity);
            const auto previous=block.append(first);
            writes_left=point;
            expect_throw<std::system_error>([&]{(void)record_evidence(block,rules,first,value);});
            CHECK(writes_left==-1&&!block.can_append());
            CHECK(block.inspect().complete_records==1);
            CHECK(block.read(previous,capacity,memory).location()==previous);
            expect_throw<std::logic_error>([&]{(void)record_evidence(block,rules,first,value);});
        }
    }
    {
        using namespace swegca::architecture;using namespace swegca::architecture::kernel;
        const auto rules=make_evidence_rules(EvidencePolicy{});MemoryBudget full_read_memory(8<<20);
        EvidenceObservation value;value.hypothesis=identity;value.source=identity;
        value.context=identity;value.producer=identity;value.observed_at=first.observed_at_ns;
        unsigned index=0;
        for(const auto media_size:{1U,65359U,65360U,65536U,65537U}){
            auto block=ExperienceBlock::create(directory/("stream-"+std::to_string(index++)),identity,8<<20);
            std::string media(media_size,'m');std::vector<std::byte> payload(2<<20,std::byte{129});
            auto input=first;input.media_type=media;input.content=payload;
            const auto saved=record_evidence(block,rules,input,value);
            const auto stored=block.read(saved.original(),8<<20,full_read_memory);
            const auto expected=decode_evidence(rules,stored);
            largest_read=0;
            const auto streamed=read_evidence(rules,block,saved.original(),8<<20);
            CHECK(largest_read<=65536);
            CHECK(streamed.cue()==expected.cue()&&streamed.original()==expected.original());
            CHECK(streamed.value().observed_at==expected.value().observed_at);
            CHECK(streamed.value().producer==expected.value().producer);
            CHECK(streamed.value().outcome==expected.value().outcome);
            expect_throw<std::invalid_argument>([&]{(void)read_evidence(rules,block,saved.original(),saved.original().bytes-1);});
            reads_left=6;expect_throw<std::system_error>([&]{(void)read_evidence(rules,block,saved.original(),8<<20);});
            CHECK(reads_left==-1);
            // Corruption in a late payload byte must be found even though the
            // observation prefix itself was already completely decoded.
            const auto path=directory/("stream-"+std::to_string(index-1));
            std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
            file.seekp(static_cast<std::streamoff>(saved.original().offset+saved.original().bytes-49));
            file.put('X');file.flush();CHECK(bool(file));
            expect_throw<std::runtime_error>([&]{(void)read_evidence(rules,block,saved.original(),8<<20);});
        }
        auto block=ExperienceBlock::create(directory/"stream-empty",identity,4096);
        auto empty=first;empty.content={};
        const auto saved=record_evidence(block,rules,empty,value);
        CHECK(read_evidence(rules,block,saved.original(),4096).cue()==saved.cue());
    }
    expect_throw<std::invalid_argument>([&] { (void)ExperienceBlock::create(directory / "zero.block", {}, capacity); });
    CHECK(!fs::exists(directory / "zero.block"));
    CHECK(memory.used() == 0);
    fs::remove_all(directory);  // Only this mkdtemp-owned test directory.
    std::printf("PASS: %u experience block checks\n", checks);
}
