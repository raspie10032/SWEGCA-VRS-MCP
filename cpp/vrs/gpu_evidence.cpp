#include "vrs/gpu_evidence.hpp"
#include "gpu_core_source.hpp"
#include <cuda.h>
#include <nvrtc.h>
#include <atomic>
#include <thread>
#include <future>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <vector>
#include <cstring>
#include <type_traits>
namespace swegca::vrs {
using namespace architecture::kernel;
namespace {
void ck(CUresult r){if(r!=CUDA_SUCCESS){const char* s=nullptr;cuGetErrorString(r,&s);throw std::runtime_error(s?s:"CUDA error");}}
void nr(nvrtcResult r){if(r!=NVRTC_SUCCESS)throw std::runtime_error(nvrtcGetErrorString(r));}
const char* source=R"(
#include "swegca_architecture/evidence_scalar.hpp"
using namespace swegca::architecture::kernel;
extern "C" __global__ void abi(unsigned long* out){out[0]=sizeof(EvidenceRules);out[1]=sizeof(EvidenceTally);out[2]=sizeof(EvidenceJudgment);}
extern "C" __global__ void decide(const EvidenceRules* rules,const EvidenceTally* tally,EvidenceJudgment* out,unsigned long n){unsigned long i=(unsigned long)blockIdx.x*blockDim.x+threadIdx.x;if(i<n)out[i]=judge_evidence(rules[i],tally[i]);}
)";
std::vector<char> compile(){nvrtcProgram p;const char* bodies[]={embedded::core_platform,embedded::evidence_scalar};const char* names[]={"swegca_architecture/core_platform.hpp","swegca_architecture/evidence_scalar.hpp"};nr(nvrtcCreateProgram(&p,source,"swegca.cu",2,bodies,names));
 const char* options[]={"--std=c++20","--device-as-default-execution-space","--gpu-architecture=compute_89","--fmad=false","--ftz=false","--prec-div=true","--prec-sqrt=true"};auto r=nvrtcCompileProgram(p,7,options);if(r!=NVRTC_SUCCESS){size_t n;nvrtcGetProgramLogSize(p,&n);std::string log(n,'\0');nvrtcGetProgramLog(p,log.data());nvrtcDestroyProgram(&p);throw std::runtime_error(log);}size_t n;nr(nvrtcGetPTXSize(p,&n));std::vector<char> data(n);nr(nvrtcGetPTX(p,data.data()));nr(nvrtcDestroyProgram(&p));return data;}
struct DeviceBuffer{CUdeviceptr p=0;size_t size=0;~DeviceBuffer(){if(p)cuMemFree(p);}void reserve(size_t n){if(n<=size)return;if(p){ck(cuMemFree(p));p=0;size=0;}ck(cuMemAlloc(&p,n));size=n;}};
}
struct GpuEvidence::Impl {
 struct Job {EvidenceRules rules;EvidenceTally tally;std::promise<EvidenceJudgment> result;};
 struct Worker {std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<Job>> queue;bool stop=false;std::exception_ptr failure;std::thread thread;};
 Worker workers[2];std::atomic<unsigned> next{0};std::vector<char> ptx;std::string directory;
 explicit Impl(std::string dir):ptx(compile()),directory(std::move(dir)){
  ck(cuInit(0));int count;ck(cuDeviceGetCount(&count));if(count<2)throw std::runtime_error("two CUDA devices required");std::filesystem::create_directories(directory);
  for(unsigned i=0;i<2;++i)workers[i].thread=std::thread([this,i]{run(i);});
 }
 ~Impl(){for(auto& w:workers){{std::lock_guard l(w.mutex);w.stop=true;}w.cv.notify_all();}for(auto& w:workers)if(w.thread.joinable())w.thread.join();}
 void run(unsigned index){auto& w=workers[index];CUdevice dev=index;CUcontext context=nullptr;CUmodule module=nullptr;std::vector<std::shared_ptr<Job>> batch;
 try{
  ck(cuDevicePrimaryCtxRetain(&context,dev));ck(cuCtxSetCurrent(context));ck(cuModuleLoadData(&module,ptx.data()));CUfunction fn,abi;ck(cuModuleGetFunction(&fn,module,"decide"));ck(cuModuleGetFunction(&abi,module,"abi"));
  DeviceBuffer rules,tallies,output,layout;layout.reserve(3*sizeof(unsigned long));void* a[]={&layout.p};ck(cuLaunchKernel(abi,1,1,1,1,1,1,0,nullptr,a,nullptr));unsigned long sizes[3];ck(cuMemcpyDtoH(sizes,layout.p,sizeof(sizes)));
  if(sizes[0]!=sizeof(EvidenceRules)||sizes[1]!=sizeof(EvidenceTally)||sizes[2]!=sizeof(EvidenceJudgment))throw std::runtime_error("GPU scalar ABI mismatch");
  CUevent begin,end;ck(cuEventCreate(&begin,0));ck(cuEventCreate(&end,0));std::uint64_t total_jobs=0,batches=0;double total_ms=0;
  for(;;){
   {std::unique_lock l(w.mutex);w.cv.wait(l,[&]{return w.stop||!w.queue.empty();});if(w.queue.empty()&&w.stop)break;
    // Coalesce actual available work; never repeat judgments to inflate load.
    w.cv.wait_for(l,std::chrono::microseconds(200));size_t free,total;ck(cuMemGetInfo(&free,&total));constexpr size_t reserve=512ULL<<20;
    if(free<=reserve)throw std::runtime_error("GPU has no available working memory");
    const size_t max_jobs=(free-reserve)/(sizeof(EvidenceRules)+sizeof(EvidenceTally)+sizeof(EvidenceJudgment));
    while(!w.queue.empty()&&batch.size()<max_jobs){batch.push_back(w.queue.front());w.queue.pop_front();}
   }
   std::vector<EvidenceRules> r;std::vector<EvidenceTally> t;r.reserve(batch.size());t.reserve(batch.size());for(auto& job:batch){r.push_back(job->rules);t.push_back(job->tally);}std::vector<EvidenceJudgment> out(batch.size());
   rules.reserve(r.size()*sizeof(r[0]));tallies.reserve(t.size()*sizeof(t[0]));output.reserve(out.size()*sizeof(out[0]));
   ck(cuMemcpyHtoD(rules.p,r.data(),r.size()*sizeof(r[0])));ck(cuMemcpyHtoD(tallies.p,t.data(),t.size()*sizeof(t[0])));unsigned long n=batch.size();void* args[]={&rules.p,&tallies.p,&output.p,&n};ck(cuEventRecord(begin,nullptr));
   ck(cuLaunchKernel(fn,(n+255)/256,1,1,256,1,1,0,nullptr,args,nullptr));ck(cuEventRecord(end,nullptr));ck(cuEventSynchronize(end));float ms;ck(cuEventElapsedTime(&ms,begin,end));ck(cuMemcpyDtoH(out.data(),output.p,out.size()*sizeof(out[0])));
   total_ms+=ms;total_jobs+=n;++batches;
   auto path=std::filesystem::path(directory)/("gpu"+std::to_string(index)+".json");auto tmp=path;tmp+=".tmp";{std::ofstream f(tmp);f<<"{\"gpu\":"<<index<<",\"core\":\"judge_evidence\",\"judgments\":"<<total_jobs<<",\"batches\":"<<batches<<",\"kernel_ms\":"<<total_ms<<",\"allocated_bytes\":"<<rules.size+tallies.size+output.size<<",\"workload_padding\":false}\n";}std::filesystem::rename(tmp,path);
   for(size_t i=0;i<batch.size();++i)batch[i]->result.set_value(out[i]);batch.clear();
  }
  cuEventDestroy(begin);cuEventDestroy(end);
 }catch(...){auto error=std::current_exception();for(auto& j:batch)j->result.set_exception(error);std::lock_guard l(w.mutex);w.failure=error;for(auto& j:w.queue)j->result.set_exception(error);w.queue.clear();}
 if(module)cuModuleUnload(module);if(context)cuDevicePrimaryCtxRelease(dev);
 }
 EvidenceJudgment judge(const EvidenceRules& r,const EvidenceTally& t){auto job=std::make_shared<Job>(Job{r,t,{}});auto future=job->result.get_future();auto& w=workers[next.fetch_add(1)%2];{std::lock_guard l(w.mutex);if(w.failure)std::rethrow_exception(w.failure);w.queue.push_back(job);}w.cv.notify_one();return future.get();}
};
static_assert(std::is_trivially_copyable_v<EvidenceRules>&&std::is_trivially_copyable_v<EvidenceTally>&&std::is_trivially_copyable_v<EvidenceJudgment>);
GpuEvidence::GpuEvidence(const std::string& dir):impl_(std::make_unique<Impl>(dir)){}
GpuEvidence::~GpuEvidence()=default;
EvidenceJudgment GpuEvidence::judge(const EvidenceRules& r,const EvidenceTally& t){return impl_->judge(r,t);}
}
