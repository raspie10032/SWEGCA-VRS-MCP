#include "vrs/parallel_ingress.hpp"
#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"
#include <fstream>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
using namespace swegca::vrs;using namespace swegca::architecture;using namespace swegca::transport;
DigestBytes key(std::string_view s){Sha256 h;h.update(s);return h.finish();}
std::string hex(const DigestBytes& d){std::string s;for(auto b:d){auto x=std::to_integer<unsigned>(b);s+="0123456789abcdef"[x>>4];s+="0123456789abcdef"[x&15];}return s;}
struct Mapped {int fd=-1;void* data=MAP_FAILED;std::size_t size=0;~Mapped(){if(data!=MAP_FAILED)munmap(data,size);if(fd>=0)close(fd);}void open(const std::string& path){fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st{};if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0)throw std::runtime_error("unreadable whole file");size=st.st_size;if(size){data=mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0);if(data==MAP_FAILED)throw std::runtime_error("mapping failed");madvise(data,size,MADV_SEQUENTIAL);}}std::span<const std::byte> view()const{return {size?static_cast<const std::byte*>(data):nullptr,size};}};
int main(int argc,char**argv)try{
 if(argc!=2)throw std::runtime_error("usage: whole-file-ingress NEW_ROOT");umask(0077);std::filesystem::path root(argv[1]);std::filesystem::create_directories(root);
 MemoryBudget memory(3000000000ULL);RuntimeConfig cfg{key(root.string()),{},1,500000000000ULL,8ULL<<20,500000000000ULL};cfg.merge_workers=10;cfg.storage_bytes=500000000000ULL;
 auto host=Runtime::create(root,cfg,memory);std::vector<std::string> names;std::vector<DigestBytes> ids;for(unsigned i=0;i<10;++i){names.push_back("whole-file-"+std::to_string(i));ids.push_back(key(root.string()+names.back()));host.attach_session(ids.back(),names.back());}
 std::set<DigestBytes> seen;std::string line;std::uint64_t seq=0;
 while(std::getline(std::cin,line)){
  auto request=parse_json(line,memory);if(request.kind!=Json::Kind::array||request.values.size()>10)throw std::runtime_error("batch must contain at most ten whole files");
  std::vector<Mapped> maps(request.values.size());std::vector<std::string> sources,media;std::vector<DigestBytes> hashes;std::vector<ParallelInput> input;std::vector<std::size_t> indices;
  sources.reserve(maps.size());media.reserve(maps.size());
  for(std::size_t i=0;i<maps.size();++i){const auto& r=request.values[i];sources.emplace_back(r.at("source").string());media.emplace_back(r.at("media").string());maps[i].open(std::string(r.at("path").string()));hashes.push_back(Sha256::of(maps[i].view()));
   if(seen.contains(hashes.back()))continue;seen.insert(hashes.back());indices.push_back(i);input.push_back({ids[i],{seq++,0,names[i],sources[i],media[i],maps[i].view()}});
  }
  auto results=ParallelIngress::retain(host,input,20260928,0);std::cout<<"[";bool comma=false;
  for(std::size_t j=0;j<results.size();++j){if(results[j].error)std::rethrow_exception(results[j].error);auto i=indices[j];const auto& a=results[j].recorded->original;if(comma)std::cout<<',';comma=true;
   // Only ONE retain call and ONE sealed original address for this whole file.
   std::cout<<"{\"index\":"<<i<<",\"bytes\":"<<maps[i].size<<",\"sha256\":\""<<hex(hashes[i])<<"\",\"original\":{\"block\":\""<<hex(a.block)<<"\",\"offset\":"<<a.offset<<",\"bytes\":"<<a.bytes<<",\"digest\":\""<<hex(a.digest)<<"\"},\"status\":"<<unsigned(results[j].recorded->refinement.result().verification().judgment().status())<<"}";
  }
  std::cout<<"]"<<std::endl;
 }
 for(const auto& id:ids)host.end_session(id);
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
