#include "swegca_architecture/association_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
Digest id(unsigned n){Digest d{};d[0]=std::byte(n);return d;}
[[gnu::noinline]] double operation(std::uint64_t i){
    return revise_association_strength(1.0,judge_association({i&1,(i>>1)&1})).current();
}
int main(){
    static_assert(!std::is_convertible_v<AssociationJudgment,EvidenceJudgment>);
    const auto binding=bind_experience(id(1),id(1),id(2),id(3));
    CHECK(binding.valid());
    CHECK(!bind_experience(id(1),id(9),id(2),id(3)).valid());
    CHECK(!bind_experience(id(1),id(1),Digest{},id(3)).valid());
    const std::array<std::uint32_t,3> tags{4,9,12};
    CHECK(observe_tag_association(binding,tags,4,9)==EvidenceOutcome::support);
    CHECK(observe_tag_association(binding,tags,9,4)==EvidenceOutcome::support);
    CHECK(observe_tag_association(binding,tags,4,99)==EvidenceOutcome::insufficient);
    CHECK(observe_tag_association(binding,tags,98,99)==EvidenceOutcome::insufficient);
    CHECK(observe_tag_association(binding,tags,4,4)==EvidenceOutcome::insufficient);
    CHECK(observe_tag_association({},tags,4,9)==EvidenceOutcome::insufficient);
    const std::array<std::uint32_t,2> other{9,88};
    CHECK(observe_common_member(binding,tags,binding,other,9)==EvidenceOutcome::support);
    CHECK(observe_common_member(binding,tags,binding,other,4)==EvidenceOutcome::insufficient);
    CHECK(observe_common_member(binding,tags,{},other,9)==EvidenceOutcome::insufficient);
    CHECK(observe_recorded_member(binding,other,9)==EvidenceOutcome::support);
    CHECK(observe_recorded_member(binding,other,12)==EvidenceOutcome::insufficient);
    auto yes=judge_association({1,0}),no=judge_association({0,1}),unknown=judge_association({}),conflict=judge_association({1,1});
    CHECK(yes.status()==EvidenceStatus::accept);
    CHECK(no.status()==EvidenceStatus::reject);
    CHECK(unknown.status()==EvidenceStatus::abstain);
    CHECK(conflict.status()==EvidenceStatus::abstain&&conflict.reason()==AssociationReason::conflicting_evidence);
    CHECK(revise_association_strength(1,yes).current()==1.01);
    CHECK(revise_association_strength(1,no).current()==.995);
    CHECK(revise_association_strength(1,unknown).current()==1);
    CHECK(revise_association_strength(1,conflict).current()==1);
    CHECK(!revise_association_strength(std::numeric_limits<double>::infinity(),yes).valid());
    CHECK(!revise_association_strength(std::numeric_limits<double>::max(),yes).valid());
    // Recorded association does not bypass the unchanged causal gate.
    EvidenceTally tally;tally.axis_support[0]=6185;tally.axis_refute[0]=73;tally.revision=6258;
    tally.source_diversity=tally.context_diversity=tally.axis_source_diversity[0]=1;
    const auto causal=judge_evidence(make_evidence_rules(EvidencePolicy{}),tally);
    CHECK(causal.status()==EvidenceStatus::abstain&&causal.reason()==EvidenceReason::minimum_effective_samples);
    const auto begin=std::chrono::steady_clock::now();double sum=0;constexpr unsigned n=2000000;
    for(unsigned i=0;i<n;++i)sum+=operation(i);
    const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/n;
    std::printf("association tests: %u checks passed; fixed judgment+strength %.3f ns/op; checksum %.3f\n",checks,ns,sum);
}
