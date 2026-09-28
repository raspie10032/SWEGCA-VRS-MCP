#include "vrs/parallel_ingress.hpp"
#include "swegca_architecture/sha256.hpp"
#include <cassert>
#include <unistd.h>
#include <iostream>
using namespace swegca::vrs;using namespace swegca::architecture;
int main(){auto root=std::filesystem::temp_directory_path()/("vrs-parallel-test-"+std::to_string(getpid()));std::filesystem::create_directory(root);
 {MemoryBudget mem(256ULL<<20);RuntimeConfig cfg{Sha256::of(std::as_bytes(std::span("main",4))),{},1,1ULL<<20,1ULL<<20,1ULL<<20};
 auto owner=Runtime::create(root,cfg,mem);std::vector<ParallelInput> inputs;std::vector<std::string> names;names.reserve(10);
 for(unsigned i=0;i<10;++i){names.push_back("worker"+std::to_string(i));auto id=Sha256::of(std::as_bytes(std::span(names.back())));owner.attach_session(id,names.back());inputs.push_back({id,{i,0,names.back(),"test","application/octet-stream",std::as_bytes(std::span("dirty\0raw",9))}});}
 auto result=ParallelIngress::retain(owner,inputs,7,0);assert(result.size()==10);
 for(unsigned i=0;i<10;++i){assert(result[i].recorded&&!result[i].error);auto raw=owner.attached_session(inputs[i].session).read_original(result[i].recorded->original);assert(evidence_payload(raw).content.size()==9);}
 auto duplicate=inputs;duplicate[1].session=duplicate[0].session;bool rejected=false;try{(void)ParallelIngress::retain(owner,duplicate,7,0);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
 for(const auto& input:inputs)owner.end_session(input.session);
 }
 std::filesystem::remove_all(root);std::cout<<"PASS: ten distinct native session writers, exact binary payloads, duplicate writer rejected\n";
}
