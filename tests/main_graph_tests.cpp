#include "vrs/main_graph.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unistd.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
static unsigned checks=0;
#define CHECK(e) do { ++checks; if (!(e)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort(); } } while(false)
static std::uint64_t reads=0,writes=0;
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t pos){++reads;return __real_pread(fd,data,n,pos);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t pos){++writes;return __real_pwrite(fd,data,n,pos);}
template<class E,class F> void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
DigestBytes id(unsigned n){DigestBytes d{};for(unsigned i=0;i<4;++i)d[i]=std::byte((n>>(8*i))&255);return d;}
class FailingMemory final:public std::pmr::memory_resource {
public: std::size_t remaining=std::numeric_limits<std::size_t>::max();
private:
 void* do_allocate(std::size_t n,std::size_t a) override {if(!remaining)throw std::bad_alloc();--remaining;return std::pmr::new_delete_resource()->allocate(n,a);}
 void do_deallocate(void* p,std::size_t n,std::size_t a) override {std::pmr::new_delete_resource()->deallocate(p,n,a);}
 bool do_is_equal(const std::pmr::memory_resource& other)const noexcept override{return this==&other;}
};
void fill(SessionRuntime& runtime,std::string_view name,unsigned connection,unsigned base,unsigned count,EvidenceOutcome outcome){
 const std::string text="retained original";
 for(unsigned n=0;n<count;++n){EvidenceObservation value;
  value.hypothesis=id(connection);value.source=id(base+n);value.context=id(base+n+10000);value.producer=id(base+n+20000);
  value.observed_at=base+n;value.outcome=outcome;
  (void)runtime.observe(id(connection),{n,base+n,name,"experiment","text/plain",std::as_bytes(std::span(text))},value,7,100000);
 }
}
int main(){
 auto pattern=(std::filesystem::temp_directory_path()/"swegca-main-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));
 const std::filesystem::path root(pattern);
 MemoryBudget source_memory(64<<20);FailingMemory failing;MemoryBudget graph_memory(64<<20,&failing);
 EvidencePolicy policy;policy.axis_count=1;
 {
  auto a_store=SessionStore::create(root,id(1),"a",65536,source_memory);SessionRuntime a(a_store,source_memory,8192);
  a.define_connection(id(10),0.75,policy);fill(a,"a",10,100,16,EvidenceOutcome::support);
  MainGraph graph(graph_memory,1.0,policy,1,1);
  throws<std::logic_error>([&]{(void)graph.merge(a,7,100000);});CHECK(graph.generation()==0);
  a.end();throws<std::logic_error>([&]{(void)graph.merge(a,7,100000);});a.publish_originals();
  const auto before_reads=reads,before_writes=writes;
  CHECK(graph.merge(a,7,100000));CHECK(reads==before_reads&&writes==before_writes);
  CHECK(graph.generation()==1&&graph.source_count()==1);
  CHECK(graph.region_count()==1&&graph.largest_region()==1);
  CHECK(graph.find(id(10))->strength()==1.01);
  CHECK(graph.find(id(10))->strength()!=a.find(id(10))->state().strength());
  CHECK(graph.find(id(10))->experiences().size()==16);
  const auto first=a.find(id(10))->state().experiences()[0].original();
  CHECK(graph.replay(id(10),0).location()==first);
  CHECK(!graph.merge(a,999,100001));CHECK(graph.find(id(10))->strength()==1.01);
  auto b_store=SessionStore::create(root,id(2),"b",65536,source_memory);SessionRuntime b(b_store,source_memory,8192);
  b.define_connection(id(10),0.75,policy);fill(b,"b",10,1000,64,EvidenceOutcome::refute);
  b.define_connection(id(11),0.75,policy);fill(b,"b",11,2000,16,EvidenceOutcome::support);
  b.end();b.publish_originals();
  const auto a_head=a.find(id(10))->head(),b_head=b.find(id(10))->head();
  const auto* shared_first=&graph.find(id(10))->experiences()[0];
  const auto baseline=graph_memory.used();unsigned failures=0;bool completed=false;
  for(unsigned point=0;point<3000;++point){
   failing.remaining=point;
   try {CHECK(graph.merge(b,7,100000));completed=true;}
   catch(const std::bad_alloc&){++failures;CHECK(graph.generation()==1&&graph.source_count()==1);
    CHECK(graph.find(id(10))->strength()==1.01&&graph.find(id(10))->experiences().size()==16);
    CHECK(graph.find(id(11))==nullptr);CHECK(graph_memory.used()==baseline);
   }
   failing.remaining=std::numeric_limits<std::size_t>::max();if(completed)break;
  }
  CHECK(completed&&failures>0);CHECK(graph.generation()==2&&graph.source_count()==2);
  CHECK(graph.region_count()==2&&graph.largest_region()==1);
  CHECK(&graph.find(id(10))->experiences()[0]==shared_first);
  CHECK(graph.find(id(10))->experiences().size()==80&&graph.find(id(11))->experiences().size()==16);
  {
   const auto rules=make_evidence_rules(policy);Connection expected(id(10),1.01,rules,source_memory);
   for(const auto& e:a.find(id(10))->state().experiences())expected.append(e);
   for(const auto& e:b.find(id(10))->state().experiences())expected.append(e);
   const auto report=expected.refine(7,100000);
   CHECK(graph.find(id(10))->strength()==expected.strength());
   CHECK(graph.refinement(id(10))->result().verification().judgment().status()==report.result().verification().judgment().status());
  }
  CHECK(graph.find(id(10))->strength()<1.01);
  CHECK(a.find(id(10))->head()==a_head&&b.find(id(10))->head()==b_head);
  CHECK(graph.replay(id(10),16).location()==b.find(id(10))->state().experiences()[0].original());
  CHECK(graph.replay(id(10),0).location()==first);
  for(std::size_t index=0;index<80;++index){
   const auto expected=index<16?a.find(id(10))->state().experiences()[index].original():
       b.find(id(10))->state().experiences()[index-16].original();
   CHECK(graph.replay(id(10),index).location()==expected);
  }
  for(std::size_t index=0;index<16;++index)
   CHECK(graph.replay(id(11),index).location()==b.find(id(11))->state().experiences()[index].original());
  throws<std::out_of_range>([&]{(void)graph.replay(id(11),16);});
  CHECK(!graph.merge(b,8,100001)&&graph.generation()==2);
  throws<std::out_of_range>([&]{(void)graph.replay(id(10),80);});
  auto bad_store=SessionStore::create(root,id(3),"bad",65536,source_memory);SessionRuntime bad(bad_store,source_memory,8192);
  bad.define_connection(id(10),0.75,policy);fill(bad,"bad",10,3000,16,EvidenceOutcome::support);
  auto other_policy=policy;other_policy.accept_margin=0.1;
  bad.define_connection(id(11),0.75,other_policy);fill(bad,"bad",11,4000,16,EvidenceOutcome::support);
  bad.end();bad.publish_originals();const auto strength=graph.find(id(10))->strength();const auto used=graph_memory.used();
  throws<std::invalid_argument>([&]{(void)graph.merge(bad,8,100000);});
  CHECK(graph.generation()==2&&graph.source_count()==2&&graph.find(id(10))->strength()==strength);
  CHECK(graph_memory.used()==used);
  std::printf("Main allocation failure points: %u\n",failures);
 }
 CHECK(graph_memory.used()==0&&source_memory.used()==0);
 {
  auto store=SessionStore::create(root,id(4),"released",65536,source_memory);
  MainGraph graph(graph_memory,1.0,policy);ExperienceLocation original;std::size_t cached=0;
  {
   SessionRuntime source(store,source_memory,8192);source.define_connection(id(10),0.75,policy);
   fill(source,"released",10,6000,16,EvidenceOutcome::support);
   source.end();source.publish_originals();original=source.find(id(10))->state().experiences()[0].original();
   CHECK(graph.merge(source,7,100000));cached=source_memory.used();
  }
  CHECK(source_memory.used()<cached);
  CHECK(graph.replay(id(10),0).location()==original);
  {
   SessionRuntime reopened_cache(store,source_memory,8192);
   CHECK(!graph.merge(reopened_cache,8,100001));CHECK(graph.generation()==1&&graph.source_count()==1);
  }
 }
 CHECK(graph_memory.used()==0&&source_memory.used()==0);
 std::filesystem::remove_all(root);std::printf("Main graph tests: %u checks passed\n",checks);
}
