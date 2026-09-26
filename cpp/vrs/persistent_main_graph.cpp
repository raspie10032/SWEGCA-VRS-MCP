#include "vrs/persistent_main_graph.hpp"
#include "swegca_architecture/sha256.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace swegca::vrs {
namespace {
using namespace architecture;
constexpr std::string_view session="main", source="swegca-main", media="application/vnd.swegca.main-v1";
constexpr std::size_t record_size=256, config_size=56;
constexpr std::uint64_t metadata=ExperienceBlock::record_overhead+session.size()+source.size()+media.size();
void put(std::span<std::byte> data,std::size_t at,std::uint64_t n){for(unsigned i=0;i<8;++i)data[at+i]=std::byte(n>>(8*i));}
std::uint64_t get(std::span<const std::byte> data,std::size_t at){std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=std::uint64_t(std::to_integer<unsigned>(data[at+i]))<<(8*i);return n;}
void put_digest(std::span<std::byte> data,std::size_t at,const DigestBytes& d){std::copy(d.begin(),d.end(),data.begin()+at);}
DigestBytes get_digest(std::span<const std::byte> data,std::size_t at){DigestBytes d;std::copy_n(data.begin()+at,d.size(),d.begin());return d;}
void put_address(std::span<std::byte> data,std::size_t at,const ExperienceLocation& a){put_digest(data,at,a.block);put(data,at+32,a.offset);put(data,at+40,a.bytes);put_digest(data,at+48,a.digest);}
ExperienceLocation get_address(std::span<const std::byte> data,std::size_t at){return {get_digest(data,at),get(data,at+32),get(data,at+40),get_digest(data,at+48)};}
DigestBytes block_id(const DigestBytes& graph,bool control,std::uint64_t index){
 Sha256 h;h.update(control?"SWEGCA Main control v1":"SWEGCA Main merge block v1");h.update(graph);
 std::array<std::byte,8> n{};put(n,0,index);h.update(n);return h.finish();
}
std::string filename(std::uint64_t index){char text[32];std::snprintf(text,sizeof(text),"m-%016llx.block",(unsigned long long)index);return text;}
void sync_directory(const std::filesystem::path& path){
 int fd=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)throw std::system_error(errno,std::generic_category(),"open Main directory");
 const int result=::fsync(fd),error=errno;::close(fd);if(result<0)throw std::system_error(error,std::generic_category(),"sync Main directory");
}
void check_metadata(const StoredExperience& stored){const auto v=stored.view();if(v.session!=session||v.source!=source||v.media_type!=media)throw std::runtime_error("invalid Main record metadata");}
}

PersistentMainGraph PersistentMainGraph::create(const std::filesystem::path& path,const DigestBytes& identity,
 MemoryBudget& memory,double strength,const EvidencePolicy& policy,std::uint64_t capacity,std::uint32_t workers,StorageBudget* storage){
 if(capacity<ExperienceBlock::header_bytes+metadata+record_size)throw std::invalid_argument("Main block cannot hold a merge");
 return PersistentMainGraph(path,identity,memory,strength,policy,capacity,nullptr,workers,storage);
}
PersistentMainGraph PersistentMainGraph::open(const std::filesystem::path& path,const DigestBytes& identity,
 MemoryBudget& memory,double strength,const EvidencePolicy& policy,MainSourceResolver& resolver,std::uint32_t workers,StorageBudget* storage){
 return PersistentMainGraph(path,identity,memory,strength,policy,0,&resolver,workers,storage);
}
PersistentMainGraph::PersistentMainGraph(const std::filesystem::path& path,const DigestBytes& identity,
 MemoryBudget& memory,double strength,const EvidencePolicy& policy,std::uint64_t capacity,MainSourceResolver* resolver,std::uint32_t workers,StorageBudget* storage)
 :directory_(path),identity_(identity),memory_(memory),storage_(storage),graph_(memory,strength,policy,workers),capacity_(capacity){
 if(!architecture::kernel::named_digest(identity))throw std::invalid_argument("empty Main identity");
 std::array<std::byte,config_size> config{};std::memcpy(config.data(),"SWGCMCF1",8);
 put(config,8,std::bit_cast<std::uint64_t>(strength));put_digest(config,16,evidence_policy_digest(policy).bytes());
 if(!resolver){
  if(!std::filesystem::create_directory(path))throw std::runtime_error("Main directory already exists");
  sync_directory(path.parent_path().empty()?std::filesystem::path("."):path.parent_path());
  control_.emplace(ExperienceBlock::create(path/"control.block",block_id(identity,true,0),ExperienceBlock::header_bytes+metadata+config_size,storage_));
  put(config,48,capacity_);(void)control_->append({0,0,session,source,media,config});
 }else{
  control_.emplace(ExperienceBlock::open_writer(path/"control.block",storage_));
  if(control_->identity()!=block_id(identity,true,0))throw std::runtime_error("Main identity mismatch");
  const auto inspected=control_->inspect();
  if(inspected.complete_records!=1||inspected.unfinished_bytes)throw std::runtime_error("incomplete Main configuration");
  const auto stored=control_->read(control_->location_at(ExperienceBlock::header_bytes),metadata+config_size,memory_);
  check_metadata(stored);const auto data=stored.view().content;
  if(data.size()!=config_size||!std::equal(config.begin(),config.begin()+48,data.begin())||stored.view().sequence||stored.view().observed_at_ns)
   throw std::runtime_error("Main configuration mismatch");
  capacity_=get(data,48);if(capacity_<ExperienceBlock::header_bytes+metadata+record_size)throw std::runtime_error("invalid Main capacity");
  restore(*resolver);
 }
}
const MainGraph& PersistentMainGraph::graph()const{
 if(!usable_)throw std::logic_error("Main must be reopened after write failure");
 return graph_;
}
bool PersistentMainGraph::merge(const SessionRuntime& source,std::uint64_t seed,std::uint64_t step){
 (void)graph();return graph_.merge_impl(source,seed,step,persist,this);
}
void PersistentMainGraph::next_block(){
 if(next_index_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Main block index exhausted");
 writer_.reset();const auto final=directory_/filename(next_index_);
 std::uint64_t attempt=0;std::filesystem::path staging;
 do {staging=directory_/("pending-"+std::to_string(next_index_)+"-"+std::to_string(attempt++)+".block");}
 while(std::filesystem::exists(staging));
 auto created=ExperienceBlock::create(staging,block_id(identity_,false,next_index_),capacity_,storage_);
 if(::link(staging.c_str(),final.c_str())<0)throw std::system_error(errno,std::generic_category(),"publish Main block");
 if(::unlink(staging.c_str())<0)throw std::system_error(errno,std::generic_category(),"release Main block staging name");
 sync_directory(directory_);writer_.emplace(std::move(created));++next_index_;
}
void PersistentMainGraph::persist(void* context,const DigestBytes& identity,const ExperienceLocation& source_root,
 const DigestBytes& result,std::uint64_t generation,std::uint64_t seed,std::uint64_t step){
 auto& self=*static_cast<PersistentMainGraph*>(context);
 std::array<std::byte,record_size> data{};std::memcpy(data.data(),"SWGCMRG1",8);put(data,8,generation);
 put_digest(data,16,identity);put_address(data,48,source_root);put(data,128,seed);put(data,136,step);
 put_digest(data,144,result);put_address(data,176,self.head_);
 try {
  if(!self.writer_)self.next_block();
  try {self.head_=self.writer_->append({generation,step,session,source,media,data});}
  catch(const std::length_error&){self.next_block();self.head_=self.writer_->append({generation,step,session,source,media,data});}
 }catch(...){self.usable_=false;throw;}
}
void PersistentMainGraph::verify(void* context,const DigestBytes& source,const ExperienceLocation& root,
 const DigestBytes& result,std::uint64_t generation,std::uint64_t seed,std::uint64_t step){
 const auto& expected=*static_cast<const Record*>(context);
 if(source!=expected.source||root!=expected.source_root||result!=expected.result||generation!=expected.generation||seed!=expected.seed||step!=expected.step)
  throw std::runtime_error("stored Main merge disagrees with SWEGCA replay");
}
void PersistentMainGraph::restore(MainSourceResolver& resolver){
 std::pmr::vector<std::uint64_t> indices(&memory_);
 for(const auto& entry:std::filesystem::directory_iterator(directory_)){
  const auto name=entry.path().filename().string();if(!name.starts_with("m-"))continue;
  std::uint64_t index=0;const auto parsed=name.size()==24?std::from_chars(name.data()+2,name.data()+18,index,16):std::from_chars_result{};
  if(name.size()!=24||parsed.ec!=std::errc{}||parsed.ptr!=name.data()+18||name!=filename(index)||entry.is_symlink()||!entry.is_regular_file())
   throw std::runtime_error("invalid Main block filename");
  indices.push_back(index);
 }
 std::sort(indices.begin(),indices.end());
 for(const auto index:indices){
  if(index!=next_index_||index==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("Main block sequence gap");
  auto block=ExperienceBlock::open_reader(directory_/filename(index), storage_);
  if(block.identity()!=block_id(identity_,false,index)||block.capacity()!=capacity_)throw std::runtime_error("Main block identity mismatch");
  const auto extent=block.inspect();
  for(auto offset=ExperienceBlock::header_bytes;offset<extent.complete_bytes;){
   const auto location=block.location_at(offset);const auto stored=block.read(location,metadata+record_size,memory_);
   check_metadata(stored);const auto data=stored.view().content;
   if(data.size()!=record_size||std::memcmp(data.data(),"SWGCMRG1",8))throw std::runtime_error("invalid Main merge record");
   Record expected{get(data,8),get(data,128),get(data,136),get_digest(data,16),get_digest(data,144),get_address(data,48),get_address(data,176)};
   if(expected.generation!=graph_.generation()+1||expected.parent!=head_||stored.view().sequence!=expected.generation||stored.view().observed_at_ns!=expected.step)
    throw std::runtime_error("Main merge lineage mismatch");
   const auto& source=resolver.resolve(expected.source);
   if(!graph_.merge_impl(source,expected.seed,expected.step,verify,&expected))throw std::runtime_error("duplicate or empty Main merge record");
   head_=location;offset+=location.bytes;
  }
  ++next_index_;
 }
 // Reopened writers always roll to a new block; incomplete tails are retained.
}

}  // namespace swegca::vrs
