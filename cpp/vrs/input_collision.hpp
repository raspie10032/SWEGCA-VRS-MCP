#pragma once
#include "swegca_architecture/association_kernel.hpp"
#include "swegca_architecture/ternary_count_kernel.hpp"
#include <algorithm>
#include <numeric>
#include <random>
#include <vector>
#include <stdexcept>
#include <functional>
#include <map>
#include "vrs/block_collectors.hpp"
namespace swegca::vrs {
using architecture::kernel::TernaryCount;
// Shared input/experience-member collision path, independent of media type.
// Members name recorded experience relations; they are not clean-data labels
// required of arbitrary payloads. The input adapter preserves the full original.
// An active edge is a claim to check, never a trusted label.
struct CollisionInput { architecture::kernel::BoundExperience binding; std::vector<std::uint32_t> members; bool membership_observed=false; };
enum class CollisionProgress { verified, needs_observation };
struct CollisionEvent { std::uint32_t input,left,right; bool member_pair,was_active; architecture::kernel::EvidenceStatus status; TernaryCount previous,current; };
class InputCollision final {
public:
 using Commit=std::function<void(std::size_t,std::size_t)>;
 using Dispatch=std::function<void(std::span<const architecture::kernel::AssociationEvidence>,std::span<architecture::kernel::AssociationJudgment>,Commit)>;
 InputCollision(unsigned input_count,unsigned member_count,std::vector<TernaryCount>& ti,std::vector<TernaryCount>& tt,Dispatch executor={},BlockCollectors* collectors=nullptr,std::size_t block_entries=65536)
 :dispatch(std::move(executor)),collectors_(collectors),block_entries_(block_entries),n(input_count),t(member_count),input_strength(ti),pair_strength(tt),neighbors(member_count),seen(tt.size()),listed(tt.size()) {
  if(!block_entries_||(collectors_&&collectors_->blocks()<block_count(ti.size(),tt.size(),block_entries_)))throw std::invalid_argument("collision block ownership");
  if(!n||t<2||ti.size()!=std::size_t(n)*t||tt.size()!=std::size_t(t)*(t-1)/2)throw std::invalid_argument("collision dimensions");
  for(unsigned a=0;a<t;++a)for(unsigned b=a+1;b<t;++b){auto id=pair_id(a,b);if(architecture::kernel::count_evidence_eligible(tt[id]))link(a,b,id);}
 }
 static std::size_t block_count(std::size_t ti,std::size_t tt,std::size_t entries=65536){if(!entries)throw std::invalid_argument("zero block size");return ti/entries+(ti%entries!=0)+tt/entries+(tt%entries!=0);}
 std::size_t pair_id(unsigned a,unsigned b)const noexcept {if(a>b)std::swap(a,b);return std::size_t(a)*(2ULL*t-a-1)/2+b-a-1;}
 template<class Sink> CollisionProgress encounter(unsigned input,const CollisionInput& raw,std::mt19937_64& rng,Sink&& sink){
  using namespace architecture::kernel;
  if(input>=n)throw std::invalid_argument("input id");
  // Missing membership is not an observed empty set. No synthetic verdict is
  // issued here; the owner must supply an actual raw-input observation path.
  if(!raw.membership_observed)return CollisionProgress::needs_observation;
  if(++epoch==0){std::fill(seen.begin(),seen.end(),0);epoch=1;}
  std::vector<unsigned> anchors=raw.members;
  for(auto member:anchors)if(member>=t)throw std::invalid_argument("input member outside current graph");
  // Direct input touches even weak links. Lookup through stored experience
  // uses only nonnegative accept-minus-reject counts. Neither a previous verdict nor proposal flags filter input.
  for(unsigned tag=0;tag<t;++tag)if(count_evidence_eligible(input_strength[std::size_t(tag)*n+input]))anchors.push_back(tag);
  std::sort(anchors.begin(),anchors.end());anchors.erase(std::unique(anchors.begin(),anchors.end()),anchors.end());
  struct Job {unsigned a,b;std::size_t id;bool pair;};std::vector<Job> jobs;jobs.reserve(t+anchors.size()*32);
  for(unsigned tag=0;tag<t;++tag)jobs.push_back({tag,0,std::size_t(tag)*n+input,false});
  const auto add=[&](unsigned a,unsigned b,std::size_t id){if(seen[id]!=epoch){seen[id]=epoch;jobs.push_back({a,b,id,true});}};
  for(auto a:anchors)for(auto [b,id]:neighbors[a])if(count_evidence_eligible(pair_strength[id]))add(std::min(a,b),std::max(a,b),id);
  // The input itself can supply a new witness to a weak or absent connection.
  // It need not pass a stored-strength gate to get verified.
  for(std::size_t i=0;i<raw.members.size();++i)for(std::size_t j=i+1;j<raw.members.size();++j){auto a=raw.members[i],b=raw.members[j];if(a!=b)add(std::min(a,b),std::max(a,b),pair_id(a,b));}
  std::shuffle(jobs.begin(),jobs.end(),rng);
  // This encounter owns one graph revision. Every job names a unique edge;
  // workers only read its immutable evidence, and the owner alone commits.
  const auto revision=++generation;
  std::vector<AssociationEvidence> evidence(jobs.size());
  std::vector<AssociationJudgment> judgments(jobs.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 4096) num_threads(10)
#endif
  for(std::size_t i=0;i<jobs.size();++i){
   const auto& job=jobs[i];
   const auto observed=job.pair?observe_member_association(raw.binding,raw.members,job.a,job.b):observe_recorded_member(raw.binding,raw.members,job.a);
   const auto outcome=to_outcome(observed);
   evidence[i]={outcome==EvidenceOutcome::support?1ULL:0ULL,outcome==EvidenceOutcome::refute?1ULL:0ULL};
  }
  std::vector<CollisionEvent> events(jobs.size());
  std::size_t committed=0;
  const auto apply=[&](std::size_t i){const auto& job=jobs[i];const auto& judgment=judgments[i];
   auto& value=job.pair?pair_strength[job.id]:input_strength[job.id];const auto previous=value;
   if(!accumulate_verdict(value,judgment.status()))throw std::overflow_error("ternary count overflow");
   events[i]={input,job.a,job.b,job.pair,count_evidence_eligible(previous),judgment.status(),previous,value};
  };
  const auto commit=[&](std::size_t first,std::size_t last){
   if(revision!=generation||first!=committed||last<first||last>jobs.size())throw std::logic_error("stale/duplicate collision commit");
   if(collectors_){
    std::map<std::size_t,std::vector<std::size_t>> grouped;
    const auto pair_base=input_strength.size()/block_entries_+(input_strength.size()%block_entries_!=0);
    for(auto i=first;i<last;++i){const auto& job=jobs[i];grouped[(job.pair?pair_base:0)+job.id/block_entries_].push_back(i);}
    for(auto& [block,indices]:grouped)collectors_->submit(block,[&,indices=std::move(indices)]{for(auto i:indices)apply(i);});
   }else for(auto i=first;i<last;++i)apply(i);
   committed=last;
  };
  try {
   if(dispatch)dispatch(evidence,judgments,commit);
   else {for(std::size_t i=0;i<jobs.size();++i)judgments[i]=judge_association(evidence[i]);commit(0,jobs.size());}
   if(collectors_)collectors_->finish();
  }catch(...){if(collectors_)collectors_->drain();throw;}
  if(committed!=jobs.size())throw std::logic_error("missing collision results");
  // Publish cross-block adjacency in the original deterministic order only
  // after the owning blocks have applied their values. Endpoints/IDs are not
  // renumbered or dropped at physical block boundaries.
  for(std::size_t i=0;i<jobs.size();++i){const auto& job=jobs[i];
   if(job.pair&&count_evidence_eligible(events[i].current))link(job.a,job.b,job.id);
   sink(events[i]);
  }
  return CollisionProgress::verified;
 }
private:
 Dispatch dispatch;BlockCollectors* collectors_;std::size_t block_entries_;std::uint64_t generation=0;
 void link(unsigned a,unsigned b,std::size_t id){if(!listed[id]){listed[id]=1;neighbors[a].push_back({b,id});neighbors[b].push_back({a,id});}}
 unsigned n,t;std::vector<TernaryCount>& input_strength;std::vector<TernaryCount>& pair_strength;
 std::vector<std::vector<std::pair<unsigned,std::size_t>>> neighbors;
 std::vector<std::uint32_t> seen;std::vector<std::uint8_t> listed;std::uint32_t epoch=0;
};
}
