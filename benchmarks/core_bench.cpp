#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/memory_promotion_kernel.hpp"
#include "swegca_architecture/memory_transaction_stage_kernel.hpp"
#include "vrs/verification.hpp"
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
template<class T> inline void consume(const T& value) {asm volatile("" : : "m"(value) : "memory");}
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
int main(){
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
 std::array<EvidenceJudgment,64> decisions{};
 for(std::size_t i=0;i<64;++i)decisions[i]=judge_evidence(r,fixtures[i]);
 SemanticPromotionThresholds thresholds;
 measure("promotion_eligibility",[&](std::size_t i){auto b=measured_eligible(decisions[i&63],thresholds);consume(b);});
 measure("promotion_decision",[&](std::size_t i){MemoryPromotionDecision o;auto b=measured_promotion(decisions[i&63],o);consume(b);consume(o);});
 measure("connection_strength",[&](std::size_t i){auto changed=measured_strength(0.9+0.01*(i&7),decisions[i&63]);consume(changed);});
 measure("connection_verification_4_axes",[&](std::size_t i){auto changed=measured_connection(r,fixtures[i&63],0.9+0.01*(i&7));consume(changed);});
 measure("transaction_transition",[&](std::size_t i){MemoryTransactionStage o=MemoryTransactionStage::prepared;auto b=measured_stage(static_cast<MemoryTransactionStage>(1+i%6),static_cast<MemoryTransactionEdge>(1+i%6),o);consume(b);consume(o);});
}
