#include "vrs/file_observation.hpp"
#include "swegca_architecture/content_observation_kernel.hpp"
#include "vrs/memory_budget.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using namespace swegca::vrs;
using swegca::architecture::kernel::EvidenceOutcome;
using swegca::architecture::kernel::observe_content_relation;
static_assert(to_outcome(observe_content_relation(true,true,false,false))==EvidenceOutcome::support);
static_assert(to_outcome(observe_content_relation(true,true,true,false))==EvidenceOutcome::refute);
static_assert(to_outcome(observe_content_relation(false,true,false,false))==EvidenceOutcome::insufficient);
static_assert(to_outcome(observe_content_relation(true,false,false,false))==EvidenceOutcome::insufficient);
static unsigned checks = 0;
#define CHECK(e) do { ++checks; if (!(e)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e); std::abort(); } } while(false)

static int calls = 0, fail_at = 0, mutate_at = 0, mutation_fd = -1;
static bool interrupted = false, short_reads = false;
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* bytes, size_t count, off_t offset) {
    ++calls;
    if (interrupted) { interrupted = false; errno = EINTR; return -1; }
    if (calls == fail_at) { errno = EIO; return -1; }
    if (calls == mutate_at) {
        // Same size but new content after the first read: final metadata must
        // prevent an apparently matching byte sample from becoming support.
        CHECK(::pwrite(mutation_fd, "X", 1, 0) == 1);
        struct timespec times[2]{{100,0},{100,0}};
        CHECK(::futimens(mutation_fd, times) == 0);
    }
    if (short_reads && count > 1) count /= 2;
    return __real_pread(fd, bytes, count, offset);
}

struct File {
    int fd;
    File() { char name[]="/tmp/swegca-observation-XXXXXX"; fd=::mkstemp(name); CHECK(fd>=0); CHECK(::unlink(name)==0); }
    ~File() { ::close(fd); }
    void set(std::string_view content) {
        CHECK(::ftruncate(fd,0)==0);
        CHECK(::pwrite(fd,content.data(),content.size(),0)==static_cast<ssize_t>(content.size()));
    }
};

int main() {
    MemoryBudget memory(65536); TransferBudget transfer(625000000);
    File a,b;
    auto measure = [&](std::uint64_t limit=1000000) {
        calls=0; auto value=observe_file_equality(a.fd,b.fd,limit,memory,transfer);
        CHECK(memory.used()==0); return value;
    };
    auto empty=measure(0);
    CHECK(empty.complete && empty.stable && empty.outcome==EvidenceOutcome::support);
    CHECK(empty.left.digest==swegca::architecture::Sha256::of({}));
    std::string content(70001,'a'); content[0]='\0'; content[32768]='\xff'; content.back()='z';
    a.set(content); b.set(content);
    CHECK(::lseek(a.fd,13,SEEK_SET)==13 && ::lseek(b.fd,19,SEEK_SET)==19);
    auto same=measure();
    CHECK(same.complete && same.stable && same.outcome==EvidenceOutcome::support);
    CHECK(same.left.bytes_read==content.size() && same.right.bytes_read==content.size());
    CHECK(same.left.digest==swegca::architecture::Sha256::of(std::as_bytes(std::span(content))));
    CHECK(same.left.digest==same.right.digest);
    CHECK(::lseek(a.fd,0,SEEK_CUR)==13 && ::lseek(b.fd,0,SEEK_CUR)==19);
    interrupted=true;short_reads=true;
    same=measure();
    CHECK(same.complete && same.outcome==EvidenceOutcome::support);
    short_reads=false;
    content.back()='!'; b.set(content);
    auto different=measure();
    CHECK(different.complete && different.stable && different.outcome==EvidenceOutcome::refute);
    CHECK(different.left.digest!=different.right.digest);
    b.set("short");
    different=measure();
    CHECK(different.complete && different.outcome==EvidenceOutcome::refute);
    CHECK(different.left.bytes_read==70001 && different.right.bytes_read==5);
    fail_at=3;
    auto failed=measure();
    CHECK(!failed.complete && failed.io_error==EIO && failed.outcome==EvidenceOutcome::insufficient);
    fail_at=0;
    auto limited=measure(65536);
    CHECK(!limited.complete && limited.io_error==EFBIG && limited.outcome==EvidenceOutcome::insufficient && calls==0);
    a.set("same");b.set("same");
    mutation_fd=a.fd;mutate_at=2;
    auto changed=measure();
    CHECK(changed.complete && !changed.stable && changed.outcome==EvidenceOutcome::insufficient);
    mutate_at=0;
    MemoryBudget tiny(65535); bool threw=false;
    try { (void)observe_file_equality(a.fd,b.fd,100,tiny,transfer); } catch(const std::bad_alloc&) { threw=true; }
    CHECK(threw && tiny.used()==0);
    int pipe_fds[2];CHECK(::pipe(pipe_fds)==0);
    auto nonregular=observe_file_equality(pipe_fds[0],b.fd,100,memory,transfer);
    CHECK(nonregular.outcome==EvidenceOutcome::insufficient && nonregular.io_error==EINVAL);
    ::close(pipe_fds[0]);::close(pipe_fds[1]);
    auto invalid=observe_file_equality(-1,b.fd,100,memory,transfer);
    CHECK(invalid.io_error==EBADF && invalid.outcome==EvidenceOutcome::insufficient);
    // A regular pseudo-file with st_size==0 must not masquerade as empty.
    const int proc=::open("/proc/self/status",O_RDONLY|O_CLOEXEC);CHECK(proc>=0);b.set("");
    auto pseudo=observe_file_equality(proc,b.fd,100,memory,transfer);::close(proc);
    CHECK(!pseudo.complete && pseudo.outcome==EvidenceOutcome::insufficient);
    CHECK(memory.peak_reserved()==65536 && memory.used()==0);
    CHECK(::fcntl(a.fd,F_GETFD)>=0 && ::fcntl(b.fd,F_GETFD)>=0);
    std::printf("file observation tests: %u checks passed\n",checks);
}
