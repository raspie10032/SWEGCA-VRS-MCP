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
static_assert(!std::is_copy_constructible_v<ExperienceRouter>);
static_assert(!std::is_move_constructible_v<ExperienceRouter>);
static_assert(familiarity_key(true,true,true)==FamiliarityKey::exact);
static_assert(familiarity_key(false,true,true)==FamiliarityKey::continuation);
static_assert(familiarity_key(false,false,true)==FamiliarityKey::context);
static_assert(familiarity_key(false,false,false)==FamiliarityKey::missing);
static_assert(familiarity_key(false,true,true,true)==FamiliarityKey::context);
static_assert(familiarity_key(true,true,true,true)==FamiliarityKey::exact);
static_assert(!requires_re_evidence(ReplayAgreement::invalid));
static_assert(!requires_re_evidence(ReplayAgreement::insufficient));
static_assert(!requires_re_evidence(ReplayAgreement::agrees));
static_assert(requires_re_evidence(ReplayAgreement::contradicts));
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
class AllocationTraffic final : public std::pmr::memory_resource {
public:
    std::size_t bytes=0,calls=0;
    bool fail=false;
private:
    void* do_allocate(std::size_t n,std::size_t alignment) override {
        if(fail)throw std::bad_alloc();
        auto* result=std::pmr::new_delete_resource()->allocate(n,alignment);
        bytes+=n;++calls;return result;
    }
    void do_deallocate(void* p,std::size_t n,std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(p,n,alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {return this==&other;}
};
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
    {
        // An explicitly keyed observation in another recorded context is not
        // evidence that this session's lifecycle-only context can seed dialogue.
        const auto verify=[&](SessionRuntime& live){
            ExperienceRouter route(live,memory);
            const auto before_reads=reads,before_writes=writes;
            auto recalled=route.input("text/plain",std::as_bytes(std::span(followup)));
            CHECK(recalled.key_kind()==FamiliarityKey::missing);
            CHECK(!recalled.temporary()&&!recalled.familiar());
            CHECK(reads==before_reads&&writes==before_writes);
        };
        {
            auto store=SessionStore::create(root,id(240),"seed-boundary",65536,memory);
            SessionRuntime live(store,memory,8192);
            (void)live.retain_input(input("seed-boundary",0),1.0,policy,0,0);
            live.define_connection(id(241),1.0,policy);
            EvidenceObservation value;value.hypothesis=id(241);value.source=id(242);
            value.context=id(243);value.producer=id(244);value.observed_at=1;
            (void)live.observe(id(241),input("seed-boundary",1),value,1,1,id(245));
            verify(live);
        }
        {
            auto store=SessionStore::open(root,id(240),memory);
            SessionRuntime live(store,memory,8192);verify(live);
        }
    }
    CHECK(memory.used()==0);
    {
        AllocationTraffic traffic;MemoryBudget budget(32<<20,&traffic);
        {
            auto store=SessionStore::create(root,id(246),"index-growth",1048576,budget);
            SessionRuntime live(store,budget,8192);live.define_connection(id(247),1.0,policy);
            EvidenceObservation value;value.hypothesis=id(247);value.source=id(248);
            value.context=id(249);value.producer=id(250);
            std::vector<ExperienceLocation> originals;
            for(unsigned n=0;n<128;++n){
                value.observed_at=n;
                originals.push_back(live.observe(id(247),input("index-growth",n),value,7,n).original);
            }
            const auto before_writes=writes;
            traffic.fail=true;value.observed_at=128;
            throws<std::bad_alloc>([&]{(void)live.observe(id(247),input("index-growth",128),value,7,128);});
            traffic.fail=false;
            CHECK(live.usable()&&writes==before_writes);
            CHECK(live.find(id(247))->state().experiences().size()==128);
            for(unsigned n=128;n<193;++n){
                value.observed_at=n;
                originals.push_back(live.observe(id(247),input("index-growth",n),value,7,n).original);
            }
            std::printf("193-record index fixture allocation traffic: %zu bytes / %zu calls\n",traffic.bytes,traffic.calls);
            ExperienceRouter route(live,budget);
            const auto before_reads=reads;
            auto recalled=route.input("application/octet-stream",std::as_bytes(std::span(payload)));
            CHECK(reads==before_reads&&recalled.matches().size()==originals.size());
            for(std::size_t n=0;n<originals.size();++n)CHECK(recalled.matches()[n].original==originals[n]);
        }
        CHECK(budget.used()==0);
    }
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
        auto missing=router.input("text/plain",std::as_bytes(std::span(followup)));
        CHECK(!missing.familiar());CHECK(!router.select_replay(missing));
        const auto reads_before=reads, writes_before=writes;
        const auto recall_memory_before=memory.used();
        auto natural=router.input("application/octet-stream",std::as_bytes(std::span(payload)));
        CHECK(reads==reads_before && writes==writes_before);
        CHECK(natural.familiar() && !natural.temporary() && natural.matches().size()==16);
        const auto recall_bytes=memory.used()-recall_memory_before;
        CHECK(recall_bytes<16*sizeof(InputMatch));
        std::printf("16-candidate receipt: %zu tracked bytes; expanded matches: %zu bytes\n",recall_bytes,16*sizeof(InputMatch));
        std::size_t match_index=0;
        for(const auto match:natural.matches()){
            CHECK(match.original==old.find(id(10))->state().experiences()[match_index].original());
            CHECK(match.original_index==match_index++);
            CHECK(match.recalled.recalled_head.strength==saved_strength);
            CHECK(match.current_observations==0);
        }
        CHECK(match_index==16);
        CHECK(router.select_replay(natural)==15);
        CHECK(reads==reads_before && writes==writes_before);
        CHECK(memory.used()==recall_memory_before+recall_bytes);
        throws<std::out_of_range>([&]{(void)natural.matches()[16];});
        CHECK(router.replay(natural,0).location()==first);
        CHECK(reads>reads_before && writes==writes_before);
        CHECK(router.input("text/plain",std::as_bytes(std::span(payload))).key_kind()==FamiliarityKey::continuation);
        const auto context_reads=reads, context_writes=writes;
        const auto continued_before=memory.used();
        auto continued=router.input("text/plain",std::as_bytes(std::span(followup)));
        const auto continued_bytes=memory.used()-continued_before;
        CHECK(continued_bytes<16*sizeof(ExperienceLocation));
        std::printf("16-candidate continued receipt: %zu tracked bytes\n",continued_bytes);
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
        {
            std::optional<ExperienceRouter> reused;reused.emplace(live,memory);
            const auto* address=&*reused;
            auto old_receipt=reused->input("application/octet-stream",std::as_bytes(std::span(payload)));
            auto old_replay=reused->replay(old_receipt,0);
            reused.reset();reused.emplace(live,memory);
            CHECK(&*reused==address);
            const auto before_reads=reads,before_writes=writes;
            throws<std::invalid_argument>([&]{(void)reused->replay(old_receipt,0);});
            throws<std::invalid_argument>([&]{(void)reused->compare_replay(old_replay,7,0);});
            CHECK(reads==before_reads&&writes==before_writes);
            auto current=reused->input("application/octet-stream",std::as_bytes(std::span(payload)));
            CHECK(reused->replay(current,0).location()==local.original);
        }

        (void)observe(live,"live",10,2,EvidenceOutcome::support);
        throws<std::logic_error>([&] { (void)router.replay(preferred,0,0); });
        CHECK(router.replay(natural_local,0).location()==local.original);
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
        auto joined=fallback.input("application/octet-stream",std::as_bytes(std::span(payload)));
        auto moved=std::move(joined);
        CHECK(moved.matches().size()==32);
        for(std::size_t index=0;index<32;++index){
            const auto match=moved.matches()[index];
            const auto& owner=index<16?old:other;
            CHECK(match.recalled.session==&owner);
            CHECK(match.recalled.recalled_head.strength==owner.find(id(10))->state().strength());
            CHECK(match.original==owner.find(id(10))->state().experiences()[index%16].original());
            CHECK(fallback.replay(moved,index).location()==match.original);
        }
        auto ranged=fallback.input("text/plain",std::as_bytes(std::span(followup)));
        CHECK(ranged.key_kind()==FamiliarityKey::continuation && ranged.matches().size()==32);
        for(std::size_t index=0;index<32;++index){
            CHECK(ranged.matches()[index].original==moved.matches()[index].original);
            CHECK(ranged.matches()[index].recalled.session==moved.matches()[index].recalled.session);
        }
        auto ranged_moved=std::move(ranged);CHECK(!ranged.familiar()&&ranged.matches().empty());
        CHECK(fallback.replay(ranged_moved,31).location()==moved.matches()[31].original);
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
        CHECK(evidence_payload(selected.original()).content.size()==message.content.size());
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
    {
        auto archive_store=SessionStore::open(root,id(1),memory);
        SessionRuntime archive(archive_store,memory,8192);
        const auto remembered_head=archive.find(id(10))->head();
        for (unsigned scenario=0;scenario<4;++scenario) {
            auto store=SessionStore::create(root,id(50+scenario),"current",65536,memory);
            SessionRuntime current(store,memory,8192);
            ExperienceRouter router(current,memory); router.mount_main(archive);
            auto activation=router.input("application/octet-stream",std::as_bytes(std::span(payload)));
            auto played=router.replay(activation,0);
            CHECK(played.input_cue()==activation.cue());
            auto no_new_comparison=router.compare_replay(played,1,100);
            const auto& no_new=no_new_comparison.evidence();
            CHECK(no_new.agreement()==ReplayAgreement::insufficient);
            CHECK(no_new.current_originals().empty());
            throws<std::invalid_argument>([&]{(void)router.re_evidence(played,no_new_comparison,1,100);});
            current.define_connection(id(10),0.75,policy);
            for (unsigned n=1;n<=16;++n) {
                EvidenceObservation value;
                value.hypothesis=id(10);value.source=id(n+120);value.context=id(n+150);value.producer=id(n+190);
                value.observed_at=n+100;
                value.outcome=scenario==1?EvidenceOutcome::refute:scenario==2?EvidenceOutcome::insufficient:EvidenceOutcome::support;
                value.has_expiry=scenario==3;value.expires_at=n+101;
                (void)current.observe(id(10),input("current",n+100),value,n,1000);
            }
            const auto current_head=current.find(id(10))->head();
            const auto current_strength=current.find(id(10))->state().strength();
            const auto read_count=reads,write_count=writes;
            auto compared=router.compare_replay(played,91,1000);
            CHECK(compared.agreement()==(scenario==0?ReplayAgreement::agrees:scenario==1?ReplayAgreement::contradicts:ReplayAgreement::insufficient));
            CHECK(compared.evidence().current_originals().size()==16);
            if(scenario==1){
                auto verified=router.re_evidence(played,compared,92,1000);
                CHECK(verified.agreement()==ReplayAgreement::contradicts);
                CHECK(verified.current_originals().size()==16);
                throws<std::invalid_argument>([&]{(void)router.re_evidence(played,compared,92,999);});
            }else{
                throws<std::invalid_argument>([&]{(void)router.re_evidence(played,compared,92,1000);});
            }
            const auto& checked=compared.evidence();
            CHECK(reads==read_count && writes==write_count);
            CHECK(checked.current_originals().size()==16);
            CHECK(checked.remembered_head().record==remembered_head);
            CHECK(checked.replayed_original()==first);
            CHECK(checked.input_cue()==activation.cue());
            CHECK(checked.current_head().record==current_head);
            CHECK(current.find(id(10))->head()==current_head && current.find(id(10))->state().strength()==current_strength);
            CHECK(archive.find(id(10))->head()==remembered_head);
            CHECK(checked.agreement()==(scenario==0?ReplayAgreement::agrees:scenario==1?ReplayAgreement::contradicts:ReplayAgreement::insufficient));
            CHECK(checked.verification().result().verification().judgment().status()==
                (scenario==0?EvidenceStatus::accept:scenario==1?EvidenceStatus::reject:EvidenceStatus::abstain));
            for (const auto& address:checked.current_originals()) CHECK(address!=first);
            if(scenario==1){
                // Re-evidence must consume observations arriving after the
                // comparison, rather than return the old comparison report.
                (void)observe(current,"current",10,50,EvidenceOutcome::insufficient);
                const auto head=current.find(id(10))->head();
                auto fresh=router.re_evidence(played,compared,94,1000);
                CHECK(fresh.current_originals().size()==17);
                CHECK(fresh.current_head().record==head);
                CHECK(compared.evidence().current_originals().size()==16);
                CHECK(fresh.agreement()==ReplayAgreement::contradicts);
            }
            // The next Recall sets a new boundary. The already considered
            // observations must not become new evidence for that next request.
            auto next=router.input("application/octet-stream",std::as_bytes(std::span(payload)));
            auto next_replay=router.replay(next,0);
            auto next_comparison=router.compare_replay(next_replay,92,1000);
            const auto& already_seen=next_comparison.evidence();
            CHECK(already_seen.current_originals().empty());
            CHECK(already_seen.agreement()==ReplayAgreement::insufficient);
            throws<std::invalid_argument>([&]{(void)router.re_evidence(next_replay,compared,93,1000);});
            ExperienceRouter other(current,memory);
            throws<std::invalid_argument>([&]{(void)other.compare_replay(played,93,1000);});
            throws<std::invalid_argument>([&]{(void)other.re_evidence(played,compared,93,1000);});
            throws<std::invalid_argument>([&] { (void)other.replay(next,0); });
            throws<std::invalid_argument>([&] { (void)other.compare_replay(next_replay,93,100); });
        }
    }
    CHECK(memory.used()==0);
    {
        auto prior_store=SessionStore::create(root,id(120),"context-prior",65536,memory);
        SessionRuntime prior(prior_store,memory,8192);prior.define_connection(id(121),1,policy);
        const std::string remembered="remembered",unseen="follow-on",nearby="related",exact="exact";
        const auto record=[&](SessionRuntime& target,unsigned connection,unsigned context,std::string_view text,std::uint64_t sequence){
            EvidenceObservation value;value.hypothesis=id(connection);value.context=id(context);
            value.source=id(200);value.producer=id(201);
            const std::string_view session=connection==121?"context-prior":"context-live";
            return target.observe(id(connection),{sequence,0,session,"experiment","text/plain",std::as_bytes(std::span(text))},value,7,0).original;
        };
        const auto old=record(prior,121,123,remembered,0);prior.end();prior.publish_originals();
        ExperienceLocation related,other_related;
        {
            auto store=SessionStore::create(root,id(122),"context-live",65536,memory);
            SessionRuntime active(store,memory,8192);
            active.define_connection(id(125),1,policy);active.define_connection(id(124),1,policy);
            active.define_connection(id(126),1,policy);
            // Insert opposite to connection order; recovery must retain order.
            other_related=record(active,125,123,nearby,0);
            related=record(active,124,123,nearby,0);
            (void)record(active,126,127,"unrelated",0);
            ExperienceRouter route(active,memory);route.mount_main(prior);
            auto first=route.input("text/plain",std::as_bytes(std::span(remembered)));
            CHECK(!first.temporary()&&first.matches()[0].original==old);
            // Partial reads and failed Replay cannot establish a context key.
            (void)route.read_payload_slice(first,0,0,1);
            CHECK(!route.input("text/plain",std::as_bytes(std::span(unseen))).familiar());
            fail_read=true;throws<std::system_error>([&]{(void)route.replay(first,0);});
            CHECK(!route.input("text/plain",std::as_bytes(std::span(unseen))).familiar());
            (void)route.replay(first,0);
            const auto before_reads=reads,before_writes=writes;
            auto continued=route.input("text/plain",std::as_bytes(std::span(unseen)));
            CHECK(continued.temporary()&&continued.key_kind()==FamiliarityKey::context&&continued.matches().size()==2);
            CHECK(continued.matches()[0].original==related&&continued.matches()[1].original==other_related);
            CHECK(reads==before_reads&&writes==before_writes);
            const auto strength=active.find(id(124))->state().strength();
            CHECK(route.replay(continued,0).location()==related);
            CHECK(active.find(id(124))->state().strength()==strength);
            const auto direct=record(active,126,127,exact,1);
            auto current=route.input("text/plain",std::as_bytes(std::span(exact)));
            CHECK(current.key_kind()==FamiliarityKey::exact&&current.matches()[0].original==direct);
        }
        {
            auto store=SessionStore::open(root,id(122),memory);SessionRuntime active(store,memory,8192);
            ExperienceRouter route(active,memory);route.mount_main(prior);
            auto first=route.input("text/plain",std::as_bytes(std::span(remembered)));(void)route.replay(first,0);
            auto restored=route.input("text/plain",std::as_bytes(std::span(unseen)));
            CHECK(restored.key_kind()==FamiliarityKey::context&&restored.matches().size()==2);
            CHECK(restored.matches()[0].original==related&&restored.matches()[1].original==other_related);
        }
    }
    CHECK(memory.used()==0);
    {
        auto store=SessionStore::create(root,id(220),"ranges",262144,memory);
        SessionRuntime live(store,memory,8192);live.define_connection(id(221),1.0,policy);
        ExperienceRouter route(live,memory);
        const std::string other="different cue inside the same connection";
        EvidenceObservation observation;observation.hypothesis=id(221);observation.source=id(11);
        observation.producer=id(12);observation.context=id(13);observation.outcome=EvidenceOutcome::insufficient;
        std::vector<ExperienceLocation> expected;
        const auto record=[&](unsigned n,std::string_view text){
            observation.observed_at=n;
            return live.observe(id(221),{n,n,"ranges","fixture","text/plain",std::as_bytes(std::span(text))},observation,n,n).original;
        };
        for(unsigned n=0;n<193;++n){
            const auto location=record(n,n==64?std::string_view(other):std::string_view(payload));
            if(n!=64)expected.push_back(location);
        }
        const auto used=memory.used(),before_reads=reads,before_writes=writes;
        auto receipt=route.input("text/plain",std::as_bytes(std::span(payload)));
        const auto cost=memory.used()-used;
        CHECK(route.select_replay(receipt)==191);
        CHECK(cost<2048);CHECK(reads==before_reads&&writes==before_writes);
        CHECK(receipt.temporary()&&receipt.matches().size()==192);
        for(std::size_t n=0;n<expected.size();++n){
            const auto match=receipt.matches()[n];
            CHECK(match.original==expected[n]);CHECK(match.original_index==(n<64?n:n+1));
            CHECK(match.current_observations==193);
        }
        auto moved=std::move(receipt);CHECK(receipt.matches().empty());
        const auto next=record(193,payload);
        CHECK(route.select_replay(moved)==191);
        throws<std::invalid_argument>([&]{(void)route.select_replay(receipt);});
        CHECK(moved.matches().size()==192&&moved.matches()[191].original==expected.back());
        auto replayed=route.replay(moved,191);CHECK(replayed.location()==expected.back());
        const auto comparison=route.compare_replay(replayed,7,193);
        const auto& assessment=comparison.evidence();
        CHECK(assessment.agreement()==ReplayAgreement::insufficient);
        CHECK(assessment.current_originals().size()==1&&assessment.current_originals()[0]==next);
        std::printf("192-candidate temporary receipt: %zu tracked bytes\n",cost);
    }
    CHECK(memory.used()==0);
    {
        // Outcome status cannot hide a negative experience from Replay.
        ExperienceLocation expected;
        {
            auto store=SessionStore::create(root,id(230),"selection",65536,memory);
            SessionRuntime live(store,memory,8192);ExperienceRouter route(live,memory);
            for(unsigned key=231;key<=233;++key)live.define_connection(id(key),key==231?1.0:2.0,policy);
            const auto record=[&](unsigned key,unsigned step,EvidenceOutcome outcome){
                EvidenceObservation value;value.hypothesis=id(key);value.source=id(1);
                value.context=id(2);value.producer=id(3);value.observed_at=step;value.outcome=outcome;
                return live.observe(id(key),input("selection",step),value,7,step).original;
            };
            (void)record(231,100,EvidenceOutcome::support);
            expected=record(232,90,EvidenceOutcome::refute);
            (void)record(233,80,EvidenceOutcome::insufficient);
            auto receipt=route.input("application/octet-stream",std::as_bytes(std::span(payload)));
            const auto before_reads=reads,before_writes=writes,used=memory.used();
            CHECK(route.select_replay(receipt)==1);
            CHECK(reads==before_reads&&writes==before_writes&&memory.used()==used);
            CHECK(route.replay(receipt,*route.select_replay(receipt)).location()==expected);
            (void)record(233,90,EvidenceOutcome::insufficient);
            auto tied=route.input("application/octet-stream",std::as_bytes(std::span(payload)));
            CHECK(tied.matches()[*route.select_replay(tied)].original==expected);
            // Mix individual address pins with a longer range in another
            // connection. Selection must retain global receipt indices.
            const auto old_expected=expected;
            for(unsigned step=100;step<140;++step)expected=record(233,step,EvidenceOutcome::insufficient);
            auto mixed=route.input("application/octet-stream",std::as_bytes(std::span(payload)));
            CHECK(mixed.matches().size()==44);
            const auto selection_reads=reads,selection_writes=writes,selection_memory=memory.used();
            CHECK(mixed.matches()[*route.select_replay(mixed)].original==expected);
            CHECK(tied.matches()[*route.select_replay(tied)].original==old_expected);
            CHECK(reads==selection_reads&&writes==selection_writes&&memory.used()==selection_memory);
        }
        {
            auto store=SessionStore::open(root,id(230),memory);
            SessionRuntime live(store,memory,8192);ExperienceRouter route(live,memory);
            auto restored=route.input("application/octet-stream",std::as_bytes(std::span(payload)));
            CHECK(restored.matches()[*route.select_replay(restored)].original==expected);
        }
    }
    CHECK(memory.used()==0);
    fs::remove_all(root);
    std::printf("session runtime tests: %u checks passed\n",checks);
}
