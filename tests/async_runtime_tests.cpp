#include "vrs/runtime.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
using namespace swegca::architecture;
using namespace swegca::vrs;
static unsigned checks=0;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
template<class E,class F>void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
static bool fail_spawn=false;
extern "C" int pthread_create(pthread_t* t,const pthread_attr_t* a,void*(*f)(void*),void* p) noexcept {
 if(fail_spawn){fail_spawn=false;return EAGAIN;}
 using Create=int(*)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
 static auto real=reinterpret_cast<Create>(::dlsym(RTLD_NEXT,"pthread_create"));if(!real)std::abort();return real(t,a,f,p);
}
class WorkerMemory final:public std::pmr::memory_resource {
public:
 const std::thread::id owner=std::this_thread::get_id();
 std::atomic<bool> hold{false},entered{false},fail{false};
 bool fail_owner=false;
 void release(){hold=false;hold.notify_all();}
private:
 void* do_allocate(std::size_t n,std::size_t a)override{
  if(std::this_thread::get_id()!=owner){
   if(hold.load()){entered=true;while(hold.load())hold.wait(true);}
   if(fail.load())throw std::bad_alloc();
  }
  else if(fail_owner)throw std::bad_alloc();
  return std::pmr::new_delete_resource()->allocate(n,a);
 }
 void do_deallocate(void* p,std::size_t n,std::size_t a)override{std::pmr::new_delete_resource()->deallocate(p,n,a);}
 bool do_is_equal(const std::pmr::memory_resource& o)const noexcept override{return this==&o;}
};
DigestBytes id(unsigned n){DigestBytes d{};d[0]=std::byte(n);return d;}
std::size_t finish(Runtime& runtime){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(std::chrono::steady_clock::now()<deadline){if(auto result=runtime.poll_work())return *result;std::this_thread::yield();}
 throw std::runtime_error("background work did not finish");
}
int main(){
 auto pattern=(std::filesystem::temp_directory_path()/"swegca-async-runtime-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));
 const std::filesystem::path root(pattern);WorkerMemory upstream;MemoryBudget memory(64<<20,&upstream);
 EvidencePolicy policy;policy.axis_count=1;RuntimeConfig config{id(99),policy,1,65536,1024,8192,2};
 const std::string text="ended session original";const auto bytes=std::as_bytes(std::span(text));
 ExperienceLocation original,active_original;
 {
  auto runtime=Runtime::create(root,config,memory);
  CHECK(!runtime.schedule_work(7,0));CHECK(runtime.poll_work()==0);
  runtime.start_session(id(200),"empty");runtime.end_session();
  runtime.start_session(id(2),"ended");
  original=runtime.retain({0,0,"ended","user","text/plain",bytes},7,0).original;
  CHECK(runtime.main().graph().generation()==0);runtime.end_session();
  // Empty sources must not hide later nonempty work.
  CHECK(runtime.schedule_work(7,0));CHECK(finish(runtime)==1);
  runtime.start_session(id(3),"to-prepare");
  (void)runtime.retain({0,0,"to-prepare","user","text/plain",bytes},7,0);runtime.end_session();
  runtime.start_session(id(4),"active");
  active_original=runtime.retain({0,0,"active","assistant","text/active",bytes},7,0).original;
  upstream.hold=true;upstream.entered=false;CHECK(runtime.schedule_work(7,0));
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!upstream.entered.load()&&std::chrono::steady_clock::now()<deadline){(void)runtime.poll_work();std::this_thread::yield();}
  CHECK(upstream.entered.load());
  for(unsigned n=0;n<10;++n){
   CHECK(!runtime.poll_work().has_value());
   auto main=runtime.input("text/plain",bytes);CHECK(!main.temporary()&&main.matches().size()==1);
   CHECK(runtime.replay(main,0).location()==original);
   auto temporary=runtime.input("text/active",bytes);CHECK(temporary.temporary());
  }
  CHECK(runtime.replay(runtime.input("text/active",bytes),0).location()==active_original);
  throws<std::logic_error>([&]{(void)runtime.work(7,0);});
  throws<std::logic_error>([&]{(void)runtime.schedule_work(7,0);});
  (void)runtime.retain({1,0,"active","tool","text/active",bytes},7,0);
  // Another session end evicts caches, but must preserve the worker's source.
  runtime.end_session();runtime.start_session(id(5),"next");
  CHECK(runtime.main().graph().generation()==1);
  upstream.release();CHECK(finish(runtime)==2); // includes the explicit end while preparation was held
  CHECK(runtime.main().graph().generation()==3);
  auto recalled=runtime.input("text/plain",bytes);CHECK(!recalled.temporary()&&recalled.matches().size()==2);
  CHECK(runtime.replay(recalled,0).location()==original);
  CHECK(!runtime.main().graph().has_source(id(5))); // the new live session stays temporary
  CHECK(runtime.schedule_work(7,0));CHECK(finish(runtime)==0); // only the empty source remains
  CHECK(runtime.main().graph().generation()==3);
  CHECK(runtime.input("text/active",bytes).matches().size()==2);
  (void)runtime.retain({0,0,"next","user","text/last",bytes},7,0);runtime.end_session();
  fail_spawn=true;throws<std::system_error>([&]{(void)runtime.schedule_work(7,0);});
  CHECK(!fail_spawn&&runtime.main().usable());
  upstream.fail=true;CHECK(runtime.schedule_work(7,0));
  throws<std::bad_alloc>([&]{(void)finish(runtime);});upstream.fail=false;
  CHECK(runtime.main().graph().generation()==3);
  CHECK(runtime.schedule_work(7,0));CHECK(finish(runtime)==1);
  CHECK(runtime.main().graph().generation()==4);
  runtime.start_session(id(6),"unfinished");
  (void)runtime.retain({0,0,"unfinished","user","text/unfinished",bytes},7,0);
 }
 CHECK(memory.used()==0);
 {
  auto runtime=Runtime::open(root,config,memory);CHECK(runtime.main().graph().generation()==4);
  runtime.resume_session(id(6));CHECK(runtime.input("text/unfinished",bytes).temporary());
  CHECK(runtime.schedule_work(7,0));CHECK(finish(runtime)==0);
  runtime.end_session();CHECK(runtime.schedule_work(7,0));
  runtime.start_session(id(7),"queued-before-exit");
  (void)runtime.retain({0,0,"queued-before-exit","user","text/queued",bytes},7,0);
  runtime.end_session();
  // Destruction joins and discards preparation, never publishes it or ends a session.
 }
 CHECK(memory.used()==0);
 {
  auto runtime=Runtime::open(root,config,memory);CHECK(runtime.main().graph().generation()==4);
  CHECK(runtime.schedule_work(7,0));CHECK(finish(runtime)==2);
  CHECK(runtime.main().graph().generation()==6);
 }
 CHECK(memory.used()==0);
 {
  const auto admission=root/"admission";std::filesystem::create_directory(admission);
  auto runtime=Runtime::create(admission,config,memory);
  runtime.start_session(id(80),"first");
  (void)runtime.retain({0,0,"first","user","text/plain",bytes},7,0);runtime.end_session();
  runtime.start_session(id(81),"later");
  (void)runtime.retain({0,0,"later","user","text/plain",bytes},7,0);
  upstream.hold=true;upstream.entered=false;CHECK(runtime.schedule_work(7,0));
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!upstream.entered.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(upstream.entered.load());
  upstream.fail_owner=true;
  throws<std::bad_alloc>([&]{runtime.end_session();});
  upstream.fail_owner=false;
  CHECK(runtime.has_session()&&runtime.session().usable());
  CHECK(runtime.session().phase()==kernel::SessionPhase::active);
  CHECK(runtime.main().graph().generation()==0);
  runtime.end_session(); // retry reserves first, then publishes and queues
  CHECK(!runtime.has_session()&&runtime.main().graph().generation()==0);
  upstream.release();CHECK(finish(runtime)==2);
  CHECK(runtime.main().graph().generation()==2);
 }
 CHECK(memory.used()==0);std::filesystem::remove_all(root);
 std::printf("async runtime tests: %u checks passed\n",checks);
}
