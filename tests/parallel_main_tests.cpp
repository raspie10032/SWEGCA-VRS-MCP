#include "vrs/persistent_main_graph.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <pthread.h>
#include <cerrno>
#include <dlfcn.h>
#include <unistd.h>
#include <chrono>
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
 std::atomic<bool> hold{false},entered{false};
 const std::thread::id owner=std::this_thread::get_id();
private:
 void* do_allocate(std::size_t n,std::size_t alignment)override{
  const bool worker=std::this_thread::get_id()!=owner;if(worker)saw_worker=true;
  if(worker&&hold.load()) {entered=true;while(hold.load())hold.wait(true);}
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
  for(unsigned n=0;n<8;++n){EvidenceObservation v;v.hypothesis=id(connection);v.source=id(base+n);v.context=id(base+100+n);v.producer=id(base+200+n);v.outcome=outcome;
   (void)runtime.observe(id(connection),{n,0,name,"experiment","text/plain",{}},v,7,0);
  }
 }
 runtime.end();runtime.publish_originals();
}
void equal(const MainGraph& serial,const MainGraph& parallel){
 CHECK(serial.generation()==parallel.generation());CHECK(serial.source_count()==parallel.source_count());
 for(unsigned n=10;n<13;++n){
  const auto* a=serial.find(id(n));const auto* b=parallel.find(id(n));CHECK(a&&b);
  CHECK(a->strength()==b->strength());CHECK(a->revision()==b->revision());CHECK(a->experiences().size()==b->experiences().size());
  CHECK(refinement_digest(*serial.refinement(id(n)))==refinement_digest(*parallel.refinement(id(n))));
  for(std::size_t i=0;i<a->experiences().size();++i)CHECK(a->experiences()[i].original()==b->experiences()[i].original());
 }
}
class Resolver:public MainSourceResolver {
public:const SessionRuntime& a;const SessionRuntime& b;
 Resolver(const SessionRuntime& x,const SessionRuntime& y):a(x),b(y){}
 const SessionRuntime& resolve(const DigestBytes& value)override{if(value==id(1))return a;if(value==id(2))return b;throw std::runtime_error("unknown source");}
};
int main(){
 auto pattern=(std::filesystem::temp_directory_path()/"swegca-parallel-main-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));const std::filesystem::path root(pattern);
 MemoryBudget memory(64<<20);WorkerMemory upstream;MemoryBudget parallel_memory(64<<20,&upstream);EvidencePolicy policy;policy.axis_count=1;
 {
  auto a_store=SessionStore::create(root,id(1),"a",65536,memory);SessionRuntime a(a_store,memory,8192);fill(a,"a",100,EvidenceOutcome::support,policy);
  auto b_store=SessionStore::create(root,id(2),"b",65536,memory);SessionRuntime b(b_store,memory,8192);fill(b,"b",1000,EvidenceOutcome::refute,policy);
  {
   // Request three slots but fail the second spawn: only caller + one worker
   // can run, and the started worker must join before candidate destruction.
   MainGraph partial(parallel_memory,1,policy,3);creates_left=1;bool failed=false;
   try{(void)partial.merge(a,31,0);}catch(const std::system_error&){failed=true;}
   CHECK(failed&&creates_left==-1);CHECK(partial.generation()==0&&parallel_memory.used()==0);
  }
  MainGraph serial(memory,1,policy,1),parallel(parallel_memory,1,policy,2);
  unsigned failures=0;bool completed=false;
  for(unsigned point=0;point<3000;++point){
   upstream.remaining=point;
   try{CHECK(parallel.merge(a,31,0));completed=true;}
   catch(const std::bad_alloc&){++failures;CHECK(parallel.generation()==0&&parallel.source_count()==0);CHECK(parallel.find(id(10))==nullptr);CHECK(parallel_memory.used()==0);}
   upstream.remaining=SIZE_MAX;if(completed)break;
  }
  CHECK(completed&&failures>0);CHECK(upstream.saw_worker&&upstream.worker_failures>0);
  CHECK(serial.merge(a,31,0));equal(serial,parallel);
  for(unsigned identity=10;identity<13;++identity){
   for(std::size_t n=0;n<8;++n){
    const auto* source=&a.find(id(identity))->state().experiences()[n];
    CHECK((&serial.find(id(identity))->experiences()[n]==source)==(n<7));
    CHECK(&parallel.find(id(identity))->experiences()[n]!=source);
   }
  }
  {
   MainGraph shared_parallel(memory,1,policy,2);
   CHECK(shared_parallel.merge(a,31,0));equal(serial,shared_parallel);
   for(unsigned identity=10;identity<13;++identity)
    CHECK(&shared_parallel.find(id(identity))->experiences()[0]==&a.find(id(identity))->state().experiences()[0]);
  }
  CHECK(serial.merge(b,37,0)&&parallel.merge(b,37,0));equal(serial,parallel);
  CHECK(parallel.replay(id(10),8).location()==b.find(id(10))->state().experiences()[0].original());
  CHECK(!parallel.merge(b,99,1));equal(serial,parallel);
  {
   // Preparation may run alongside real Recall/Replay and a different active
   // temporary session. Hold a worker allocation to make overlap deterministic.
   auto durable=PersistentMainGraph::create(root/"prepared",id(78),parallel_memory,1,policy,1024,2);
   CHECK(durable.merge(a,31,0));const auto before=durable.head();
   auto query_store=SessionStore::create(root,id(4),"query",65536,memory);
   SessionRuntime query(query_store,memory,8192);ExperienceRouter router(query,memory);router.mount_main(durable);
   std::optional<MainGraph::PreparedMerge> batch;std::exception_ptr failure;
   upstream.hold=true;upstream.entered=false;
   std::jthread worker([&]{try{batch.emplace(durable.prepare_merge(b,37,0));}catch(...){failure=std::current_exception();}});
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
   while(!upstream.entered.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
   CHECK(upstream.entered.load());
   for(unsigned iteration=0;iteration<10;++iteration) {
    auto recalled=router.input("text/plain",{});
    CHECK(!recalled.temporary()&&recalled.matches().size()==24);
    CHECK(router.replay(recalled,0).location()==a.find(id(10))->state().experiences()[0].original());
    CHECK(durable.head()==before&&durable.graph().generation()==1);
   }
   (void)query.retain_input({0,0,"query","user","text/new",{}},1,policy,7,0);
   CHECK(router.input("text/new",{}).temporary());
   upstream.hold=false;upstream.hold.notify_all();worker.join();
   if(failure)std::rethrow_exception(failure);
   CHECK(batch.has_value()&&durable.head()==before);
   // Another graph cannot install a batch; rejection must leave it usable by
   // its real owner. Moving invalidates the source handle.
   bool rejected=false;try{(void)parallel.commit_merge(std::move(*batch));}catch(const std::logic_error&){rejected=true;}CHECK(rejected);
   auto stale=durable.prepare_merge(b,37,0);
   auto moved=std::move(*batch);
   rejected=false;try{(void)durable.commit_merge(std::move(*batch));}catch(const std::logic_error&){rejected=true;}CHECK(rejected);
   CHECK(durable.commit_merge(std::move(moved)));equal(serial,durable.graph());
   rejected=false;try{(void)durable.commit_merge(std::move(moved));}catch(const std::logic_error&){rejected=true;}CHECK(rejected);
   rejected=false;try{(void)durable.commit_merge(std::move(stale));}catch(const std::logic_error&){rejected=true;}CHECK(rejected);
   equal(serial,durable.graph());
   auto duplicate=durable.prepare_merge(b,99,1);
   CHECK(!durable.commit_merge(std::move(duplicate)));
   rejected=false;try{(void)durable.commit_merge(std::move(duplicate));}catch(const std::logic_error&){rejected=true;}CHECK(rejected);
   router.mount_main(durable);
   const auto recalled=router.input("text/plain",{});CHECK(!recalled.temporary()&&recalled.matches().size()==48);
   CHECK(router.replay(recalled,8).location()==b.find(id(10))->state().experiences()[0].original());
  }
  ExperienceLocation head;
  {
   auto durable=PersistentMainGraph::create(root/"graph",id(77),parallel_memory,1,policy,1024,2);
   CHECK(durable.merge(a,31,0)&&durable.merge(b,37,0));equal(serial,durable.graph());head=durable.head();
  }
  Resolver resolver(a,b);
  {
   auto durable=PersistentMainGraph::open(root/"graph",id(77),parallel_memory,1,policy,resolver,1);
   CHECK(durable.head()==head);equal(serial,durable.graph());
  }
  {
   auto durable=PersistentMainGraph::open(root/"graph",id(77),parallel_memory,1,policy,resolver,2);
   CHECK(durable.head()==head);equal(serial,durable.graph());
  }
  std::printf("parallel Main allocation failure points: %u; worker failures: %u\n",failures,upstream.worker_failures.load());
 }
 CHECK(memory.used()==0&&parallel_memory.used()==0);std::filesystem::remove_all(root);
 std::printf("parallel Main tests: %u checks passed\n",checks);
}
