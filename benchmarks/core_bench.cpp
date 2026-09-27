#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/evidence_observation_kernel.hpp"
#include "swegca_architecture/session_kernel.hpp"
#include "swegca_architecture/head_publication_kernel.hpp"
#include "swegca_architecture/recall_route_kernel.hpp"
#include "swegca_architecture/replay_evidence_kernel.hpp"
#include "swegca_architecture/memory_promotion_kernel.hpp"
#include "swegca_architecture/memory_transaction_stage_kernel.hpp"
#include "vrs/verification.hpp"
#include "swegca_architecture/metadata_residency_kernel.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
__attribute__((noinline)) EvidenceJudgment measured_judge(const EvidenceRules& r,const EvidenceTally& t) noexcept {return judge_evidence(r,t);}
__attribute__((noinline)) bool measured_eligible(const EvidenceJudgment& j,const SemanticPromotionThresholds& t) noexcept {return semantic_promotion_evidence_eligible(j,t);}
__attribute__((noinline)) bool measured_stage(MemoryTransactionStage s,MemoryTransactionEdge e,MemoryTransactionStage& o) noexcept {return next_memory_transaction_stage(s,e,o);}
__attribute__((noinline)) bool measured_promotion(const EvidenceJudgment& j,MemoryPromotionDecision& o) noexcept {
 return decide_memory_promotion(MemoryTier::episodic,j,true,true,o);
}
__attribute__((noinline)) ConnectionStrengthResult measured_strength(double previous,const EvidenceJudgment& j) noexcept {
 return revise_connection_strength(previous,j);
}
__attribute__((noinline)) swegca::vrs::ConnectionVerification measured_connection(const EvidenceRules& r,const EvidenceTally& t,double previous) noexcept {
 return swegca::vrs::verify_connection(r,t,previous);
}
__attribute__((noinline)) ObservationUse measured_admission(const EvidenceRules& r,const Digest& hypothesis,const EvidenceObservation& value,std::uint64_t step,bool seen) noexcept {
 return admit_observation(r,hypothesis,value,step,seen);
}
__attribute__((noinline)) EffectiveEvidence measured_group(std::uint32_t supports,std::uint32_t refutes) noexcept {
 return normalize_evidence_group(supports,refutes);
}
__attribute__((noinline)) bool measured_session(SessionPhase phase,SessionOperation operation,SessionPhase& next) noexcept {
 return next_session_phase(phase,operation,next);
}
__attribute__((noinline)) HeadPublication measured_publication(const ConnectionHead* current,const RecordAddress& expected,const ConnectionHead& candidate,bool lineage) noexcept {
 return assess_head_publication(current,expected,candidate,lineage);
}
template<class T> inline void consume(const T& value) {asm volatile("" : : "m"(value) : "memory");}
__attribute__((noinline)) FamiliarityKey measured_familiarity(bool exact,bool continued) noexcept {
 return familiarity_key(exact,continued);
}
__attribute__((noinline)) ReplayAgreement measured_replay(const EvidenceRules& rules,const EvidenceObservation& original,
 const Digest& hypothesis,const EvidenceJudgment& current,std::uint64_t step) noexcept {
 return compare_replay_evidence(rules,original,hypothesis,current,step);
}
__attribute__((noinline)) ReplayPreference measured_selection(const ReplayCandidate* current,const ReplayCandidate& candidate) noexcept {
 return prefer_replay(current,candidate);
}
__attribute__((noinline)) bool measured_context_reference(bool input,bool seed) noexcept {
 return context_reference_eligible(input,seed);
}
template<class Fn> void measure(const char* name,Fn fn) {
 constexpr std::size_t iterations=100000;
 for(std::size_t i=0;i<iterations;++i)fn(i);
 std::array<double,31> samples{};
 for(auto& elapsed:samples){
  const auto start=std::chrono::steady_clock::now();
  for(std::size_t i=0;i<iterations;++i)fn(i);
  const auto end=std::chrono::steady_clock::now();
  elapsed=std::chrono::duration<double,std::nano>(end-start).count()/iterations;
 }
 std::sort(samples.begin(),samples.end());
 std::cout<<name<<",median_ns="<<samples[15]<<",p95_block_mean_ns="<<samples[29]<<",min_ns="<<samples.front()<<'\n';
}
__attribute__((noinline)) MetadataRelease measured_metadata(bool complete,std::size_t owners,bool backed) noexcept {
 return metadata_release(complete,owners,backed);
}
int main(){
 measure("metadata_release",[&](std::size_t i){auto result=measured_metadata(i%2,i%3,i%5!=0);consume(result);});
 std::array<EvidenceTally,64> fixtures{};
 for(std::size_t i=0;i<fixtures.size();++i){
  auto& t=fixtures[i];t.source_diversity=4;t.context_diversity=6;t.revision=i+1;
  for(std::size_t a=0;a<max_axes;++a){t.axis_support[a]=(i%3==0?90: i%3==1?1:40)+i%5;t.axis_refute[a]=(i%3==0?3:i%3==1?90:55);t.axis_source_diversity[a]=2;}
 }
 for(unsigned axes:{1U,4U,8U}){
  EvidencePolicy p;p.axis_count=axes;const auto r=make_evidence_rules(p);
  const char* name=axes==1?"judge_1_axis":axes==4?"judge_4_axes":"judge_8_axes";
  measure(name,[&](std::size_t i){auto j=measured_judge(r,fixtures[i&63]);consume(j);});
 }
 const auto r=make_evidence_rules(EvidencePolicy{});
 Digest hypothesis{};hypothesis[0]=std::byte{1};
 std::array<EvidenceObservation,64> observations{};
 for(std::size_t i=0;i<observations.size();++i){
  auto& o=observations[i];o.hypothesis=hypothesis;o.address=o.source=o.context=o.producer=hypothesis;
  o.axis=i%4;o.outcome=static_cast<EvidenceOutcome>(i%3);o.has_expiry=i%4==0;o.expires_at=10;
 }
 measure("observation_admission",[&](std::size_t i){auto a=measured_admission(r,hypothesis,observations[i&63],i%16,i%7==0);consume(a);});
 measure("evidence_group_normalization",[&](std::size_t i){auto g=measured_group(i%16,i%7);consume(g);});
 measure("session_transition",[&](std::size_t i){SessionPhase next;auto valid=measured_session(static_cast<SessionPhase>(i%5),static_cast<SessionOperation>((i/5)%5),next);consume(valid);consume(next);});
 measure("context_reference_eligibility",[&](std::size_t i){auto eligible=measured_context_reference(i%2,(i/2)%2);consume(eligible);});
 measure("familiarity_key",[&](std::size_t i){auto key=measured_familiarity(i%2,(i/2)%2);consume(key);});
 ConnectionHead current;current.identity=hypothesis;current.record={hypothesis,80,300,hypothesis};current.revision=2;current.ordinal=2;current.observations=1;current.strength=0.5;
 std::array<ConnectionHead,64> versions{};
 for(std::size_t i=0;i<versions.size();++i){versions[i]=current;versions[i].ordinal=3+i;versions[i].revision=3+i;versions[i].record.offset=400+300*i;versions[i].strength=0.51+0.001*i;}
 ReplayCandidate selected{1.0,10,hypothesis,current.record};
 std::array<ReplayCandidate,64> candidates{};
 for(std::size_t i=0;i<candidates.size();++i){
  candidates[i]={0.9+0.1*(i%3),i%16,hypothesis,versions[i].record};
 }
 measure("replay_candidate_selection",[&](std::size_t i){auto result=measured_selection(i%7?&selected:nullptr,candidates[i&63]);consume(result);});
 measure("head_publication",[&](std::size_t i){auto result=measured_publication(i%7?&current:nullptr,i%7?current.record:RecordAddress{},versions[i&63],i%3!=0);consume(result);});
 std::array<EvidenceJudgment,64> decisions{};
 for(std::size_t i=0;i<64;++i)decisions[i]=judge_evidence(r,fixtures[i]);
 measure("replay_evidence_comparison",[&](std::size_t i){auto result=measured_replay(r,observations[i&63],hypothesis,decisions[i&63],i%16);consume(result);});
 SemanticPromotionThresholds thresholds;
 measure("promotion_eligibility",[&](std::size_t i){auto b=measured_eligible(decisions[i&63],thresholds);consume(b);});
 measure("promotion_decision",[&](std::size_t i){MemoryPromotionDecision o;auto b=measured_promotion(decisions[i&63],o);consume(b);consume(o);});
 measure("connection_strength",[&](std::size_t i){auto changed=measured_strength(0.9+0.01*(i&7),decisions[i&63]);consume(changed);});
 measure("connection_verification_4_axes",[&](std::size_t i){auto changed=measured_connection(r,fixtures[i&63],0.9+0.01*(i&7));consume(changed);});
 measure("transaction_transition",[&](std::size_t i){MemoryTransactionStage o=MemoryTransactionStage::prepared;auto b=measured_stage(static_cast<MemoryTransactionStage>(1+i%6),static_cast<MemoryTransactionEdge>(1+i%6),o);consume(b);consume(o);});
}
