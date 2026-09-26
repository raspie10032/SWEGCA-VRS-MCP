#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/memory_promotion_kernel.hpp"
#include "swegca_architecture/memory_transaction_stage_kernel.hpp"
#include "vrs/verification.hpp"
#include <array>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
static unsigned checks=0;
static std::size_t allocations=0;
void* operator new(std::size_t size){++allocations;if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::abort();}}while(false)
EvidenceTally tally(double support=100,double refute=0){
 EvidenceTally t;t.revision=1;t.source_diversity=4;t.context_diversity=6;
 t.axis_support.fill(support);t.axis_refute.fill(refute);t.axis_source_diversity.fill(2);return t;
}
void expect_invalid(const EvidenceRules& r,const EvidenceTally& t){
 const auto j=judge_evidence(r,t);CHECK(j.status()==EvidenceStatus::abstain);CHECK(j.reason()==EvidenceReason::invalid_input);
 CHECK(!semantic_promotion_evidence_eligible(j,SemanticPromotionThresholds{}));
}
int main(){
 static_assert(!std::is_aggregate_v<EvidenceJudgment>);
 static_assert(noexcept(judge_evidence(std::declval<const EvidenceRules&>(),std::declval<const EvidenceTally&>())));
 CHECK(std::fegetround()==FE_TONEAREST);
 const auto rules=make_evidence_rules(EvidencePolicy{});
 const auto accepted=judge_evidence(rules,tally());
 const auto rejected=judge_evidence(rules,tally(0,100));
 const auto uncertain=judge_evidence(rules,tally(50,50));
 CHECK(accepted.status()==EvidenceStatus::accept);CHECK(accepted.reason()==EvidenceReason::causal_lower_bound);
 CHECK(rejected.status()==EvidenceStatus::reject);CHECK(uncertain.reason()==EvidenceReason::uncertain);
 CHECK(!semantic_promotion_evidence_eligible(EvidenceJudgment{},SemanticPromotionThresholds{}));
 CHECK(semantic_promotion_evidence_eligible(accepted,SemanticPromotionThresholds{}));
 CHECK(!semantic_promotion_evidence_eligible(rejected,SemanticPromotionThresholds{}));
 for(double v:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),-1.0}){
  auto thresholds=SemanticPromotionThresholds{};thresholds.minimum_causal_lower_bound=v;
  CHECK(!semantic_promotion_evidence_eligible(accepted,thresholds));
 }
 for(unsigned axes=1;axes<=max_axes;++axes){
  auto p=EvidencePolicy{};p.axis_count=axes;auto r=make_evidence_rules(p);
  CHECK(judge_evidence(r,tally()).status()==EvidenceStatus::accept);
  for(unsigned axis=0;axis<axes;++axis){
   auto bad=tally();bad.axis_source_diversity[axis]=5;expect_invalid(r,bad);
   for(double v:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
    bad=tally();bad.axis_support[axis]=v;expect_invalid(r,bad);
    bad=tally();bad.axis_refute[axis]=v;expect_invalid(r,bad);
   }
  }
 }
 auto bad=tally();bad.revision=0;expect_invalid(rules,bad);
 const auto empty=judge_evidence(rules,EvidenceTally{});
 CHECK(empty.status()==EvidenceStatus::abstain);
 CHECK(empty.reason()==EvidenceReason::minimum_effective_samples);
 CHECK(empty.posterior_mean()==0.5 && empty.effective_sample_size()==0);
 CHECK(!semantic_promotion_evidence_eligible(empty,SemanticPromotionThresholds{}));
 auto impossible_empty=EvidenceTally{};impossible_empty.source_diversity=1;expect_invalid(rules,impossible_empty);
 impossible_empty={};impossible_empty.recent_count=1;expect_invalid(rules,impossible_empty);
 bad=tally();bad.recent_count=7;expect_invalid(rules,bad);
 bad=tally();bad.recent_count=6;bad.recent_sum=6.1;expect_invalid(rules,bad);
 bad=tally();bad.axis_support.fill(std::numeric_limits<double>::max());expect_invalid(rules,bad);
 auto limited=tally(0,0);CHECK(judge_evidence(rules,limited).reason()==EvidenceReason::minimum_effective_samples);
 limited=tally();limited.source_diversity=1;limited.axis_source_diversity.fill(1);
 CHECK(judge_evidence(rules,limited).reason()==EvidenceReason::source_diversity);
 limited=tally();limited.axis_source_diversity[0]=0;CHECK(judge_evidence(rules,limited).reason()==EvidenceReason::axis_source_diversity);
 limited=tally();limited.context_diversity=3;CHECK(judge_evidence(rules,limited).reason()==EvidenceReason::context_diversity);
 limited=tally();limited.recent_count=6;limited.recent_sum=0;
 const auto changed=judge_evidence(rules,limited);CHECK(changed.reason()==EvidenceReason::regime_change_suspected);
 auto zero=EvidencePolicy{};zero.regime_change_threshold=0;
 CHECK(judge_evidence(make_evidence_rules(zero),tally()).reason()==EvidenceReason::regime_change_suspected);
 struct PromotionCase {MemoryTier from;const EvidenceJudgment* j;bool cf;bool provenance;MemoryTier to;MemoryPromotionAction action;bool read;};
 const std::array cases{
  PromotionCase{MemoryTier::episodic,&accepted,true,true,MemoryTier::semantic,MemoryPromotionAction::promote,true},
  PromotionCase{MemoryTier::semantic,&accepted,true,true,MemoryTier::semantic,MemoryPromotionAction::refresh_semantic,true},
  PromotionCase{MemoryTier::semantic,&rejected,true,true,MemoryTier::retracted,MemoryPromotionAction::retract,false},
  PromotionCase{MemoryTier::episodic,&rejected,true,false,MemoryTier::quarantined,MemoryPromotionAction::quarantine,false},
  PromotionCase{MemoryTier::semantic,&accepted,false,true,MemoryTier::quarantined,MemoryPromotionAction::quarantine,false},
  PromotionCase{MemoryTier::episodic,&accepted,false,true,MemoryTier::episodic,MemoryPromotionAction::record_episode,false},
  PromotionCase{MemoryTier::semantic,&changed,true,true,MemoryTier::quarantined,MemoryPromotionAction::quarantine,false},
  PromotionCase{MemoryTier::episodic,&uncertain,true,true,MemoryTier::episodic,MemoryPromotionAction::record_episode,false}
 };
 for(auto c:cases){MemoryPromotionDecision o;CHECK(decide_memory_promotion(c.from,*c.j,c.cf,c.provenance,o));CHECK(o.next_tier==c.to);CHECK(o.action==c.action);CHECK(o.semantic_read_allowed==c.read);}
 MemoryPromotionDecision promotion;
 CHECK(decide_memory_promotion(MemoryTier::episodic,accepted,true,true,promotion));CHECK(promotion.semantic_read_allowed);
 CHECK(!decide_memory_promotion(MemoryTier::episodic,EvidenceJudgment{},true,true,promotion));
 CHECK(!promotion.semantic_read_allowed);CHECK(promotion.action==MemoryPromotionAction::none);CHECK(promotion.reason==MemoryPromotionReason::invalid_input);
 CHECK(decide_memory_promotion(MemoryTier::episodic,accepted,true,true,promotion));
 CHECK(!decide_memory_promotion(static_cast<MemoryTier>(255),accepted,true,true,promotion));CHECK(promotion.next_tier==MemoryTier::none);
 // Allowed state chart: columns commit-memory/state/complete/begin/finish/recover.
 constexpr unsigned edges[6][6]={{2,0,0,0,0,6},{0,3,0,0,0,6},{0,0,4,0,0,6},{0,0,0,5,0,0},{0,0,0,0,6,6},{0,0,0,0,0,0}};
 for(unsigned from=1;from<=6;++from)for(unsigned edge=1;edge<=6;++edge){
  auto out=MemoryTransactionStage::completed;
  CHECK(next_memory_transaction_stage(static_cast<MemoryTransactionStage>(from),static_cast<MemoryTransactionEdge>(edge),out)==(edges[from-1][edge-1]!=0));
  CHECK(static_cast<unsigned>(out)==edges[from-1][edge-1]);
 }
 auto stage=MemoryTransactionStage::completed;CHECK(!next_memory_transaction_stage(MemoryTransactionStage::invalid,MemoryTransactionEdge::complete,stage));CHECK(stage==MemoryTransactionStage::invalid);
 stage=MemoryTransactionStage::completed;CHECK(!next_memory_transaction_stage(MemoryTransactionStage::prepared,static_cast<MemoryTransactionEdge>(255),stage));CHECK(stage==MemoryTransactionStage::invalid);
 // Batch matches scalar, including full immutable results and an invalid item.
 constexpr std::size_t n=3;
 std::array<double,4*n> support{},refute{};std::array<std::uint32_t,4*n> diversity{};
 std::array<std::uint32_t,n> sources{4,4,4},contexts{6,6,6},recent{};
 std::array<double,n> sums{},means{},lower{},upper{},samples{},regime{};
 std::array<std::uint64_t,n> revisions{1,2,3};
 std::array<EvidenceStatus,n> status{};std::array<EvidenceReason,n> reason{};std::array<EvidenceJudgment,n> judged{};
 const std::array inputs{tally(),tally(),tally(0,100)};
 for(std::size_t a=0;a<4;++a)for(std::size_t i=0;i<n;++i){support[a*n+i]=inputs[i].axis_support[a];refute[a*n+i]=inputs[i].axis_refute[a];diversity[a*n+i]=2;}
 diversity[1]=5;
 EvidenceColumns in{n,support,refute,diversity,sources,contexts,recent,sums,revisions};
 EvidenceJudgmentColumns out{status,reason,means,lower,upper,samples,regime,judged};
 CHECK(judge_evidence_batch(rules,in,out,0,1));CHECK(judged[0].status()==EvidenceStatus::accept);CHECK(judged[1].reason()==EvidenceReason::invalid_input);
 CHECK(judge_evidence_batch(rules,in,out,1,n));CHECK(reason[1]==EvidenceReason::invalid_input);CHECK(status[2]==EvidenceStatus::reject);
 for(std::size_t i=0;i<n;++i){auto t=inputs[i];t.revision=i+1;if(i==1)t.axis_source_diversity[0]=5;auto j=judge_evidence(rules,t);
  CHECK(status[i]==j.status());CHECK(reason[i]==j.reason());CHECK(means[i]==j.posterior_mean());CHECK(lower[i]==j.causal_lower_bound());CHECK(upper[i]==j.overall_upper_bound());CHECK(samples[i]==j.effective_sample_size());CHECK(regime[i]==j.regime_change_score());CHECK(judged[i].revision()==i+1);
 }
 CHECK(semantic_promotion_evidence_eligible(judged[0],SemanticPromotionThresholds{}));
 auto overlap=out;
 overlap.status=std::span<EvidenceStatus>(reinterpret_cast<EvidenceStatus*>(judged.data()),n);
 CHECK(!judge_evidence_batch(rules,in,overlap,0,n));
 CHECK(judged[0].status()==EvidenceStatus::accept);
 auto short_judgments=out;short_judgments.judgments=std::span<EvidenceJudgment>(judged.data(),n-1);
 CHECK(!judge_evidence_batch(rules,in,short_judgments,0,n));
 out.judgments={};CHECK(judge_evidence_batch(rules,in,out,0,n));
 auto alias=out;alias.posterior_mean=std::span<double>(support.data(),n);auto saved=support;
 CHECK(!judge_evidence_batch(rules,in,alias,0,n));CHECK(support==saved);
 auto short_out=out;short_out.status=std::span<EvidenceStatus>(status.data(),n-1);CHECK(!judge_evidence_batch(rules,in,short_out,0,n));
 CHECK(!judge_evidence_batch(rules,in,out,0,n+1));
 const auto before=allocations;
 for(unsigned i=0;i<1000;++i){auto j=judge_evidence(rules,tally());CHECK(semantic_promotion_evidence_eligible(j,SemanticPromotionThresholds{}));CHECK(decide_memory_promotion(MemoryTier::episodic,j,true,true,promotion));CHECK(next_memory_transaction_stage(MemoryTransactionStage::prepared,MemoryTransactionEdge::commit_memory,stage));}
 CHECK(allocations==before);
 CHECK(swegca::vrs::verify_experience(rules,tally()).connection_change()==swegca::vrs::ConnectionChange::strengthen);
 std::printf("PASS: %u checks; core hot-path allocations: 0\n",checks);
}
