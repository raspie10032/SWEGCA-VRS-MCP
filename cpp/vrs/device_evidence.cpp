#include "vrs/device_evidence.hpp"
#include "gpu_core_source.hpp"
#include <cuda.h>
#include <nvrtc.h>
#include <vector>
#include <mutex>
#include <stdexcept>
namespace swegca::vrs {
using namespace architecture::kernel;
namespace {
void check(CUresult r){if(r!=CUDA_SUCCESS){const char* s=nullptr;cuGetErrorString(r,&s);throw std::runtime_error(s?s:"CUDA error");}}
void check(nvrtcResult r){if(r!=NVRTC_SUCCESS)throw std::runtime_error(nvrtcGetErrorString(r));}
const std::vector<char>& program(){static const auto ptx=[] {
 const char* source=R"(
#include "swegca_architecture/evidence_scalar.hpp"
#include "swegca_architecture/association_scalar.hpp"
using namespace swegca::architecture::kernel;
extern "C" __global__ void layout(unsigned long* p){p[0]=sizeof(EvidenceRules);p[1]=sizeof(EvidenceTally);p[2]=sizeof(EvidenceJudgment);p[3]=sizeof(AssociationEvidence);p[4]=sizeof(AssociationJudgment);}
extern "C" __global__ void associations(const AssociationEvidence* in,AssociationJudgment* out,unsigned long n){for(unsigned long i=(unsigned long)blockIdx.x*blockDim.x+threadIdx.x;i<n;i+=(unsigned long)gridDim.x*blockDim.x)out[i]=judge_association(in[i]);}
extern "C" __global__ void run(const EvidenceRules* r,const EvidenceTally* t,EvidenceJudgment* o,unsigned long n){
 for(unsigned long i=(unsigned long)blockIdx.x*blockDim.x+threadIdx.x;i<n;i+=(unsigned long)gridDim.x*blockDim.x)o[i]=judge_evidence(*r,t[i]);
})";
 const char* bodies[]={embedded::core_platform,embedded::evidence_scalar,embedded::association_scalar};const char* names[]={"swegca_architecture/core_platform.hpp","swegca_architecture/evidence_scalar.hpp","swegca_architecture/association_scalar.hpp"};
 nvrtcProgram p;check(nvrtcCreateProgram(&p,source,"swegca-batch.cu",3,bodies,names));
 const char* options[]={"--std=c++20","--device-as-default-execution-space","--gpu-architecture=compute_89","--fmad=false","--ftz=false","--prec-div=true","--prec-sqrt=true"};
 auto result=nvrtcCompileProgram(p,7,options);if(result!=NVRTC_SUCCESS){size_t n;nvrtcGetProgramLogSize(p,&n);std::string log(n,'\0');nvrtcGetProgramLog(p,log.data());nvrtcDestroyProgram(&p);throw std::runtime_error(log);}
 size_t n;check(nvrtcGetPTXSize(p,&n));std::vector<char> v(n);check(nvrtcGetPTX(p,v.data()));check(nvrtcDestroyProgram(&p));return v;
 }();return ptx;}
}
struct DeviceEvidence::Impl {
 int device;CUcontext context=nullptr;CUmodule module=nullptr;CUfunction run=nullptr,association=nullptr;CUdeviceptr rules=0,tallies=0,output=0,association_input=0,association_output=0;std::size_t allocated=0,association_capacity=0;std::mutex mutex;
 explicit Impl(int d):device(d){
  check(cuInit(0));check(cuDevicePrimaryCtxRetain(&context,device));
  try{check(cuCtxSetCurrent(context));check(cuModuleLoadData(&module,program().data()));check(cuModuleGetFunction(&run,module,"run"));check(cuModuleGetFunction(&association,module,"associations"));check(cuMemAlloc(&rules,sizeof(EvidenceRules)));
   CUfunction layout;check(cuModuleGetFunction(&layout,module,"layout"));void* args[]={&rules};check(cuLaunchKernel(layout,1,1,1,1,1,1,0,nullptr,args,nullptr));unsigned long sizes[5];check(cuMemcpyDtoH(sizes,rules,sizeof(sizes)));
   if(sizes[0]!=sizeof(EvidenceRules)||sizes[1]!=sizeof(EvidenceTally)||sizes[2]!=sizeof(EvidenceJudgment)||sizes[3]!=sizeof(AssociationEvidence)||sizes[4]!=sizeof(AssociationJudgment))throw std::runtime_error("SWEGCA device ABI mismatch");
  }catch(...){release();throw;}
 }
 void release(){if(context)cuCtxSetCurrent(context);if(rules)cuMemFree(rules);if(tallies)cuMemFree(tallies);if(output)cuMemFree(output);if(association_input)cuMemFree(association_input);if(association_output)cuMemFree(association_output);if(module)cuModuleUnload(module);if(context)cuDevicePrimaryCtxRelease(device);context=nullptr;}
 ~Impl(){release();}
 std::size_t capacity(){check(cuCtxSetCurrent(context));size_t free,total;check(cuMemGetInfo(&free,&total));constexpr size_t reserve=512ULL<<20;return allocated+(free>reserve?(free-reserve)/(sizeof(EvidenceTally)+sizeof(EvidenceJudgment)):0);}
 void judge(const EvidenceRules& r,std::span<const EvidenceTally> in,std::span<EvidenceJudgment> out){
  std::lock_guard l(mutex);if(in.size()!=out.size())throw std::invalid_argument("device output count");if(in.empty())return;check(cuCtxSetCurrent(context));
  if(in.size()>allocated){if(in.size()>capacity())throw std::runtime_error("device batch exceeds available VRAM");
   if(tallies){check(cuMemFree(tallies));tallies=0;}
   if(output){check(cuMemFree(output));output=0;}
   allocated=0;
   CUdeviceptr t=0,o=0;try{check(cuMemAlloc(&t,in.size_bytes()));check(cuMemAlloc(&o,out.size_bytes()));}catch(...){if(t)cuMemFree(t);if(o)cuMemFree(o);throw;}
   if(tallies)check(cuMemFree(tallies));if(output)check(cuMemFree(output));tallies=t;output=o;allocated=in.size();
  }
  check(cuMemcpyHtoD(rules,&r,sizeof(r)));check(cuMemcpyHtoD(tallies,in.data(),in.size_bytes()));unsigned long n=in.size();void* args[]={&rules,&tallies,&output,&n};
  check(cuLaunchKernel(run,(n+255)/256,1,1,256,1,1,0,nullptr,args,nullptr));check(cuMemcpyDtoH(out.data(),output,out.size_bytes()));
 }
};
DeviceEvidence::DeviceEvidence(int d):impl_(std::make_unique<Impl>(d)){}
DeviceEvidence::~DeviceEvidence()=default;
void DeviceEvidence::associate(std::span<const AssociationEvidence> in,std::span<AssociationJudgment> out){
 if(in.size()!=out.size())throw std::invalid_argument("association output size");if(in.empty())return;
 std::lock_guard lock(impl_->mutex);check(cuCtxSetCurrent(impl_->context));
 if(in.size()>impl_->association_capacity){
  CUdeviceptr a=0,b=0;
  try{check(cuMemAlloc(&a,in.size_bytes()));check(cuMemAlloc(&b,out.size_bytes()));}catch(...){if(a)cuMemFree(a);if(b)cuMemFree(b);throw;}
  if(impl_->association_input)check(cuMemFree(impl_->association_input));if(impl_->association_output)check(cuMemFree(impl_->association_output));
  impl_->association_input=a;impl_->association_output=b;impl_->association_capacity=in.size();
 }
 check(cuMemcpyHtoD(impl_->association_input,in.data(),in.size_bytes()));unsigned long n=in.size();void* args[]={&impl_->association_input,&impl_->association_output,&n};
 check(cuLaunchKernel(impl_->association,(n+255)/256,1,1,256,1,1,0,nullptr,args,nullptr));check(cuMemcpyDtoH(out.data(),impl_->association_output,out.size_bytes()));
}

int DeviceEvidence::available(){check(cuInit(0));int n;check(cuDeviceGetCount(&n));return n;}
std::size_t DeviceEvidence::capacity()const{std::lock_guard l(impl_->mutex);return impl_->capacity();}
void DeviceEvidence::judge(const EvidenceRules& r,std::span<const EvidenceTally> in,std::span<EvidenceJudgment> out){impl_->judge(r,in,out);}

void cpu_evidence_batch(const EvidenceRules& rules,std::span<const EvidenceTally> in,std::span<EvidenceJudgment> out){
 if(in.size()!=out.size())throw std::invalid_argument("CPU output count");const auto n=in.size(),axes=std::size_t(rules.axis_count());
 std::vector<double> support(n*axes),refute(n*axes),recent_sum(n);std::vector<std::uint32_t> axis_sources(n*axes),sources(n),contexts(n),recent_count(n);std::vector<std::uint64_t> revisions(n);
 for(size_t i=0;i<n;++i){const auto& t=in[i];for(size_t a=0;a<axes;++a){support[a*n+i]=t.axis_support[a];refute[a*n+i]=t.axis_refute[a];axis_sources[a*n+i]=t.axis_source_diversity[a];}sources[i]=t.source_diversity;contexts[i]=t.context_diversity;recent_count[i]=t.recent_count;recent_sum[i]=t.recent_sum;revisions[i]=t.revision;}
 std::vector<EvidenceStatus> status(n);std::vector<EvidenceReason> reason(n);std::vector<double> mean(n),lower(n),upper(n),samples(n),regime(n);
 if(!judge_evidence_batch(rules,{n,support,refute,axis_sources,sources,contexts,recent_count,recent_sum,revisions},{status,reason,mean,lower,upper,samples,regime,out},0,n))throw std::runtime_error("SWEGCA batch rejected input layout");
}
}
