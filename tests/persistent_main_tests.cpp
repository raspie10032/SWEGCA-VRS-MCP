#include "vrs/persistent_main_graph.hpp"
#include "swegca_architecture/input_cue.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs=std::filesystem;
static unsigned checks=0;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
template<class E,class F>void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
static unsigned reads=0;
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t pos){++reads;return __real_pread(fd,data,n,pos);}
static int writes_left=-1;static bool fail_sync=false;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t pos){
 if(writes_left==0){writes_left=-1;errno=ENOSPC;return -1;}if(writes_left>0)--writes_left;return __real_pwrite(fd,data,n,pos);
}
extern "C" int __real_fdatasync(int);
extern "C" int __wrap_fdatasync(int fd){if(fail_sync){fail_sync=false;errno=EIO;return -1;}return __real_fdatasync(fd);}
DigestBytes id(unsigned n){DigestBytes d{};for(unsigned i=0;i<4;++i)d[i]=std::byte(n>>(8*i));return d;}
class FailingMemory final:public std::pmr::memory_resource {
public: std::size_t remaining=std::numeric_limits<std::size_t>::max();
 std::size_t request_limit=std::numeric_limits<std::size_t>::max(),largest_request=0;
private:
 void* do_allocate(std::size_t n,std::size_t a)override{largest_request=std::max(largest_request,n);if(!remaining||n>request_limit)throw std::bad_alloc();--remaining;return std::pmr::new_delete_resource()->allocate(n,a);}
 void do_deallocate(void* p,std::size_t n,std::size_t a)override{std::pmr::new_delete_resource()->deallocate(p,n,a);}
 bool do_is_equal(const std::pmr::memory_resource& other)const noexcept override{return this==&other;}
};
class Resolver:public MainSourceResolver {
public:std::map<DigestBytes,const SessionRuntime*> sources;unsigned calls=0;
 const SessionRuntime& resolve(const DigestBytes& identity)override{++calls;return *sources.at(identity);}
};
void fill(SessionRuntime& runtime,std::string_view name,unsigned base,EvidenceOutcome outcome){
 for(unsigned n=0;n<8;++n){EvidenceObservation v;v.hypothesis=id(10);v.source=id(base+n);v.context=id(base+n+1000);v.producer=id(base+n+2000);v.outcome=outcome;
  (void)runtime.observe(id(10),{n,0,name,"experiment","text/plain",{}},v,7,0);
 }
 runtime.end();runtime.publish_originals();
}
int main(){
 auto pattern=(fs::temp_directory_path()/"swegca-persistent-main-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));const fs::path root(pattern);
 MemoryBudget memory(64<<20);EvidencePolicy policy;policy.axis_count=1;
 {
  auto a_store=SessionStore::create(root,id(1),"a",65536,memory);SessionRuntime a(a_store,memory,8192);a.define_connection(id(10),0.75,policy);fill(a,"a",100,EvidenceOutcome::support);
  auto b_store=SessionStore::create(root,id(2),"b",65536,memory);SessionRuntime b(b_store,memory,8192);b.define_connection(id(10),0.75,policy);fill(b,"b",200,EvidenceOutcome::refute);
  auto c_store=SessionStore::create(root,id(3),"c",65536,memory);SessionRuntime c(c_store,memory,8192);c.define_connection(id(10),0.75,policy);fill(c,"c",300,EvidenceOutcome::insufficient);
  Resolver resolver;resolver.sources={{id(1),&a},{id(2),&b},{id(3),&c}};
  {
   auto extra_store=SessionStore::create(root,id(71),"extra",65536,memory);
   SessionRuntime extra(extra_store,memory,8192);
   for(unsigned connection:{9U,11U}) {
    extra.define_connection(id(connection),1,policy);
    for(unsigned n=0;n<2;++n) {
     EvidenceObservation value;value.hypothesis=id(connection);value.source=id(700+n);
     value.context=id(1700+n);value.producer=id(2700+n);value.outcome=EvidenceOutcome::insufficient;
     (void)extra.observe(id(connection),{n,0,"extra","experiment",n?"text/untouched":"text/plain",{}},value,7,0);
    }
   }
   extra.end();extra.publish_originals();
   auto query_store=SessionStore::create(root,id(72),"query",65536,memory);
   SessionRuntime query(query_store,memory,8192);
   auto indexed=PersistentMainGraph::create(root/"incremental-main",id(73),memory,1,policy,1024);
   CHECK(indexed.merge(extra,1,0));CHECK(indexed.merge(a,2,0));
   FailingMemory index_allocator;MemoryBudget index_memory(1<<20,&index_allocator);
   ExperienceRouter incremental(query,index_memory);incremental.mount_main(indexed);
   const auto untouched=incremental.input("text/untouched",{});
   CHECK(untouched.matches().size()==2);
   const auto prior_direct=incremental.recall(id(9));
   // Skip a generation deliberately; the old observation counts still select
   // exactly the new suffix across both commits.
   CHECK(indexed.merge(b,3,0));CHECK(indexed.merge(c,4,0));
   index_allocator.largest_request=0;index_allocator.request_limit=512;
   index_allocator.remaining=20;
   incremental.mount_main(indexed);
   CHECK(index_allocator.largest_request<=512);
   index_allocator.remaining=std::numeric_limits<std::size_t>::max();
   throws<std::logic_error>([&]{(void)incremental.replay(prior_direct,0,0);});
   index_allocator.request_limit=std::numeric_limits<std::size_t>::max();
   ExperienceRouter rebuilt(query,memory);rebuilt.mount_main(indexed);
   for(const auto media:{"text/plain","text/untouched"}) {
    const auto delta=incremental.input(media,{});const auto complete=rebuilt.input(media,{});
    CHECK(delta.matches().size()==complete.matches().size());
    for(std::size_t i=0;i<delta.matches().size();++i) {
     CHECK(delta.matches()[i].original==complete.matches()[i].original);
     CHECK(delta.matches()[i].recalled.recalled_head.identity==complete.matches()[i].recalled.recalled_head.identity);
     CHECK(delta.matches()[i].recalled.recalled_head.record==indexed.head());
    }
   }
   const auto current_direct=incremental.recall(id(9));
   CHECK(current_direct.at(0).recalled_head.record==indexed.head());
   CHECK(!current_direct.temporary()&&current_direct.size()==1);
   CHECK(incremental.replay(current_direct,0,0).location()==extra.find(id(9))->state().experiences()[0].original());
  }
  const auto path=root/"main-graph";const auto identity=id(77);ExperienceLocation first_head,last_head;double strength=0;
  {
   auto main=PersistentMainGraph::create(path,identity,memory,1,policy,1024);
   CHECK(main.graph().generation()==0);
   throws<std::system_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,policy,resolver);});
   CHECK(main.merge(a,17,0));first_head=main.head();CHECK(main.graph().generation()==1);
   CHECK(!main.merge(a,18,1)&&main.head()==first_head);
   writes_left=1;throws<std::system_error>([&]{(void)main.merge(b,19,0);});
   CHECK(!main.usable());throws<std::logic_error>([&]{(void)main.graph();});
  }
  const auto old_size=fs::file_size(path/"m-0000000000000000.block");
  {
   auto main=PersistentMainGraph::open(path,identity,memory,1,policy,resolver);
   CHECK(main.graph().generation()==1&&main.head()==first_head);
   CHECK(main.merge(b,19,0));CHECK(main.graph().generation()==2);
   CHECK(fs::file_size(path/"m-0000000000000000.block")==old_size);
   CHECK(fs::exists(path/"m-0000000000000001.block"));
   // Complete record present, but sync did not report success: reopen decides
   // from actual validated storage rather than assuming the old memory state.
   fail_sync=true;throws<std::system_error>([&]{(void)main.merge(c,23,0);});CHECK(!main.usable());
  }
  {
   auto main=PersistentMainGraph::open(path,identity,memory,1,policy,resolver);
   CHECK(main.graph().generation()==3&&main.graph().source_count()==3);
   CHECK(!main.merge(c,999,1));CHECK(main.graph().find(id(10))->experiences().size()==24);
   CHECK(main.graph().replay(id(10),0).location()==a.find(id(10))->state().experiences()[0].original());
   CHECK(main.graph().replay(id(10),8).location()==b.find(id(10))->state().experiences()[0].original());
   strength=main.graph().find(id(10))->strength();last_head=main.head();
  }
  {
   auto main=PersistentMainGraph::open(path,identity,memory,1,policy,resolver);
   CHECK(main.graph().find(id(10))->strength()==strength&&main.head()==last_head);
  }
  auto changed=policy;changed.accept_margin=0.1;
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,changed,resolver);});
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,id(78),memory,1,policy,resolver);});
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,0.5,policy,resolver);});
  Resolver missing;throws<std::out_of_range>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,policy,missing);});
  Resolver wrong;wrong.sources={{id(1),&b},{id(2),&b},{id(3),&c}};
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,policy,wrong);});
  {
   auto active_store=SessionStore::create(root,id(4),"active",65536,memory);
   SessionRuntime active(active_store,memory,8192);
   auto main=PersistentMainGraph::create(root/"query-main",id(88),memory,1,policy,1024);
   CHECK(main.merge(a,17,0));
   FailingMemory failures;MemoryBudget query_memory(1<<20,&failures);
   ExperienceRouter route(active,query_memory);route.mount_main(main);
   // Refreshing an unchanged head allocates nothing.
   failures.remaining=0;route.mount_main(main);
   failures.remaining=std::numeric_limits<std::size_t>::max();
   const auto before=reads;
   auto recalled=route.input("text/plain",{});
   CHECK(reads==before&&!recalled.temporary()&&recalled.matches().size()==8);
   CHECK(recalled.matches()[0].recalled.recalled_head.strength==main.graph().find(id(10))->strength());
   auto original=route.replay(recalled,3);
   CHECK(original.location()==a.find(id(10))->state().experiences()[3].original());
   const auto receipt_baseline=query_memory.used();
   unsigned receipt_failures=0;
   for(std::size_t point=0;point<16;++point){
    failures.remaining=point;bool success=false;
    try{auto probe=route.input("text/other",{});success=true;}
    catch(const std::bad_alloc&){++receipt_failures;}
    failures.remaining=std::numeric_limits<std::size_t>::max();
    CHECK(query_memory.used()==receipt_baseline);
    if(success)break;
   }
   CHECK(receipt_failures==2);
   auto continued=route.input("text/other",{});
   CHECK(continued.key_kind()==FamiliarityKey::continuation&&continued.matches().size()==8);
   CHECK(route.re_evidence(original,9,0).agreement()==ReplayAgreement::insufficient);
   auto old=route.input("text/plain",{});
   CHECK(main.merge(b,19,0));
   CHECK(continued.matches().size()==8);
   for(std::size_t n=0;n<8;++n){
    CHECK(continued.matches()[n].original==a.find(id(10))->state().experiences()[n].original());
    CHECK(continued.matches()[n].recalled.recalled_head.observations==8);
   }
   throws<std::logic_error>([&]{(void)route.replay(continued,0);});
   throws<std::logic_error>([&]{(void)route.input("text/plain",{});});
   throws<std::logic_error>([&]{(void)route.replay(old,0);});
   unsigned failure_points=0;
   for(std::size_t point=0;point<100;++point){
    const auto used=query_memory.used();failures.remaining=point;bool failed=false;
    try{route.mount_main(main);}catch(const std::bad_alloc&){failed=true;}
    failures.remaining=std::numeric_limits<std::size_t>::max();
    if(!failed)break;
    ++failure_points;CHECK(query_memory.used()==used);
    throws<std::logic_error>([&]{(void)route.input("text/plain",{});});
   }
   CHECK(failure_points>3);
   std::printf("Incremental Main index failure points: %u\n",failure_points);
   throws<std::logic_error>([&]{(void)route.replay(old,0);});
   auto joined=route.input("text/plain",{});
   CHECK(joined.matches().size()==16);
   CHECK(joined.matches()[0].recalled.recalled_head.strength==main.graph().find(id(10))->strength());
   CHECK(route.replay(joined,8).location()==b.find(id(10))->state().experiences()[0].original());
   // The earlier Replay keeps its original meaning even after graph nodes have
   // been replaced. Only new temporary observations enter Re-evidence.
   active.define_connection(id(10),1,policy);
   for(unsigned n=0;n<8;++n){
    EvidenceObservation value;value.hypothesis=id(10);value.source=id(900+n);value.context=id(1900+n);value.producer=id(2900+n);value.outcome=EvidenceOutcome::refute;
    (void)active.observe(id(10),{n,0,"active","experiment","text/plain",{}},value,7,0);
   }
   const auto reevaluated=route.re_evidence(original,7,0);
   CHECK(reevaluated.agreement()==ReplayAgreement::contradicts&&reevaluated.current_originals().size()==8);
   auto local=route.input("text/plain",{});
   CHECK(local.temporary()&&local.matches().size()==8);
   CHECK(route.replay(local,0).location()==active.find(id(10))->state().experiences()[0].original());
   CHECK(route.recall(id(10)).temporary());
   throws<std::logic_error>([&]{route.mount_main(a);});
   ExperienceRouter session_route(active,memory);session_route.mount_main(a);
   throws<std::logic_error>([&]{session_route.mount_main(main);});
   auto other_store=SessionStore::create(root,id(5),"other",65536,memory);SessionRuntime other(other_store,memory,8192);
   ExperienceRouter other_route(other,memory);other_route.mount_main(main);
   auto candidates=other_route.recall(id(10));
   CHECK(candidates.size()==1&&!candidates.temporary());
   CHECK(other_route.replay(candidates,0,8).location()==b.find(id(10))->state().experiences()[0].original());
   CHECK(main.merge(c,23,0));
   // Stale Main indexing cannot suppress an already available temporary hit.
   CHECK(route.input("text/plain",{}).temporary());
  }
  {
   auto main=PersistentMainGraph::open(root/"query-main",id(88),memory,1,policy,resolver);
   auto fresh_store=SessionStore::create(root,id(6),"fresh",65536,memory);SessionRuntime fresh(fresh_store,memory,8192);
   ExperienceRouter route(fresh,memory);route.mount_main(main);
   const auto before=reads;auto recalled=route.input("text/plain",{});
   CHECK(reads==before&&recalled.matches().size()==24);
   CHECK(recalled.matches()[0].recalled.recalled_head.strength==main.graph().find(id(10))->strength());
   CHECK(route.replay(recalled,16).location()==c.find(id(10))->state().experiences()[0].original());
  }
  // Forge a checksum-valid first merge with a wrong core-result digest.
  auto block=ExperienceBlock::open_reader(path/"m-0000000000000000.block");
  auto stored=block.read(first_head,1024,memory);auto view=stored.view();std::vector<std::byte> data(view.content.begin(),view.content.end());data[144]^=std::byte{1};view.content=data;
  fs::rename(path/"m-0000000000000000.block",path/"retained-original.block");
  {
   auto forged=ExperienceBlock::create(path/"m-0000000000000000.block",block.identity(),block.capacity());(void)forged.append(view);
  }
  bool core_mismatch=false;
  try{(void)PersistentMainGraph::open(path,identity,memory,1,policy,resolver);}
  catch(const std::runtime_error& error){core_mismatch=std::string_view(error.what())=="stored Main merge disagrees with SWEGCA replay";}
  CHECK(core_mismatch);
 }
 CHECK(memory.used()==0);fs::remove_all(root);std::printf("persistent Main tests: %u checks passed\n",checks);
}
