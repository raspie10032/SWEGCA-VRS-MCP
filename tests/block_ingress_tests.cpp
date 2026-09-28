#include "vrs/block_ingress.hpp"
#include "swegca_architecture/sha256.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <atomic>
#include <unistd.h>
using namespace swegca::vrs;using namespace swegca::architecture;using namespace swegca::architecture::kernel;
int main(){
 const auto base=std::filesystem::temp_directory_path()/("swegca-block-ingress-"+std::to_string(getpid()));std::filesystem::remove_all(base);std::filesystem::create_directories(base);
 std::vector<std::filesystem::path> files;for(unsigned i=0;i<24;++i){auto path=base/(std::to_string(i)+".txt");std::ofstream f(path);f<<"actual input "<<i<<" 日本語 한국어\n";files.push_back(path);}
 auto image=base/"image.ppm";{std::ofstream f(image,std::ios::binary);f<<"P6\n1 1\n255\n";const char pixels[]={char(255),0,0};f.write(pixels,3);}files.push_back(image);
 BlockIngress::Config config;config.gpu_devices=0;config.codec_workers=4;config.cpu_workers=2;config.collector_workers=3;config.originals_per_block=3;config.queue=8;config.batch=3;config.memory_bytes=64ULL<<20;config.storage_bytes=1ULL<<28;
 std::mutex mutex;std::vector<BlockIngress::Receipt> receipts;DigestBytes peer{};peer[0]=std::byte(123);
 {
  BlockIngress ingress(base/"store",config,[&](auto r){std::lock_guard lock(mutex);receipts.push_back(r);});
  for(unsigned i=0;i<files.size();++i){BlockFileRequest r;r.path=files[i];r.source="fixture";r.ticket=i;r.observation="measured fixture "+std::to_string(i);r.relations.push_back({peer,{i%3==0?1ULL:0ULL,i%3==1?1ULL:0ULL}});ingress.submit(r);if(i==0)ingress.submit(r);}
  ingress.finish();assert(ingress.stats().applied==26);
 }
 assert(receipts.size()==26);unsigned duplicates=0,images=0;
 {BlockStore store(base/"store",{64ULL<<20,3,1ULL<<28});assert(store.blocks().size()==9);
  for(auto block:store.blocks()){auto state=store.read(block);assert(state.originals.size()<=3);assert(state.originals.size()==state.connections.size());}
  for(auto r:receipts){if(r.duplicate){++duplicates;continue;}assert(r.status==EvidenceStatus::abstain);if(r.ticket==24){assert(r.decoded_views==1);++images;}
   const auto state=store.read(r.block);bool found=false;for(auto& [identity,address]:state.originals)if(address==r.original){found=true;const auto counts=state.connections.at({identity,peer}).counts;assert(counts.accept==(r.ticket%3==0));assert(counts.reject==(r.ticket%3==1));assert(counts.abstain==(r.ticket%3==2));}
   assert(found);
   bool payload_found=false;MemoryBudget memory(4ULL<<20);
   for(auto& entry:std::filesystem::directory_iterator(base/"store"/("block-"+std::to_string(r.block))))if(entry.path().extension()==".payload"){
    auto file=ExperienceBlock::open_reader(entry.path());if(file.identity()!=r.original.block)continue;
    auto stored=file.read(r.original,4ULL<<20,memory);std::ifstream source(files.at(r.ticket),std::ios::binary);std::string expected((std::istreambuf_iterator<char>(source)),{});
    const auto actual=stored.view().content;assert(actual.size()==expected.size());assert(std::memcmp(actual.data(),expected.data(),expected.size())==0);payload_found=true;
   }
   assert(payload_found);
  }
 }
 assert(duplicates==1&&images==1);
 // Routed application has one owner per block, but different blocks overlap.
 {
  using F=WorkPipeline<unsigned,unsigned,unsigned>;std::atomic<unsigned> active=0,applied=0;
  F flow({2,16,16,16,4,std::chrono::milliseconds(1000)},[](unsigned i){return std::make_shared<const unsigned>(i);},{{"CPU",8,8,[](auto rows){std::vector<unsigned> values;for(auto& row:rows)values.push_back(*row);return values;}}},[&](auto rows){
   assert(rows.size()==4);const auto block=rows.front()/4;for(auto row:rows)assert(row/4==block);
   ++active;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   while(active.load()<2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();assert(active==2);applied+=rows.size();
  },[](unsigned value){return value/4;},2);
  for(unsigned i=0;i<8;++i)flow.submit(i);flow.finish();assert(applied==8&&flow.stats().apply_batches==2);
 }
 // GPU kernels execute the SAME association primitive on distinct input rows.
 for(int device=0;device<DeviceEvidence::available()&&device<2;++device){DeviceEvidence gpu(device);std::vector<AssociationEvidence> input(4096);std::vector<AssociationJudgment> output(input.size());for(std::size_t i=0;i<input.size();++i)input[i]={i%2,(i/2)%2};gpu.associate(input,output);for(std::size_t i=0;i<input.size();++i){const auto expected=judge_association(input[i]);assert(output[i].status()==expected.status()&&output[i].reason()==expected.reason());}}
 std::filesystem::remove_all(base);std::cout<<"PASS actual files/codecs -> core -> block collectors -> reopened originals and ternary connections; duplicate once; both CUDA association parity\n";
}
