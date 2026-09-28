#include "vrs/block_ingress.hpp"
#include "swegca_architecture/sha256.hpp"
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace swegca::vrs;using namespace swegca::architecture;using namespace swegca::architecture::kernel;
template<class F>void fails(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}assert(caught);}
std::string bytes(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
std::map<std::string,std::string> snapshot(const std::filesystem::path& path){std::map<std::string,std::string> out;for(const auto& entry:std::filesystem::directory_iterator(path))if(entry.is_regular_file())out.emplace(entry.path().filename(),bytes(entry.path()));return out;}
struct Inbox {
 std::mutex mutex;std::condition_variable cv;std::vector<BlockIngress::Receipt> rows;
 void push(BlockIngress::Receipt r){std::lock_guard lock(mutex);rows.push_back(r);cv.notify_all();}
 BlockIngress::Receipt wait(){std::unique_lock lock(mutex);assert(cv.wait_for(lock,std::chrono::seconds(5),[&]{return !rows.empty();}));auto result=rows.front();rows.erase(rows.begin());return result;}
};
int main(){
 auto base=std::filesystem::temp_directory_path()/("swegca-block-session-"+std::to_string(getpid()));std::filesystem::remove_all(base);std::filesystem::create_directories(base);
 auto root=base/"store";BlockIngress::Config config;config.gpu_devices=0;config.codec_workers=2;config.cpu_workers=2;config.collector_workers=2;config.originals_per_block=2;config.queue=8;config.batch=2;config.memory_bytes=32ULL<<20;config.storage_bytes=1ULL<<28;
 auto request=[&](unsigned number){auto file=base/(std::to_string(number)+".txt");{std::ofstream out(file);out<<"experience "<<number<<" 日本語 한국어\n";}BlockFileRequest r;r.path=file;r.source="session-test";r.ticket=number;return r;};
 const auto a=request(1),d=request(4);BlockIngress::Receipt ar,br,dr,er;Inbox inbox;
 {
  BlockIngress ingress(root,config,[&](auto row){inbox.push(row);});ingress.submit(a);ar=inbox.wait();assert(ingress.original(ar.identity)==ar.original);ingress.finish();ingress.finish();fails([&]{ingress.submit(a);});
 }
 const auto first=snapshot(root/"block-0");assert(ar.block==0);
 {BlockStore store(root,{64ULL<<20,2,1ULL<<28});assert(store.read(0).sealed);auto state=store.read(0);const auto count=state.batches;store.seal(0);assert(store.read(0).batches==count);
  const std::pair<BlockStore::Id,RecordAddress> row{ar.identity,ar.original};fails([&]{store.originals(0,{&row,1});});}
 auto b=request(2);b.relations.push_back({ar.identity,{1,0}});
 {
  BlockIngress ingress(root,config,[&](auto row){inbox.push(row);});assert(ingress.original(ar.identity)==ar.original);
  ingress.submit(b);br=inbox.wait();assert(br.block==1);
  // Both a closed old block and the current live block remain addressable.
  auto forward=ingress.connected(br.identity),reverse=ingress.connected(ar.identity);
  assert(forward.size()==1&&forward[0].original==ar.original&&forward[0].strength.accept==1);
  assert(reverse.size()==1&&reverse[0].original==br.original);
  ingress.finish();
 }
 assert(snapshot(root/"block-0")==first);const auto second=snapshot(root/"block-1");
 {
  BlockIngress ingress(root,config,[&](auto row){inbox.push(row);});assert(ingress.connected(br.identity).at(0).original==ar.original);
  ingress.submit(a);auto duplicate=inbox.wait();assert(duplicate.duplicate&&duplicate.identity==ar.identity&&duplicate.original==ar.original);
  auto c=request(3);c.relations.push_back({br.identity,{0,1}});ingress.submit(c);auto cr=inbox.wait();assert(cr.block==2&&cr.relations.reject==1&&ingress.connected(cr.identity).empty());ingress.finish();
 }
 assert(snapshot(root/"block-0")==first&&snapshot(root/"block-1")==second);
 // Destruction without explicit session end drains work but MUST NOT seal it.
 {
  BlockIngress ingress(root,config,[&](auto row){inbox.push(row);});ingress.submit(d);dr=inbox.wait();assert(dr.block==3);
 }
 {BlockStore store(root,{64ULL<<20,2,1ULL<<28});assert(!store.read(3).sealed);}
 const auto payload=root/"block-3"/"0.payload",journal=root/"block-3"/"0.block";
 {std::ofstream out(payload,std::ios::binary|std::ios::app);out<<"cut";}
 {std::ofstream out(journal,std::ios::binary|std::ios::app);out<<"cut";}
 const auto partial_payload=bytes(payload),partial_journal=bytes(journal);
 {
  BlockIngress ingress(root,config,[&](auto row){inbox.push(row);});assert(ingress.original(dr.identity)==dr.original);
  auto e=request(5);e.relations.push_back({dr.identity,{1,0}});ingress.submit(e);er=inbox.wait();assert(er.block==dr.block);assert(ingress.connected(er.identity).at(0).original==dr.original);ingress.finish();
 }
 assert(bytes(payload)==partial_payload&&bytes(journal)==partial_journal);
 {BlockStore store(root,{64ULL<<20,2,1ULL<<28});auto state=store.read(3);assert(state.sealed&&state.originals.size()==2);assert(state.connections.at({er.identity,dr.identity}).counts.accept==1);}
 {
  BlockIngress ingress(root,config,[&](auto row){inbox.push(row);});assert(ingress.connected(dr.identity).at(0).original==er.original);ingress.submit(d);assert(inbox.wait().duplicate);ingress.finish();
 }
 // A failed receipt callback cannot turn a failed finish into a sealed session.
 const auto failed_root=base/"failed";
 {
  BlockIngress ingress(failed_root,config,[](auto){throw std::runtime_error("receipt failure");});ingress.submit(a);fails([&]{ingress.finish();});
 }
 {BlockStore store(failed_root,{64ULL<<20,2,1ULL<<28});assert(!store.read(0).sealed&&store.read(0).originals.size()==1);}
 {
  BlockIngress ingress(failed_root,config,[&](auto row){inbox.push(row);});ingress.submit(a);assert(inbox.wait().duplicate);ingress.finish();
 }
 // Missing referenced data is an error; it is never dropped from recovery.
 std::filesystem::remove(root/"block-0"/"0.payload");fails([&]{BlockIngress ingress(root,config,[](auto){});});
 std::filesystem::remove_all(base);
 std::cout<<"PASS seal idempotence/immutability, resume live blocks, preserve interrupted tails, dedup across restarts, live+closed bidirectional portals, unchanged closed block bytes, failure-not-sealed, missing payload rejection\n";
}
