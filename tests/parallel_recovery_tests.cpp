#include "vrs/session_runtime.hpp"
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <pthread.h>
#include <cerrno>
#include <dlfcn.h>
#include <unistd.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
static unsigned checks=0;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
static int creates_left=-1;
extern "C" int pthread_create(pthread_t* thread,const pthread_attr_t* attributes,void*(*entry)(void*),void* argument) noexcept {
 if(creates_left==0){creates_left=-1;return EAGAIN;}if(creates_left>0)--creates_left;
 using Create=int(*)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
 static auto real=reinterpret_cast<Create>(::dlsym(RTLD_NEXT,"pthread_create"));
 if(!real)std::abort();
 return real(thread,attributes,entry,argument);
}
DigestBytes id(unsigned n){DigestBytes d{};for(unsigned i=0;i<4;++i)d[i]=std::byte(n>>(8*i));return d;}
class WorkerMemory final:public std::pmr::memory_resource {
public:
 std::atomic<std::size_t> remaining{SIZE_MAX};std::atomic<bool> saw_worker{false};std::atomic<unsigned> worker_failures{0};
 const std::thread::id owner=std::this_thread::get_id();
private:
 void* do_allocate(std::size_t n,std::size_t alignment)override{
  const bool worker=std::this_thread::get_id()!=owner;if(worker)saw_worker=true;
  auto left=remaining.load();
  while(left!=SIZE_MAX){if(left==0){if(worker)++worker_failures;throw std::bad_alloc();}if(remaining.compare_exchange_weak(left,left-1))break;}
  return std::pmr::new_delete_resource()->allocate(n,alignment);
 }
 void do_deallocate(void* p,std::size_t n,std::size_t alignment)override{std::pmr::new_delete_resource()->deallocate(p,n,alignment);}
 bool do_is_equal(const std::pmr::memory_resource& other)const noexcept override{return this==&other;}
};
void fill(SessionRuntime& runtime,std::string_view name,unsigned base,EvidenceOutcome outcome,const EvidencePolicy& policy){
 for(unsigned connection=10;connection<13;++connection){
  runtime.define_connection(id(connection),1,policy);
  for(unsigned n=0;n<8;++n){EvidenceObservation v;v.hypothesis=id(connection);v.source=id(base+n);v.context=id(base+100+n);v.producer=id(base+200+n);v.outcome=connection==10?outcome:(connection==11?EvidenceOutcome::refute:EvidenceOutcome::insufficient);
   (void)runtime.observe(id(connection),{n,0,name,"experiment","text/plain",{}},v,7,0);
  }
 }
 runtime.end();runtime.publish_originals();
}
static std::atomic<unsigned> writes{0};
static std::atomic<bool> fail_worker_read{false};
static const auto owner_thread=std::this_thread::get_id();
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t pos){
 if(std::this_thread::get_id()!=owner_thread&&fail_worker_read.exchange(false)){errno=EIO;return -1;}
 return __real_pread(fd,data,n,pos);
}
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t pos){++writes;return __real_pwrite(fd,data,n,pos);}
int main(){
 auto pattern=(std::filesystem::temp_directory_path()/"swegca-parallel-recovery-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));
 const std::filesystem::path root(pattern);MemoryBudget memory(64<<20);WorkerMemory upstream;MemoryBudget recovered_memory(64<<20,&upstream);
 EvidencePolicy policy;policy.axis_count=1;
 {
  auto store=SessionStore::create(root,id(1),"source",65536,memory);
  std::array<ConnectionHead,3> expected;std::array<ExperienceLocation,24> originals;
  {
   SessionRuntime initial(store,memory,8192);fill(initial,"source",100,EvidenceOutcome::support,policy);
   for(unsigned c=0;c<3;++c){const auto* connection=initial.find(id(c+10));expected[c]=connection->snapshot();
    for(unsigned n=0;n<8;++n)originals[c*8+n]=connection->state().experiences()[n].original();}
  }
  CHECK(expected[0].strength>1&&expected[1].strength<1&&expected[2].strength==1);
  writes=0;
  const auto verify=[&](const SessionRuntime& state){
   for(unsigned c=0;c<3;++c){const auto* connection=state.find(id(c+10));CHECK(connection);
    const auto head=connection->snapshot();CHECK(head.record==expected[c].record);CHECK(head.revision==expected[c].revision);
    CHECK(head.strength==expected[c].strength&&head.observations==8);
    for(unsigned n=0;n<8;++n)CHECK(connection->state().experiences()[n].original()==originals[c*8+n]);
   }
  };
  {SessionRuntime serial(store,recovered_memory,8192,1);verify(serial);}
  CHECK(recovered_memory.used()==0);
  creates_left=1;bool failed=false;
  try{SessionRuntime partial(store,recovered_memory,8192,3);}catch(const std::system_error&){failed=true;}
  CHECK(failed&&creates_left==-1&&recovered_memory.used()==0);
  fail_worker_read=true;failed=false;
  try{SessionRuntime broken_read(store,recovered_memory,8192,2);}catch(const std::system_error&){failed=true;}
  CHECK(failed&&!fail_worker_read&&recovered_memory.used()==0);
  unsigned failures=0;bool completed=false;
  for(unsigned point=0;point<5000;++point){
   upstream.remaining=point;
   try{SessionRuntime parallel(store,recovered_memory,8192,2);upstream.remaining=SIZE_MAX;verify(parallel);
    ExperienceRouter route(parallel,recovered_memory);auto recalled=route.input("text/plain",{});
    CHECK(recalled.matches().size()==24);
    for(unsigned n=0;n<24;++n)CHECK(recalled.matches()[n].original==originals[n]);
    CHECK(route.replay(recalled,23).location()==originals[23]);completed=true;}
   catch(const std::bad_alloc&){++failures;}
   upstream.remaining=SIZE_MAX;CHECK(recovered_memory.used()==0);if(completed)break;
  }
  CHECK(completed&&failures>0&&upstream.saw_worker&&upstream.worker_failures>0);CHECK(writes==0);
  std::printf("Recovery allocation failure points: %u; worker failures: %u\n",failures,upstream.worker_failures.load());
 }
 CHECK(memory.used()==0&&recovered_memory.used()==0);std::filesystem::remove_all(root);
 std::printf("Parallel recovery tests: %u checks passed\n",checks);
}
