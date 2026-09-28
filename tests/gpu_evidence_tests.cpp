#include "vrs/gpu_evidence.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include <bit>
#include <future>
#include <iostream>
#include <cassert>
#include <limits>
#include <vector>
using namespace swegca::architecture;using namespace swegca::architecture::kernel;
int main(int argc,char**argv){assert(argc==2);swegca::vrs::GpuEvidence gpu(argv[1]);EvidencePolicy policy;auto rules=make_evidence_rules(policy);std::vector<std::future<void>> jobs;
 for(unsigned worker=0;worker<10;++worker)jobs.push_back(std::async(std::launch::async,[&,worker]{for(unsigned i=0;i<80;++i){EvidenceTally t{};unsigned mode=(i+worker)%8;t.revision=mode?1:0;
  if(mode){t.source_diversity=16;t.context_diversity=16;t.axis_source_diversity.fill(4);for(unsigned a=0;a<8;++a){t.axis_support[a]=mode==1?1000:mode==2?0:double((i*17+a)%111);t.axis_refute[a]=mode==1?0:mode==2?1000:double((i*3+a)%97);}}
  if(mode==4)t.axis_support[0]=std::numeric_limits<double>::infinity();if(mode==5)t.axis_refute[0]=-1;if(mode==6){t.recent_count=6;t.recent_sum=6;}if(mode==7)t.axis_support[0]=std::numeric_limits<double>::denorm_min();
  auto cpu=judge_evidence(rules,t),device=gpu.judge(rules,t);assert(cpu.status()==device.status()&&cpu.reason()==device.reason()&&cpu.revision()==device.revision());
  assert(cpu.source_diversity()==device.source_diversity()&&cpu.context_diversity()==device.context_diversity());
  const double x[]={cpu.posterior_mean(),cpu.causal_lower_bound(),cpu.overall_upper_bound(),cpu.effective_sample_size(),cpu.regime_change_score()};const double y[]={device.posterior_mean(),device.causal_lower_bound(),device.overall_upper_bound(),device.effective_sample_size(),device.regime_change_score()};
  for(unsigned j=0;j<5;++j)assert(std::bit_cast<std::uint64_t>(x[j])==std::bit_cast<std::uint64_t>(y[j]));
 }}));for(auto& j:jobs)j.get();std::cout<<"PASS: 800 actual CUDA core judgments across both GPUs match CPU fields bit-for-bit\n";}
