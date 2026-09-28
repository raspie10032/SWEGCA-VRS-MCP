#include "vrs/collision_dispatch.hpp"
#include "gpu_core_source.hpp"
#include <cuda.h>
#include <nvrtc.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <map>
namespace swegca::vrs {
namespace {
void check(CUresult r){if(r!=CUDA_SUCCESS){const char* s=nullptr;cuGetErrorString(r,&s);throw std::runtime_error(s?s:"CUDA failure");}}
void check(nvrtcResult r){if(r!=NVRTC_SUCCESS)throw std::runtime_error(nvrtcGetErrorString(r));}
std::vector<char> compile(){
 const char* source=R"(
#include "swegca_architecture/association_scalar.hpp"
using namespace swegca::architecture::kernel;
extern "C" __global__ void layout(unsigned long* p){p[0]=sizeof(AssociationEvidence);p[1]=sizeof(AssociationJudgment);}
extern "C" __global__ void collision(const AssociationEvidence* in,AssociationJudgment* out,unsigned long n){
 for(unsigned long i=(unsigned long)blockIdx.x*blockDim.x+threadIdx.x;i<n;i+=(unsigned long)gridDim.x*blockDim.x)out[i]=judge_association(in[i]);
})";
 const char* bodies[]={embedded::core_platform,embedded::evidence_scalar,embedded::association_scalar};
 const char* names[]={"swegca_architecture/core_platform.hpp","swegca_architecture/evidence_scalar.hpp","swegca_architecture/association_scalar.hpp"};
 nvrtcProgram p;check(nvrtcCreateProgram(&p,source,"collision.cu",3,bodies,names));
 const char* opts[]={"--std=c++20","--device-as-default-execution-space","--gpu-architecture=compute_89","--fmad=false","--ftz=false"};
 auto result=nvrtcCompileProgram(p,5,opts);if(result!=NVRTC_SUCCESS){size_t n;nvrtcGetProgramLogSize(p,&n);std::string log(n,'\0');nvrtcGetProgramLog(p,log.data());nvrtcDestroyProgram(&p);throw std::runtime_error(log);}
 size_t n;check(nvrtcGetPTXSize(p,&n));std::vector<char> out(n);check(nvrtcGetPTX(p,out.data()));check(nvrtcDestroyProgram(&p));return out;
}
}
struct CollisionDispatch::Impl {
 struct Batch {std::span<const Evidence> in;std::span<Judgment> out;size_t remaining;std::exception_ptr error;std::map<size_t,size_t> finished;};
 struct Range {std::shared_ptr<Batch> batch;size_t first,last;};
 std::mutex mutex;std::condition_variable ready,completed;std::deque<Range> queue;bool stop=false;
 std::vector<std::thread> workers;std::vector<char> ptx;std::string directory;
 unsigned initialized=0;std::exception_ptr startup_error;
 static constexpr size_t cpu_chunk=4096,gpu_chunk=262144;
 Impl(std::string dir):ptx(compile()),directory(std::move(dir)){
  check(cuInit(0));int n;check(cuDeviceGetCount(&n));if(n<2)throw std::runtime_error("two GPUs required");std::filesystem::create_directories(directory);
  try{for(unsigned i=0;i<12;++i)workers.emplace_back([this,i]{run(i);});}
  catch(...){{std::lock_guard l(mutex);stop=true;}ready.notify_all();for(auto& t:workers)t.join();throw;}
  std::unique_lock l(mutex);completed.wait(l,[&]{return initialized==12;});
  if(startup_error){auto error=startup_error;stop=true;l.unlock();ready.notify_all();for(auto& t:workers)t.join();std::rethrow_exception(error);}
 }
 ~Impl(){{std::lock_guard l(mutex);stop=true;}ready.notify_all();for(auto& t:workers)t.join();}
 void run(unsigned index){
  const bool gpu=index>=10;CUcontext context=nullptr;CUmodule module=nullptr;CUdeviceptr input=0,output=0;CUfunction fn=nullptr;
  std::exception_ptr device_error;size_t total=0,ranges=0;
  try{if(gpu){check(cuDevicePrimaryCtxRetain(&context,index-10));check(cuCtxSetCurrent(context));check(cuModuleLoadData(&module,ptx.data()));check(cuModuleGetFunction(&fn,module,"collision"));
    check(cuMemAlloc(&input,gpu_chunk*sizeof(Evidence)));check(cuMemAlloc(&output,gpu_chunk*sizeof(Judgment)));
    CUfunction layout;check(cuModuleGetFunction(&layout,module,"layout"));void* args[]={&input};check(cuLaunchKernel(layout,1,1,1,1,1,1,0,nullptr,args,nullptr));unsigned long sizes[2];check(cuMemcpyDtoH(sizes,input,sizeof(sizes)));
    if(sizes[0]!=sizeof(Evidence)||sizes[1]!=sizeof(Judgment))throw std::runtime_error("association GPU ABI mismatch");
   }}catch(...){device_error=std::current_exception();}
  {std::lock_guard l(mutex);++initialized;if(device_error)startup_error=device_error;}completed.notify_all();
  if(!device_error)for(;;){Range range;
   {std::unique_lock l(mutex);ready.wait(l,[&]{return stop||!queue.empty();});if(stop&&queue.empty())break;
    auto& front=queue.front();range={front.batch,front.first,std::min(front.last,front.first+(gpu?gpu_chunk:cpu_chunk))};front.first=range.last;if(front.first==front.last)queue.pop_front();
   }
   std::exception_ptr error;
   try{
    if(device_error)std::rethrow_exception(device_error);
    const auto n=range.last-range.first;
    if(gpu){check(cuMemcpyHtoD(input,range.batch->in.data()+range.first,n*sizeof(Evidence)));unsigned long count=n;void* args[]={&input,&output,&count};check(cuLaunchKernel(fn,(n+255)/256,1,1,256,1,1,0,nullptr,args,nullptr));check(cuMemcpyDtoH(range.batch->out.data()+range.first,output,n*sizeof(Judgment)));}
    else for(size_t i=range.first;i<range.last;++i)range.batch->out[i]=architecture::kernel::judge_association(range.batch->in[i]);
    total+=n;++ranges;
   }catch(...){error=std::current_exception();if(gpu)device_error=error;}
   // Completing a range releases the worker immediately to pull the next one.
   {std::lock_guard l(mutex);if(error&&!range.batch->error)range.batch->error=error;range.batch->remaining-=range.last-range.first;range.batch->finished.emplace(range.first,range.last);}completed.notify_all();
   if((ranges&255)==1){auto path=std::filesystem::path(directory)/("worker-"+std::to_string(index)+".json");auto tmp=path;tmp+=".tmp";std::ofstream f(tmp);f<<"{\"worker\":"<<index<<",\"gpu\":"<<(gpu?"true":"false")<<",\"judgments\":"<<total<<",\"ranges\":"<<ranges<<"}\n";f.close();std::filesystem::rename(tmp,path);}
  }
  if(input)cuMemFree(input);if(output)cuMemFree(output);if(module)cuModuleUnload(module);if(context)cuDevicePrimaryCtxRelease(index-10);
  auto path=std::filesystem::path(directory)/("worker-"+std::to_string(index)+".json");std::ofstream f(path);f<<"{\"worker\":"<<index<<",\"gpu\":"<<(gpu?"true":"false")<<",\"judgments\":"<<total<<",\"ranges\":"<<ranges<<"}\n";
 }
 void judge(std::span<const Evidence> in,std::span<Judgment> out,Commit commit){
  if(in.size()!=out.size())throw std::invalid_argument("collision output size");if(in.empty())return;
  auto batch=std::make_shared<Batch>(Batch{in,out,in.size(),{}, {}});
  std::unique_lock l(mutex);if(stop)throw std::runtime_error("dispatcher stopped");queue.push_back({batch,0,in.size()});ready.notify_all();
  size_t next=0;std::exception_ptr commit_error;
  while(next<in.size()){
   completed.wait(l,[&]{return batch->finished.contains(next);});
   const auto end=batch->finished.at(next);batch->finished.erase(next);
   // One owner applies completed ranges in submission order, while workers
   // continue pulling work. It never holds the dispatch lock while applying.
   if(!batch->error&&!commit_error&&commit){l.unlock();try{commit(next,end);}catch(...){commit_error=std::current_exception();}l.lock();}
   next=end;
  }
  if(batch->error)std::rethrow_exception(batch->error);if(commit_error)std::rethrow_exception(commit_error);
 }
};
CollisionDispatch::CollisionDispatch(const std::string& dir):impl_(std::make_unique<Impl>(dir)){}
CollisionDispatch::~CollisionDispatch()=default;
void CollisionDispatch::judge(std::span<const Evidence> in,std::span<Judgment> out,Commit commit){impl_->judge(in,out,std::move(commit));}
}
