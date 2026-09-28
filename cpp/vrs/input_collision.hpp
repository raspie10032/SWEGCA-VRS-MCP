#pragma once
#include "swegca_architecture/association_kernel.hpp"
#include "swegca_architecture/ternary_count_kernel.hpp"
#include <algorithm>
#include <numeric>
#include <random>
#include <vector>
#include <stdexcept>
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
 InputCollision(unsigned input_count,unsigned member_count,std::vector<TernaryCount>& ti,std::vector<TernaryCount>& tt)
 :n(input_count),t(member_count),input_strength(ti),pair_strength(tt),neighbors(member_count),seen(tt.size()),listed(tt.size()) {
  if(!n||t<2||ti.size()!=std::size_t(n)*t||tt.size()!=std::size_t(t)*(t-1)/2)throw std::invalid_argument("collision dimensions");
  for(unsigned a=0;a<t;++a)for(unsigned b=a+1;b<t;++b){auto id=pair_id(a,b);if(architecture::kernel::count_evidence_eligible(tt[id]))link(a,b,id);}
 }
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
  for(const auto& job:jobs){
   // The proposition is applicability to THIS input, not the old global
   // existential claim that a pair co-occurred somewhere in the corpus.
   const auto observed=job.pair?observe_member_association(raw.binding,raw.members,job.a,job.b):observe_recorded_member(raw.binding,raw.members,job.a);
   const auto outcome=to_outcome(observed);
   const auto judgment=judge_association({outcome==EvidenceOutcome::support?1ULL:0ULL,outcome==EvidenceOutcome::refute?1ULL:0ULL});
   auto& value=job.pair?pair_strength[job.id]:input_strength[job.id];const auto previous=value;
   if(!accumulate_verdict(value,judgment.status()))throw std::overflow_error("ternary count overflow");
   if(job.pair&&count_evidence_eligible(value))link(job.a,job.b,job.id);
   sink(CollisionEvent{input,job.a,job.b,job.pair,count_evidence_eligible(previous),judgment.status(),previous,value});
  }
  return CollisionProgress::verified;
 }
private:
 void link(unsigned a,unsigned b,std::size_t id){if(!listed[id]){listed[id]=1;neighbors[a].push_back({b,id});neighbors[b].push_back({a,id});}}
 unsigned n,t;std::vector<TernaryCount>& input_strength;std::vector<TernaryCount>& pair_strength;
 std::vector<std::vector<std::pair<unsigned,std::size_t>>> neighbors;
 std::vector<std::uint32_t> seen;std::vector<std::uint8_t> listed;std::uint32_t epoch=0;
};
}
