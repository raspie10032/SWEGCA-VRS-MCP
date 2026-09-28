#include "vrs/work_pipeline.hpp"
#include "vrs/device_evidence.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <set>
#include <bit>
using namespace swegca::vrs;using namespace swegca::architecture;using namespace swegca::architecture::kernel;
struct Decoded {unsigned id;std::vector<EvidenceTally> evidence;};
struct Verified {unsigned id;std::vector<EvidenceJudgment> judgments;};
using Pipeline=WorkPipeline<unsigned,Decoded,Verified>;
EvidenceTally tally(unsigned i){EvidenceTally t{};if(i%3){t.revision=1;t.source_diversity=16;t.context_diversity=16;t.axis_source_diversity.fill(4);for(unsigned a=0;a<4;++a){t.axis_support[a]=i%3==1?100:0;t.axis_refute[a]=i%3==2?100:0;}}return t;}
void equal(const EvidenceJudgment& a,const EvidenceJudgment& b){assert(a.status()==b.status()&&a.reason()==b.reason()&&a.revision()==b.revision());assert(a.source_diversity()==b.source_diversity()&&a.context_diversity()==b.context_diversity());const double x[]={a.posterior_mean(),a.causal_lower_bound(),a.overall_upper_bound(),a.effective_sample_size(),a.regime_change_score()},y[]={b.posterior_mean(),b.causal_lower_bound(),b.overall_upper_bound(),b.effective_sample_size(),b.regime_change_score()};for(unsigned i=0;i<5;++i)assert(std::bit_cast<std::uint64_t>(x[i])==std::bit_cast<std::uint64_t>(y[i]));}
int main(){
 const auto rules=make_evidence_rules({});std::atomic<unsigned> cpu_calls=0,gpu_calls=0;std::set<unsigned> applied;std::thread::id applier;std::atomic<bool> overlap=false,applying=false;
 auto decode=[&](unsigned id){if(applying)overlap=true;auto d=std::make_shared<Decoded>();d->id=id;for(unsigned i=0;i<1024;++i)d->evidence.push_back(tally(id+i));return d;};
 auto verify=[&](std::span<const std::shared_ptr<const Decoded>> batch,DeviceEvidence* gpu){
  if(applying)overlap=true;std::vector<EvidenceTally> in;for(auto& d:batch)in.insert(in.end(),d->evidence.begin(),d->evidence.end());std::vector<EvidenceJudgment> out(in.size());
  if(gpu){gpu->judge(rules,in,out);++gpu_calls;}else{cpu_evidence_batch(rules,in,out);++cpu_calls;}
  std::vector<Verified> results;size_t at=0;for(auto& d:batch){Verified v{d->id,{}};v.judgments.insert(v.judgments.end(),out.begin()+at,out.begin()+at+d->evidence.size());at+=d->evidence.size();results.push_back(std::move(v));}return results;
 };
 std::vector<std::shared_ptr<DeviceEvidence>> devices;std::vector<Pipeline::Backend> backends;
 for(unsigned i=0;i<10;++i)backends.push_back({"CPU"+std::to_string(i),1,64,[&](auto b){return verify(b,nullptr);}});
 for(int i=0;i<DeviceEvidence::available();++i){auto device=std::make_shared<DeviceEvidence>(i);devices.push_back(device);backends.push_back({"GPU"+std::to_string(i),8,std::max<size_t>(8,std::min<size_t>(256,device->capacity()/1024)),[&,device](auto b){return verify(b,device.get());}});}
 assert(devices.size()>=2);
 // Actual GPU batches are independently checked, not inferred from registration.
 for(auto& device:devices){std::vector<EvidenceTally> input(65536);for(unsigned i=0;i<input.size();++i)input[i]=tally(i);std::vector<EvidenceJudgment> result(input.size());device->judge(rules,input,result);for(unsigned i=0;i<input.size();++i)equal(result[i],judge_evidence(rules,input[i]));}
 Pipeline p({10,128,128,128,32,std::chrono::milliseconds(5)},decode,std::move(backends),[&](auto batch){
  applying=true;if(applier==std::thread::id{})applier=std::this_thread::get_id();assert(applier==std::this_thread::get_id());
  for(const auto& v:batch){assert(applied.insert(v.id).second);for(unsigned i=0;i<v.judgments.size();++i)equal(v.judgments[i],judge_evidence(rules,tally(v.id+i)));}
  std::this_thread::sleep_for(std::chrono::milliseconds(1));applying=false;
 });
 for(unsigned i=0;i<1024;++i)p.submit(i);p.finish();auto stats=p.stats();assert(stats.submitted==1024&&stats.decoded==1024&&stats.verified==1024&&stats.applied==1024&&applied.size()==1024&&overlap&&cpu_calls&&gpu_calls);
 std::cout<<"PASS pipeline: actual core judgments="<<1024*1024<<" CPU batches="<<cpu_calls<<" GPU batches="<<gpu_calls<<" apply batches="<<stats.apply_batches<<"; single applier, stage overlap, exact once, CPU/GPU bit parity\n";
 for(unsigned failure=0;failure<3;++failure){bool caught=false;try{
  Pipeline bad({2,2,2,2,8,std::chrono::milliseconds(1)},[&](unsigned id){if(failure==0&&id==2)throw std::runtime_error("codec failure");return decode(id);},{{"CPU",1,2,[&](auto b){if(failure==1)throw std::runtime_error("verify failure");return verify(b,nullptr);}}},[&](auto){if(failure==2)throw std::runtime_error("apply failure");});
  for(unsigned i=0;i<16;++i)bad.submit(i);bad.finish();
 }catch(const std::runtime_error&){caught=true;}assert(caught);}
 std::cout<<"PASS failure propagation and undersized result queue drain\n";
}
