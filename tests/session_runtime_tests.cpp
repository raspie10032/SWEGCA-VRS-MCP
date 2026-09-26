#include "vrs/session_runtime.hpp"
#include "swegca_architecture/input_cue.hpp"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs = std::filesystem;
static unsigned checks = 0;
static std::uint64_t reads = 0, writes = 0;
static bool fail_read = false;
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t count, off_t offset) {
    ++reads;
    if (fail_read) { fail_read=false; errno=EIO; return -1; }
    return __real_pread(fd, data, count, offset);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t count, off_t offset) {
    ++writes; return __real_pwrite(fd, data, count, offset);
}
#define CHECK(e) do { ++checks; if (!(e)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #e); std::abort(); } } while (false)
template<class E, class F> void throws(F f) { bool caught=false; try { f(); } catch (const E&) { caught=true; } CHECK(caught); }
DigestBytes id(unsigned n) { DigestBytes d{}; d[0]=std::byte(n); return d; }
const std::string payload("original\0with bytes", 19);
OriginalExperienceView input(std::string_view session, unsigned n) {
    return {n, n, session, "experiment", "application/octet-stream", std::as_bytes(std::span(payload))};
}
RecordedRefinement observe(SessionRuntime& runtime, std::string_view session, unsigned key, unsigned n, EvidenceOutcome outcome) {
    EvidenceObservation value;
    value.hypothesis=id(key); value.source=id(n+20); value.context=id(n+50); value.producer=id(n+90);
    value.observed_at=n; value.outcome=outcome;
    return runtime.observe(id(key), input(session,n), value, n, n);
}
int main() {
    auto pattern=(fs::temp_directory_path()/"swegca-runtime-XXXXXX").string();
    CHECK(::mkdtemp(pattern.data()) != nullptr);
    fs::path root(pattern);
    MemoryBudget memory(32 << 20);
    EvidencePolicy policy; policy.axis_count=1;
    const std::string followup="그 다음은 어떻게 이어지지?";
    CHECK(familiarity_key(false,false)==FamiliarityKey::missing);
    CHECK(familiarity_key(true,true)==FamiliarityKey::exact);
    CHECK(familiarity_key(false,true)==FamiliarityKey::continuation);
    ExperienceLocation first;
    double saved_strength=0;
    {
        auto old_store=SessionStore::create(root,id(1),"old",65536,memory);
        SessionRuntime old(old_store,memory,8192);
        old.define_connection(id(10),1.0,policy);
        CHECK(old.find(id(10))==nullptr); // Unobserved definitions are not recall candidates.
        auto raw_input=input("old",0); raw_input.content={};
        auto raw=old.retain_input(raw_input,0.75,policy,0,0);
        CHECK(evidence_payload(old_store.read(raw.original,8192)).content.empty());
        for (unsigned n=1;n<=16;++n) {
            auto result=observe(old,"old",10,n,EvidenceOutcome::support);
            if (n==1) first=result.original;
            CHECK(result.refinement.connection()==id(10));
            CHECK(old.find(id(10))->state().strength()==result.refinement.result().strength().current());
        }
        saved_strength=old.find(id(10))->state().strength(); CHECK(saved_strength>1.0);
        throws<std::invalid_argument>([&] { old.define_connection(id(10),1.0,policy); });
        auto live_store=SessionStore::create(root,id(2),"live",65536,memory);
        SessionRuntime live(live_store,memory,8192);
        ExperienceRouter router(live,memory);
        throws<std::logic_error>([&] { router.mount_main(old); });
        throws<std::logic_error>([&] { old.publish_originals(); });
        CHECK(old.phase()==SessionPhase::active);
        old.end();
        throws<std::logic_error>([&] { router.mount_main(old); });
        old.publish_originals(); router.mount_main(old); router.mount_main(old);
        CHECK(!router.input("text/plain",std::as_bytes(std::span(followup))).familiar());
        const auto reads_before=reads, writes_before=writes;
        auto natural=router.input("application/octet-stream",std::as_bytes(std::span(payload)));
        CHECK(reads==reads_before && writes==writes_before);
        CHECK(natural.familiar() && !natural.temporary() && natural.matches().size()==16);
        CHECK(router.replay(natural,0).location()==first);
        CHECK(reads>reads_before && writes==writes_before);
        CHECK(router.input("text/plain",std::as_bytes(std::span(payload))).key_kind()==FamiliarityKey::continuation);
        const auto context_reads=reads, context_writes=writes;
        auto continued=router.input("text/plain",std::as_bytes(std::span(followup)));
        CHECK(continued.key_kind()==FamiliarityKey::continuation && !continued.temporary());
        CHECK(continued.matches().size()==16 && reads==context_reads && writes==context_writes);
        CHECK(router.replay(continued,0).location()==first);
        auto recalled=router.recall(id(10));
        CHECK(!recalled.temporary() && recalled.size()==1);
        auto replayed=router.replay(recalled,0,0);
        CHECK(replayed.location()==first);
        const auto content=evidence_payload(replayed).content;
        CHECK(std::string_view(reinterpret_cast<const char*>(content.data()),content.size())==payload);
        throws<std::out_of_range>([&] { (void)router.replay(recalled,0,999); });
        CHECK(router.recall(id(99)).size()==0);
        live.define_connection(id(10),0.75,policy);
        CHECK(!router.recall(id(10)).temporary());
        auto local=observe(live,"live",10,1,EvidenceOutcome::insufficient);
        CHECK(local.refinement.result().verification().judgment().status()==EvidenceStatus::abstain);
        auto preferred=router.recall(id(10));
        CHECK(preferred.temporary() && preferred.size()==1);
        CHECK(router.replay(preferred,0,0).location()==local.original);
        auto natural_local=router.input("application/octet-stream",std::as_bytes(std::span(payload)));
        CHECK(natural_local.temporary() && natural_local.matches().size()==1);
        CHECK(router.replay(natural_local,0).location()==local.original);
        (void)observe(live,"live",10,2,EvidenceOutcome::support);
        throws<std::logic_error>([&] { (void)router.replay(preferred,0,0); });
        throws<std::logic_error>([&] { (void)router.replay(natural_local,0); });
        CHECK(router.replay(router.recall(id(10)),0,0).location()==local.original);
        CHECK(old.find(id(10))->state().strength()==saved_strength);
        // Separate Main session lineage must not overwrite the first one.
        auto other_store=SessionStore::create(root,id(3),"other",65536,memory);
        SessionRuntime other(other_store,memory,8192);
        other.define_connection(id(10),1.0,policy);
        for (unsigned n=1;n<=16;++n) (void)observe(other,"other",10,n,EvidenceOutcome::refute);
        CHECK(other.find(id(10))->state().strength()<1.0);
        other.end(); other.publish_originals(); router.mount_main(other);
        CHECK(router.recall(id(10)).temporary());
        auto blank_store=SessionStore::create(root,id(4),"blank",65536,memory);
        SessionRuntime blank(blank_store,memory,8192);
        ExperienceRouter fallback(blank,memory); fallback.mount_main(old); fallback.mount_main(other);
        CHECK(fallback.recall(id(10)).size()==2);
        CHECK(fallback.replay(fallback.recall(id(10)),0,0).location()==first);
        CHECK(fallback.input("application/octet-stream",std::as_bytes(std::span(payload))).matches().size()==32);
        // A natural utterance without a known outcome remains insufficient,
        // but is retained through the actual shuffle/core/publication path.
        const std::string utterance="스웨카가 셔플값을 검증한다.";
        const OriginalExperienceView message{25,25,"live","user","text/plain",std::as_bytes(std::span(utterance))};
        CHECK(router.input(message.media_type,message.content).key_kind()==FamiliarityKey::continuation);
        auto retained=live.retain_input(message,0.75,policy,25,25);
        CHECK(retained.refinement.result().verification().judgment().status()==EvidenceStatus::abstain);
        CHECK(retained.refinement.result().strength().current()==0.75);
        auto known=router.input(message.media_type,message.content);
        CHECK(known.temporary() && known.matches().size()==1);
        CHECK(known.key_kind()==FamiliarityKey::exact);
        fail_read=true;
        throws<std::system_error>([&] { (void)router.replay(known,0); });
        CHECK(router.input("text/plain",std::as_bytes(std::span(followup))).matches()[0].recalled.recalled_head.identity==id(10));
        auto selected=router.replay(known,0);
        CHECK(selected.location()==retained.original);
        CHECK(evidence_payload(selected).content.size()==message.content.size());
        CHECK(live.find(input_cue(message.media_type,message.content))->state().experiences()[0].value().outcome==EvidenceOutcome::insufficient);
        auto continued_new=router.input("text/plain",std::as_bytes(std::span(followup)));
        CHECK(continued_new.temporary() && continued_new.matches().size()==1);
        CHECK(continued_new.matches()[0].recalled.recalled_head.identity==known.cue());
        // A temporary continued experience is considered before an exact key
        // that exists only in Main (the initial empty old-session original).
        auto temporary_context=router.input("application/octet-stream",{});
        CHECK(temporary_context.temporary() && temporary_context.key_kind()==FamiliarityKey::continuation);
        // Fresh router/session dialogue state does not inherit a prior key.
        ExperienceRouter fresh(live,memory); fresh.mount_main(old);
        CHECK(!fresh.input("text/plain",std::as_bytes(std::span(followup))).familiar());
        // Temporary write failure must not become a false miss and Main fallback.
        blank.define_connection(id(10),1.0,policy);
        std::string huge(100000,'x'); auto oversized=input("blank",1);
        oversized.content=std::as_bytes(std::span(huge));
        throws<std::length_error>([&] { (void)blank.retain_input(oversized,0.75,policy,1,1); });
        CHECK(!blank.usable());
        throws<std::logic_error>([&] { (void)fallback.recall(id(10)); });
        throws<std::logic_error>([&] { (void)fallback.input("application/octet-stream",std::as_bytes(std::span(payload))); });
    }
    CHECK(memory.used()==0);
    {
        auto old_store=SessionStore::open(root,id(1),memory);
        SessionRuntime old(old_store,memory,8192);
        CHECK(old.phase()==SessionPhase::published);
        CHECK(old.find(id(10))->state().strength()==saved_strength);
        CHECK(old.replay(id(10),0).location()==first);
        auto live_store=SessionStore::open(root,id(2),memory);
        SessionRuntime live(live_store,memory,8192);
        ExperienceRouter router(live,memory); router.mount_main(old);
        CHECK(router.recall(id(10)).temporary());
        CHECK(router.input("application/octet-stream",std::as_bytes(std::span(payload))).matches().size()==2);
        const std::string utterance="스웨카가 셔플값을 검증한다.";
        CHECK(router.input("text/plain",std::as_bytes(std::span(utterance))).familiar());
        auto result=observe(live,"live",10,3,EvidenceOutcome::support);
        CHECK(live.find(id(10))->head()!=ExperienceLocation{});
        CHECK(result.refinement.after_revision()>result.refinement.before_revision());
    }
    CHECK(memory.used()==0);
    fs::remove_all(root);
    std::printf("session runtime tests: %u checks passed\n",checks);
}
