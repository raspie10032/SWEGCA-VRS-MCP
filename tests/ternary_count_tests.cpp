#include "swegca_architecture/ternary_count_kernel.hpp"
#include <cassert>
#include <iostream>
using namespace swegca::architecture::kernel;
int main(){
 assert(count_evidence_eligible({0,0,0}));assert(count_evidence_eligible({3,3,99}));assert(!count_evidence_eligible({2,3,0}));
 TernaryCount c;assert(accumulate_verdict(c,EvidenceStatus::accept));assert(accumulate_verdict(c,EvidenceStatus::reject));assert(accumulate_verdict(c,EvidenceStatus::abstain));assert((c==TernaryCount{1,1,1}));
 for(unsigned i=0;i<100;++i){assert(accumulate_verdict(c,EvidenceStatus::accept));}
 assert((c==TernaryCount{101,1,1}));
 c.accept=UINT64_MAX;const auto old=c;assert(!accumulate_verdict(c,EvidenceStatus::accept)&&c==old);assert(!accumulate_verdict(c,static_cast<EvidenceStatus>(77))&&c==old);
 std::cout<<"PASS: exact ternary accumulation, independent abstention, atomic overflow/invalid rejection\n";
}
