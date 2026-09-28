#include "vrs/block_store.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <barrier>
#include <atomic>
#include <unistd.h>
using namespace swegca::vrs;using namespace swegca::architecture::kernel;
Digest id(unsigned n){Digest d{};d[0]=std::byte(n);return d;}
template<class F> void fails(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}assert(threw);}
int main(){
 const auto root=std::filesystem::temp_directory_path()/("vrs-block-test-"+std::to_string(getpid()));std::filesystem::remove_all(root);
 const BlockStore::Config config{1024,8,1ULL<<24};
 auto verdict=judge_association({1,0});OctahedralBlocks::Pair pair{id(1),id(2)};
 auto update=[&](BlockStore& store,std::size_t block,unsigned n){OctahedralBlocks::Applied a{pair,{n,0,0},{n+1,0,0},verdict,n};store.append(block,{&a,1});};
 {
  BlockStore store(root,config);fails([&]{BlockStore other(root,config);});
  std::vector<std::thread> threads;
  for(unsigned b=0;b<4;++b)threads.emplace_back([&,b]{for(unsigned n=0;n<6;++n)update(store,b,n);});
  for(auto& t:threads)t.join();
  for(unsigned b=0;b<4;++b){assert(store.read(b).connections.at(pair).counts.accept==6);fails([&]{update(store,b,0);});}
  assert(store.blocks().size()==4);
 }
 {
  BlockStore store(root,config);for(unsigned b=0;b<4;++b)assert(store.read(b).connections.at(pair).counts.accept==6);
  std::uint64_t total=0;for(auto& f:std::filesystem::recursive_directory_iterator(root))if(f.is_regular_file())total+=f.file_size();assert(store.storage_used()==total);
 }
 fails([&]{BlockStore bad(root,{2048,8,1ULL<<24});});
 auto dir=root/"block-0";std::filesystem::path last;unsigned maximum=0;
 for(auto& f:std::filesystem::directory_iterator(dir)){auto n=std::stoul(f.path().stem());if(last.empty()||n>maximum){maximum=n;last=f.path();}assert(f.file_size()<=config.segment_bytes);}
 const auto size=std::filesystem::file_size(last);{std::ofstream out(last,std::ios::app|std::ios::binary);out<<"abc";}
 {BlockStore store(root,config);assert(store.read(0).connections.at(pair).counts.accept==6);update(store,0,6);assert(store.read(0).connections.at(pair).counts.accept==7);}
 assert(std::filesystem::file_size(last)==size+3);
 {BlockStore store(root,config);assert(store.read(0).connections.at(pair).counts.accept==7);}
 // Real topology -> core verdict -> independent block collector -> durable store.
 {
  BlockStore store(root/"graph",config);
  OctahedralBlocks graph({1,1,3,16},[&](auto b,auto rows){store.append(b,rows);});
  std::array<OctahedralBlocks::Work,3> work{{{{id(3),id(4)},{1,0},1},{{id(4),id(5)},{0,1},1},{{id(5),id(6)},{0,0},1}}};
  graph.submit(work,[](auto rows){std::vector<OctahedralBlocks::Verdict> out;for(auto& row:rows)out.push_back(judge_association(row.evidence));return out;});graph.finish();
  assert(store.blocks().size()==3);assert(graph.snapshot().experience_blocks==4);assert(graph.snapshot().relation_blocks==3);
  for(auto& row:work)assert(graph.counts(row.pair)==store.read(&row-&work[0]).connections.at(row.pair).counts);
 }
 // Independent collectors must overlap: the rendezvous fails on a serial collector.
 {
  BlockCollectors collectors(2,2,4);std::atomic<unsigned> entered=0;
  auto job=[&]{++entered;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(entered.load()<2&&std::chrono::steady_clock::now()<until)std::this_thread::yield();assert(entered==2);};
  collectors.submit(0,job);collectors.submit(1,job);collectors.finish();
 }
 {
  auto payload=ExperienceBlock::create(root/"original.block",id(99),4096);
  const std::array<std::byte,1> bytes{std::byte(42)};
  const auto address=payload.append({0,0,"test","test","binary",bytes});
  {BlockStore store(root/"bindings",config);const std::pair<BlockStore::Id,swegca::architecture::RecordAddress> binding{id(7),address};store.originals(0,{&binding,1});}
  {BlockStore store(root/"bindings",config);assert(store.read(0).originals.at(id(7))==address);}
 }
 {
  BlockStore store(root/"atomic",config);
  std::array<OctahedralBlocks::Applied,2> rows{{{pair,{}, {1,0,0},verdict,0},{pair,{9,0,0},{10,0,0},verdict,1}}};
  fails([&]{store.append(0,rows);});assert(store.read(0).batches==0);
 }
 // Complete-record corruption must not be treated as a recoverable torn tail.
 {
  std::fstream f(root/"block-1"/"0.block",std::ios::in|std::ios::out|std::ios::binary);f.seekg(200);char c;f.get(c);f.seekp(200);f.put(c^1);
 }
 {BlockStore store(root,config);fails([&]{(void)store.read(1);});}
 std::filesystem::remove_all(root);std::cout<<"PASS independent block writers, rollover, recovery, torn-tail preservation, stale rejection, ownership, topology/core storage integration\n";
}
