#include "vrs/synapse.hpp"
#include "swegca_architecture/content_observation_kernel.hpp"
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();} } while(false)
static bool fail_write=false;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t offset) {
    if(fail_write){fail_write=false;errno=ENOSPC;return -1;}
    return __real_pwrite(fd,p,n,offset);
}
template<class F> void rejects(F f) {bool caught=false;try{f();}catch(const std::exception&){caught=true;}CHECK(caught);}
DigestBytes id(unsigned n){DigestBytes d{};d[0]=std::byte(n);return d;}
std::span<const std::byte> bytes(std::string_view s){return {reinterpret_cast<const std::byte*>(s.data()),s.size()};}
ExperienceLocation raw(SessionStore& s,std::string_view text){
    return s.append({s.original_count(),0,s.name(),"synthetic-test","text/plain",bytes(text)});
}
// Synthetic, explicitly multi-axis fixtures test wiring, not visual semantics
// or independent real observations. No test policy is used on the image store.
void observations(SessionStore& store,Synapse& synapse,bool complete,bool equal) {
    for(unsigned axis=0;axis<4;++axis)for(unsigned group=0;group<12;++group){
        EvidenceObservation o;
        o.hypothesis=synapse.identity();o.source=id(group+1);o.context=id(group+31);
        o.producer=id(group+61);o.axis=axis;
        o.outcome=to_outcome(observe_content_relation(complete,true,equal,true));
        (void)synapse.observe({store.original_count(),0,store.name(),"synthetic-test","text/plain",
            bytes("synthetic relation observation")},o);
    }
}
int main(){
    auto name=(std::filesystem::temp_directory_path()/"swegca-synapse-XXXXXX").string();
    CHECK(::mkdtemp(name.data())!=nullptr);
    MemoryBudget memory(32<<20);EvidencePolicy policy;
    std::array<SynapseCheckpoint,3> heads;
    std::array<double,3> strengths{1.01,.995,1.0};
    std::array<ExperienceLocation,3> members;
    ExperienceLocation definition;
    {
        auto store=SessionStore::create(name,id(200),"synapse-test",1<<20,memory);
        members={raw(store,"image fixture"),raw(store,"tag fixture"),raw(store,"feature fixture")};
        definition=raw(store,"fixture relation with explicit measurement criterion");
        auto before=store.original_count();
        rejects([&]{(void)Synapse::create(store,std::span(members).first(1),definition,1,policy,memory,65536);});
        std::array duplicates{members[0],members[0]};
        rejects([&]{(void)Synapse::create(store,duplicates,definition,1,policy,memory,65536);});
        auto bad=members;bad[0].digest[0]^=std::byte{1};
        rejects([&]{(void)Synapse::create(store,bad,definition,1,policy,memory,65536);});
        // Payload alone fits, but the sealed record framing does not.
        rejects([&]{(void)Synapse::create(store,members,definition,1,policy,memory,336);});
        CHECK(store.original_count()==before);
        for(unsigned i=0;i<3;++i){
            auto s=Synapse::create(store,members,definition,1,policy,memory,65536);
            CHECK(s.members().size()==3&&s.members()[0]==members[0]);
            EvidenceObservation wrong;wrong.hypothesis=id(250);
            auto count=store.original_count();
            rejects([&]{(void)s.observe({0,0,store.name(),"fixture","text/plain",bytes("bad")},wrong);});
            CHECK(store.original_count()==count);
            observations(store,s,i!=2,i==0);
            auto report=s.refine(12345,0);
            CHECK(report.samples().size()==48);
            CHECK(report.result().verification().judgment().status()==
                (i==0?EvidenceStatus::accept:i==1?EvidenceStatus::reject:EvidenceStatus::abstain));
            CHECK(s.strength()==strengths[i]);
            CHECK(report.connection()==s.identity());
            heads[i]=s.checkpoint();
        }
        // Changing a member changes the binding, so another state cannot be
        // spliced under it even though both records have valid outer checksums.
        auto swapped=members;std::swap(swapped[0],swapped[1]);
        auto other=Synapse::create(store,swapped,definition,1,policy,memory,65536);
        rejects([&]{(void)Synapse::recover(store,{other.checkpoint().binding,heads[0].state},memory,65536);});
    }
    CHECK(memory.used()==0);
    {
        auto store=SessionStore::open(name,id(200),memory);
        for(unsigned i=0;i<3;++i){
            auto s=Synapse::recover(store,heads[i],memory,65536);
            CHECK(s.definition()==definition&&s.members()[2]==members[2]);
            CHECK(s.strength()==strengths[i]);
            const auto report=s.refine(42,0);
            // Same original evidence count, never 96 new observations.
            CHECK(report.samples().size()==48);
            CHECK(s.strength()==strengths[i]*strengths[i]);
        }
        auto s=Synapse::recover(store,heads[0],memory,65536);
        const auto checkpoint=s.checkpoint();const auto revision=s.revision();
        fail_write=true;
        rejects([&]{(void)s.refine(77,0);});
        CHECK(s.checkpoint().state==checkpoint.state);
        CHECK(s.revision()==revision&&s.strength()==strengths[0]);
    }
    CHECK(memory.used()==0);
    {
        auto store=SessionStore::open(name,id(200),memory);
        auto s=Synapse::recover(store,heads[0],memory,65536);
        CHECK(s.strength()==1.01);
        CHECK(s.members()[0]==members[0]);
        store.end();
        rejects([&]{(void)s.refine(91,0);});
        CHECK(s.strength()==1.01);
    }
    CHECK(memory.used()==0);
    std::filesystem::remove_all(name);
    std::printf("synapse tests: %u checks passed (synthetic wiring/persistence, not image semantics)\n",checks);
}
