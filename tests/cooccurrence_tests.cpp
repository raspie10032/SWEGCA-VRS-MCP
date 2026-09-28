#include "swegca_architecture/cooccurrence_kernel.hpp"
#include "swegca_architecture/association_kernel.hpp"
#include <array>
#include <cassert>
#include <iostream>
using namespace swegca::architecture::kernel;
int main(){
 std::array<std::uint64_t,1> known{UINT64_MAX},a{3},b{6},c{8},unknown{UINT64_MAX^1ULL},zero{0};
 auto yes=observe_cooccurrence({known,a},{known,b});assert(yes.witnesses==1&&to_outcome(yes.predicate)==EvidenceOutcome::support);
 auto no=observe_cooccurrence({known,a},{known,c});assert(no.witnesses==0&&to_outcome(no.predicate)==EvidenceOutcome::refute);
 auto missing=observe_cooccurrence({unknown,zero},{known,a});assert(to_outcome(missing.predicate)==EvidenceOutcome::insufficient);
 auto irrelevant=observe_cooccurrence({unknown,zero},{known,c});assert(to_outcome(irrelevant.predicate)==EvidenceOutcome::refute);
 auto corrupt=observe_cooccurrence({zero,a},{known,b});assert(!corrupt.predicate.valid);
 assert(!observe_cooccurrence({},{}).predicate.valid);
 auto absent=judge_association({0,1});auto strength=revise_association_strength(1,absent);
 assert(strength.current()==.995&&!strength.evidence_eligible());
 assert(revise_association_strength(strength.current(),judge_association({1,0})).evidence_eligible());
 std::cout<<"PASS cooccurrence: exists, absent, invalid, irrelevant unknown, malformed, retention and recovery\n";
}
