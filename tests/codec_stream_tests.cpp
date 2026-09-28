#include "vrs/codec_stream.hpp"
#include "swegca_architecture/sha256.hpp"
#include <cassert>
#include <fstream>
#include <map>
#include <mutex>
#include <condition_variable>
#include <set>
#include <iostream>
#include <unistd.h>
using namespace swegca::vrs;
int main(int argc,char** argv){
 char temp[]="/tmp/swegca-stream-XXXXXX";assert(mkdtemp(temp));std::filesystem::path root(temp);
 // Large raw source must not become a large RAM vector or a second snapshot.
 const std::string unit(65536,'x');swegca::architecture::Sha256 expected;
 {std::ofstream out(root/"large.txt",std::ios::binary);for(int i=0;i<512;++i){out.write(unit.data(),unit.size());expected.update(unit);}}
 MemoryBudget memory(4ULL<<20);std::uint64_t seen=0;std::size_t calls=0;
 auto receipt=stream_codec_file(root/"large.txt",memory,[&](const CodecChunk& c){assert(c.original&&c.offset==seen&&c.bytes.size()<=65536);seen+=c.bytes.size();++calls;},65536);
 assert(receipt.identity==expected.finish()&&seen==(32ULL<<20)&&calls==512&&memory.used()==0&&memory.peak_reserved()<=131072);
 // Decoded bytes are delivered while being produced, preserving the complete
 // stream and offsets. Invalid/NUL bytes are not filtered by the transport.
 {std::ofstream out(root/"image.ppm",std::ios::binary);out<<"P6\n128 128\n255\n";std::string pixels(128*128*3,'\0');for(std::size_t i=0;i<pixels.size();++i)pixels[i]=char(i%256);out.write(pixels.data(),pixels.size());}
 std::mutex mutex;std::map<std::size_t,std::uint64_t> offsets;std::map<std::size_t,swegca::architecture::Sha256> hashes;
 auto image=stream_codec_file(root/"image.ppm",memory,[&](const CodecChunk& c){std::lock_guard lock(mutex);assert(c.offset==offsets[c.stream]);offsets[c.stream]+=c.bytes.size();hashes[c.stream].update(c.bytes);},4096);
 assert(image.codec_errors.empty()&&image.views.size()==1&&image.views[0].complete);
 assert(image.views[0].digest==hashes[1].finish()&&image.views[0].bytes==offsets[1]&&offsets[1]>4096);
 // Consumer errors are not turned into a successful file or a core abstention.
 bool failed=false;try{stream_codec_file(root/"image.ppm",memory,[](const CodecChunk&){throw std::logic_error("consumer failed");});}catch(const std::logic_error& e){failed=std::string(e.what())=="consumer failed";}assert(failed);
 // Changes to the input invalidate provisional output; never publish it.
 failed=false;bool changed=false;
 try{stream_codec_file(root/"large.txt",memory,[&](const CodecChunk& c){if(c.original&&!changed){changed=true;std::ofstream out(root/"large.txt",std::ios::app);out<<'z';}});}catch(const std::runtime_error& e){failed=std::string(e.what()).find("source changed")!=std::string::npos;}assert(failed);
 assert(memory.used()==0);std::filesystem::remove_all(root);
 if(argc>1){
  std::mutex gate;std::condition_variable ready;std::set<std::size_t> arrived;
  auto tracks=stream_codec_file(argv[1],memory,[&](const CodecChunk& c){
   if(c.original||c.offset)return;
   std::unique_lock lock(gate);arrived.insert(c.stream);ready.notify_all();
   if(!ready.wait_for(lock,std::chrono::seconds(5),[&]{return arrived.size()>=2;}))throw std::runtime_error("tracks were decoded serially");
  },4096);
  assert(tracks.codec_errors.empty()&&tracks.views.size()==2&&arrived.size()==2);
  for(const auto& view:tracks.views)assert(view.complete&&view.bytes);
  std::cout<<"PASS video/audio codec tracks overlap before either finishes\n";
 }
 std::cout<<"PASS bounded unchanged stream, real image codec, offset/digest continuity, consumer failure and source mutation\n";
}
