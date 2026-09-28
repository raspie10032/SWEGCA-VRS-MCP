#include "vrs/input_collision.hpp"
#include <cassert>
#include <iostream>
using namespace swegca::vrs;using namespace swegca::architecture::kernel;
Digest id(unsigned x){Digest d{};d[0]=std::byte(x);return d;}
int main(){
 auto binding=bind_experience(id(1),id(1),id(2),id(3));
 // Stored active claims disagree with dirty input. They must be challenged,
 // not treated as labels or used to reject this input.
 std::vector<double> ti{1.01,1.01,.995},tt{1.01,.995,.995};InputCollision engine(1,3,ti,tt);
 CollisionInput raw{binding,{0,2,2}};std::mt19937_64 rng(19);unsigned reject=0,restored=0,seen=0;
 engine.encounter(0,raw,rng,[&](const CollisionEvent& e){++seen;reject+=e.status==EvidenceStatus::reject;restored+=e.previous<1&&e.current>=1;});
 assert(seen==5&&reject==2&&restored==2);assert(ti[1]<1.01&&ti[2]>1);assert(tt[0]<1.01&&tt[1]>1);
 const double old=ti[0];engine.encounter(0,raw,rng,[](const auto&){});assert(ti[0]==old*1.01);
 // Invalid observation still reaches the core and abstains without deleting state.
 const auto before=ti;engine.encounter(0,CollisionInput{{},{0}},rng,[](const auto& e){assert(e.status==EvidenceStatus::abstain&&e.current==e.previous);});assert(ti==before);
 std::cout<<"PASS: contradictory active claims challenged, weak links restored by input, duplicates per encounter coalesced, repeated collision refresh, invalid input abstains\n";
}
