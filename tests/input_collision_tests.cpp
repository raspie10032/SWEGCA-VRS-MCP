#include "vrs/input_collision.hpp"
#include <cassert>
#include <iostream>
using namespace swegca::vrs;using namespace swegca::architecture::kernel;
Digest id(unsigned x){Digest d{};d[0]=std::byte(x);return d;}
int main(){
 auto binding=bind_experience(id(1),id(1),id(2),id(3));
 // Stored active claims disagree with dirty input. They must be challenged,
 // not treated as labels or used to reject this input.
 std::vector<TernaryCount> ti{{1,0,0},{1,0,0},{0,1,0}},tt{{1,0,0},{0,1,0},{0,1,0}};InputCollision engine(1,3,ti,tt);
 CollisionInput raw{binding,{0,2,2},true};std::mt19937_64 rng(19);unsigned reject=0,restored=0,seen=0;
 engine.encounter(0,raw,rng,[&](const CollisionEvent& e){++seen;reject+=e.status==EvidenceStatus::reject;restored+=!count_evidence_eligible(e.previous)&&count_evidence_eligible(e.current);});
 assert(seen==5&&reject==2&&restored==2);assert((ti[1]==TernaryCount{1,1,0}&&ti[2]==TernaryCount{1,1,0}));assert((tt[0]==TernaryCount{1,1,0}&&tt[1]==TernaryCount{1,1,0}));
 const auto old=ti[0];engine.encounter(0,raw,rng,[](const auto&){});assert(ti[0].accept==old.accept+1&&ti[0].reject==old.reject);
 // Invalid observation still reaches the core and abstains without deleting state.
 const auto before=ti;engine.encounter(0,CollisionInput{{},{0},true},rng,[](const auto& e){assert(e.status==EvidenceStatus::abstain&&e.current.accept==e.previous.accept&&e.current.reject==e.previous.reject&&e.current.abstain==e.previous.abstain+1);});for(unsigned i=0;i<3;++i)assert(ti[i].abstain==before[i].abstain+1);
 const auto missing_ti=ti,missing_tt=tt;unsigned missing_events=0;
 assert(engine.encounter(0,CollisionInput{binding,{}},rng,[&](const auto&){++missing_events;})==CollisionProgress::needs_observation);
 assert(missing_events==0&&ti==missing_ti&&tt==missing_tt);
 // Explicitly observed empty membership still refutes the membership predicate.
 unsigned empty_rejects=0;
 assert(engine.encounter(0,CollisionInput{binding,{},true},rng,[&](const auto& e){assert(e.status==EvidenceStatus::reject);++empty_rejects;})==CollisionProgress::verified);
 assert(empty_rejects>=3);
 std::cout<<"PASS: contradictory active claims challenged, weak links restored by input, duplicates per encounter coalesced, repeated collision refresh, invalid input abstains\n";
}
