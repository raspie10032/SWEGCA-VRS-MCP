#include "vrs/collision_dispatch.hpp"
#include "vrs/input_collision.hpp"
#include <cassert>
#include <iostream>
#include <chrono>
using namespace swegca::vrs;using namespace swegca::architecture::kernel;
int main(int argc,char**argv){
 assert(argc==2);CollisionDispatch pool(argv[1]);
 std::vector<AssociationEvidence> evidence(2000000);
 for(size_t i=0;i<evidence.size();++i)evidence[i]={i%2,(i/2)%2};
 std::vector<AssociationJudgment> result(evidence.size());size_t committed=0;
 auto begin=std::chrono::steady_clock::now();
 pool.judge(evidence,result,[&](size_t first,size_t last){assert(first==committed);for(size_t i=first;i<last;++i){auto expected=judge_association(evidence[i]);assert(result[i].status()==expected.status());assert(result[i].reason()==expected.reason());}committed=last;});
 assert(committed==evidence.size());
 std::vector<TernaryCount> ti(64*64,{1,0,0}),tt(64*63/2,{1,0,0}),ti2=ti,tt2=tt;
 InputCollision serial(64,64,ti,tt),parallel(64,64,ti2,tt2,[&](auto in,auto out,auto commit){pool.judge(in,out,commit);});
 Digest d{};d[0]=std::byte{1};auto bound=bind_experience(d,d,d,d);std::mt19937_64 a(39),b(39);
 for(unsigned round=0;round<3;++round)for(unsigned i=0;i<64;++i){CollisionInput input{bound,{i,(i+1)%64,(i+17)%64},true};std::vector<CollisionEvent> events;serial.encounter(i,input,a,[&](auto e){events.push_back(e);});size_t n=0;parallel.encounter(i,input,b,[&](auto e){auto expected=events.at(n++);assert(e.input==expected.input&&e.left==expected.left&&e.right==expected.right&&e.status==expected.status&&e.previous==expected.previous&&e.current==expected.current);});assert(n==events.size());assert(ti==ti2&&tt==tt2);}
 bool thrown=false;try{pool.judge(evidence,result,[](auto,auto){throw std::runtime_error("committer failure");});}catch(const std::runtime_error&){thrown=true;}assert(thrown);
 pool.judge(evidence,result); // worker completion survives committer failure
 std::cout<<"PASS: 2M mixed verdicts, ordered single committer, 3-round graph equivalence, committer failure drain; seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<'\n';
}
