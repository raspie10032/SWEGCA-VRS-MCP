#include "vrs/session_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs = std::filesystem;
static unsigned checks = 0;
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
    ExperienceLocation first;
    double saved_strength=0;
    {
        auto old_store=SessionStore::create(root,id(1),"old",65536,memory);
        SessionRuntime old(old_store,memory,8192);
        old.define_connection(id(10),1.0,policy);
        CHECK(old.find(id(10))==nullptr); // Unobserved definitions are not recall candidates.
        auto raw=old.record(input("old",0));
        CHECK(old_store.read(raw,8192).view().content.size()==payload.size());
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
        // Temporary write failure must not become a false miss and Main fallback.
        blank.define_connection(id(10),1.0,policy);
        std::string huge(100000,'x'); auto oversized=input("blank",1);
        oversized.content=std::as_bytes(std::span(huge));
        throws<std::length_error>([&] { (void)blank.record(oversized); });
        CHECK(!blank.usable());
        throws<std::logic_error>([&] { (void)fallback.recall(id(10)); });
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
        auto result=observe(live,"live",10,2,EvidenceOutcome::support);
        CHECK(live.find(id(10))->head()!=ExperienceLocation{});
        CHECK(result.refinement.after_revision()>result.refinement.before_revision());
    }
    CHECK(memory.used()==0);
    fs::remove_all(root);
    std::printf("session runtime tests: %u checks passed\n",checks);
}
