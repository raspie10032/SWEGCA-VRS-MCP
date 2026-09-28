#pragma once
#include "vrs/runtime.hpp"
#include <set>
#include <thread>
#include <vector>
namespace swegca::vrs {
struct ParallelInput { architecture::DigestBytes session; OriginalExperienceView original; };
struct ParallelResult { std::optional<RecordedRefinement> recorded; std::exception_ptr error; };
// One Runtime still owns Main, budgets and session lifetime. During a call the
// owner makes no other Runtime calls. Each attached session has exactly ONE
// writer. No Main work or lifecycle mutation runs inside these worker threads.
class ParallelIngress {
public:
 // The caller assigns one persistent worker to each distinct session. Session
 // topology/lifecycle stays fixed until all workers have joined.
 static RecordedRefinement retain_one(Runtime& owner,const architecture::DigestBytes& id,
     const OriginalExperienceView& original,std::uint64_t seed,std::uint64_t step,EvidenceExecutor* executor){
  const auto it=owner.sessions_.find(id);
  if(it==owner.sessions_.end())throw std::invalid_argument("unattached worker session");
  EvidenceExecutorScope scope(executor);
  return it->second.runtime.retain_input(original,owner.config_.initial_strength,owner.config_.policy,seed,step);
 }
 static std::vector<ParallelResult> retain(Runtime& owner,std::span<const ParallelInput> inputs,
                                         std::uint64_t seed,std::uint64_t step,EvidenceExecutor* executor=nullptr){
  if(inputs.size()>10)throw std::invalid_argument("at most ten ingress workers");
  std::set<architecture::DigestBytes> ids;std::vector<SessionRuntime*> targets;
  for(const auto& input:inputs){
   if(!ids.insert(input.session).second)throw std::invalid_argument("two writers for one session");
   const auto it=owner.sessions_.find(input.session);
   if(it==owner.sessions_.end())throw std::invalid_argument("session is not attached");
   if(!it->second.runtime.usable()||it->second.runtime.phase()!=architecture::kernel::SessionPhase::active)
    throw std::invalid_argument("inactive ingress session");
   targets.push_back(&it->second.runtime);
  }
  std::vector<ParallelResult> results(inputs.size());std::vector<std::jthread> workers;workers.reserve(inputs.size());
  for(std::size_t i=0;i<inputs.size();++i)workers.emplace_back([&,i]{
   try{EvidenceExecutorScope scope(executor);results[i].recorded.emplace(targets[i]->retain_input(inputs[i].original,
        owner.config_.initial_strength,owner.config_.policy,seed,step));}
   catch(...){results[i].error=std::current_exception();}
  });
  for(auto& worker:workers)worker.join();return results;
 }
};
}
