#include "vrs/connection_regions.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace swegca::vrs;
using swegca::architecture::DigestBytes;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
class Failing final:public std::pmr::memory_resource{
public: std::size_t remaining=SIZE_MAX;
private:
 void* do_allocate(std::size_t n,std::size_t a)override{if(!remaining)throw std::bad_alloc();--remaining;return std::pmr::new_delete_resource()->allocate(n,a);}
 void do_deallocate(void* p,std::size_t n,std::size_t a)override{std::pmr::new_delete_resource()->deallocate(p,n,a);}
 bool do_is_equal(const std::pmr::memory_resource& r)const noexcept override{return this==&r;}
};
struct Value{explicit Value(unsigned v):value(v){}Value(const Value&)=delete;unsigned value;};
DigestBytes key(unsigned n){DigestBytes id{};for(unsigned i=0;i<4;++i)id[31-i]=std::byte((n>>(8*i))&255);return id;}
int main(){
 using swegca::architecture::kernel::region_partition_end;
 CHECK(!region_partition_end(0,1,0));CHECK(!region_partition_end(1,1,2));
 CHECK(region_partition_end(SIZE_MAX-2,SIZE_MAX,8)==SIZE_MAX);
 Failing failing;MemoryBudget memory(8<<20,&failing);
 {
  ConnectionRegions<Value> regions(memory,8);ConnectionRegions<Value>::Entries pending(&memory);
  CHECK(!regions.next_key(key(0)));
  for(unsigned n=1;n<=65;++n)pending.try_emplace(key(n),n);
  auto first=regions.prepare(pending);CHECK(regions.region_count()==0);
  failing.remaining=0;regions.commit(first,pending);failing.remaining=SIZE_MAX;
  CHECK(pending.empty()&&regions.region_count()==9&&regions.largest_region()==8);
  CHECK(regions.next_key(key(0))==key(1));
  for(unsigned n=1;n<=65;++n)CHECK(regions.next_key(key(n))==key(n));
  CHECK(!regions.next_key(key(66)));
  const auto* preserved=regions.find(key(65));const auto* unrelated=regions.find(key(30));
  CHECK(!regions.find(key(0))&&!regions.find(key(100)));
  for(unsigned n=1;n<=10;++n)pending.try_emplace(key(n),n+1000);
  for(unsigned n=66;n<=90;++n)pending.try_emplace(key(n),n);
  const auto baseline=memory.used();unsigned failures=0;bool committed=false;
  for(unsigned limit=0;limit<1000;++limit){
   failing.remaining=limit;
   try{
    auto plan=regions.prepare(pending);
    CHECK(regions.find(key(1))->value==1&&regions.find(key(90))==nullptr);
    failing.remaining=0;regions.commit(plan,pending);committed=true;
   }catch(const std::bad_alloc&){++failures;CHECK(memory.used()==baseline);CHECK(regions.region_count()==9);}
   failing.remaining=SIZE_MAX;if(committed)break;
  }
  CHECK(committed&&failures>0&&pending.empty());
  CHECK(regions.largest_region()<=8&&regions.region_count()==12);
  CHECK(regions.find(key(65))==preserved&&regions.find(key(30))==unrelated);
  for(unsigned n=1;n<=90;++n)CHECK(regions.find(key(n))->value==(n<=10?n+1000:n));
  // Repeated prepared updates must not increase region count or lose boundaries.
  for(unsigned n=90;n>0;--n)pending.try_emplace(key(n),n+2000);
  auto update=regions.prepare(pending);failing.remaining=0;regions.commit(update,pending);failing.remaining=SIZE_MAX;
  CHECK(regions.region_count()==12&&regions.largest_region()<=8);
  for(unsigned n=1;n<=90;++n)CHECK(regions.find(key(n))->value==n+2000);
 }
 CHECK(memory.used()==0);
 {
  // A large first publication may reserve only the region directory. A
  // second digest array for all 32K pending connections cannot fit here.
  ConnectionRegions<Value> regions(memory,256);ConnectionRegions<Value>::Entries pending(&memory);
  for(unsigned n=0;n<32768;++n)pending.try_emplace(key(n*2),n);
  const auto reserved=memory.limit()-memory.used()-32768;
  auto* held=memory.allocate(reserved);
  auto plan=regions.prepare(pending);
  failing.remaining=0;regions.commit(plan,pending);failing.remaining=SIZE_MAX;
  memory.deallocate(held,reserved);
  CHECK(pending.empty()&&regions.region_count()==128&&regions.largest_region()==256);
  for(unsigned n=0;n<32768;++n)CHECK(regions.find(key(n*2))->value==n);
  CHECK(!regions.find(key(1))&&!regions.find(key(65536)));
  // Interleave new keys with replacements on both sides of existing region
  // boundaries. Duplicate keys count once and existing values stay stable.
  const auto* retained=regions.find(key(40000));
  for(unsigned n=0;n<1024;++n)pending.try_emplace(key(n),n+100000);
  auto split=regions.prepare(pending);
  failing.remaining=0;regions.commit(split,pending);failing.remaining=SIZE_MAX;
  CHECK(pending.empty()&&regions.largest_region()<=256);
  CHECK(regions.find(key(40000))==retained);
  for(unsigned n=0;n<1024;++n)CHECK(regions.find(key(n))->value==n+100000);
  for(unsigned n=512;n<32768;++n)CHECK(regions.find(key(n*2))->value==n);
 }
 CHECK(memory.used()==0);
 std::printf("connection region tests: %u checks passed\n",checks);
}
