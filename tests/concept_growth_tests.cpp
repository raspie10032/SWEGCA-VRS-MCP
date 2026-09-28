#include "vrs/concept_growth.hpp"
#include <cassert>
#include <cmath>
#include <sstream>
#include <iostream>
using namespace swegca::architecture::kernel;using swegca::vrs::ConceptGrowth;
Digest id(unsigned i){Digest d{};d[0]=std::byte(i);return d;}
int main(){
 auto a=connection_growth({}, {90,10,0}),b=connection_growth({}, {9,1,90});
 assert(a.observations==100&&b.observations==100);
 assert(a.accept_fraction==.9&&b.accept_fraction==.09&&b.abstain_fraction==.9);
 assert(connection_growth({1,2,3},{1,2,3}).state==GrowthState::no_observation);
 assert(connection_growth({1,2,3},{0,3,4}).state==GrowthState::invalid_counts);
 auto abstain=connection_growth({}, {0,0,5});assert(abstain.abstain_fraction==1&&abstain.refute_fraction==0);
 const auto max=std::numeric_limits<std::uint64_t>::max();assert(connection_growth({}, {max,1,0}).state==GrowthState::invalid_counts);
 auto near=connection_growth({}, {max-2,1,1});assert(near.state==GrowthState::observed&&near.observations==max);
 TernaryCount counts{max,2,3};assert(!merge_growth_counts(counts,{1,0,0})&&(counts==TernaryCount{max,2,3}));
 ConceptGrowth first,second;
 first.observe(id(1),{}, {90,10,0});second.observe(id(1),{}, {9,1,90});second.observe(id(2),{}, {0,1,0});first.merge(second);
 assert((first.growth().delta==TernaryCount{99,12,90}));assert(first.growth().observations==201);
 auto spread=first.spread(10);assert(spread.valid&&spread.tested==2&&spread.supported==1&&spread.supported_fraction==.1);
 assert(!first.spread(1).valid);assert(!experience_spread(10,1,2).valid);
 ConceptGrowth empty;std::ostringstream out;empty.write(out,4,2,0);assert(out.str().find("\"fractions\":null")!=std::string::npos);
 std::ostringstream observed;first.write(observed,11,2,10);assert(observed.str().find("\"delta\":[99,12,90]")!=std::string::npos&&observed.str().find("\"promotion_decision\":null")!=std::string::npos);
 std::cout<<"PASS three-state deltas, abstention denominator, no-observation, overflow, exact cardinality across blocks, weighted merge, explicit no-promotion\n";
}
