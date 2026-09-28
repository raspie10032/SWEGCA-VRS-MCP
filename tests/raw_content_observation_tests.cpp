#include "swegca_architecture/content_observation_kernel.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace swegca::architecture::kernel;
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)

int main() {
    std::array<std::byte,256> left{},right{};
    for(unsigned i=0;i<256;++i)left[i]=right[i]=std::byte(i);
    const auto original=left;
    CHECK(to_outcome(observe_raw_content_relation(left,right,true,true,true))==EvidenceOutcome::support);
    CHECK(to_outcome(observe_raw_content_relation(left,right,true,true,false))==EvidenceOutcome::refute);
    for(unsigned i=0;i<256;++i){
        right[i]^=std::byte{1};
        CHECK(to_outcome(observe_raw_content_relation(left,right,true,true,true))==EvidenceOutcome::refute);
        CHECK(to_outcome(observe_raw_content_relation(left,right,true,true,false))==EvidenceOutcome::support);
        right[i]^=std::byte{1};
    }
    CHECK(left==original&&right==original);
    CHECK(to_outcome(observe_raw_content_relation(left,std::span(right).first(255),true,true,true))==EvidenceOutcome::refute);
    for(bool complete:{false,true})for(bool stable:{false,true}){
        if(complete&&stable)continue;
        for(bool expect:{false,true})
            CHECK(to_outcome(observe_raw_content_relation(left,right,complete,stable,expect))==EvidenceOutcome::insufficient);
    }
    CHECK(to_outcome(observe_raw_content_relation({}, {},true,true,true))==EvidenceOutcome::support);
    CHECK(to_outcome(observe_raw_content_relation({}, {},false,true,true))==EvidenceOutcome::insufficient);
    std::printf("raw content primitive: %u checks passed; exact-byte predicate only\n",checks);
}
