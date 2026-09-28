#include "vrs/block_store.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/session_kernel.hpp"
#include <fstream>
#include <charconv>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstring>
namespace swegca::vrs {
namespace {
using namespace architecture;using namespace architecture::kernel;
constexpr std::size_t header=24,entry=160;
constexpr std::string_view magic="SWGCBLK1",media="application/x-swegca-block-batch-v1",source="vrs-block-collector",session="vrs-block";
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t v){for(unsigned i=0;i<8;++i)bytes[at+i]=std::byte(v>>(8*i));}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(std::to_integer<unsigned>(bytes[at+i]))<<(8*i);return v;}
void putid(std::span<std::byte> bytes,std::size_t at,const DigestBytes& id){std::copy(id.begin(),id.end(),bytes.begin()+at);}
DigestBytes getid(std::span<const std::byte> bytes,std::size_t at){DigestBytes id;std::copy_n(bytes.begin()+at,32,id.begin());return id;}
std::uint64_t number(std::string_view name){std::uint64_t n;auto [end,error]=std::from_chars(name.data(),name.data()+name.size(),n);if(error!=std::errc{}||end!=name.data()+name.size()||std::to_string(n)!=name)throw std::runtime_error("invalid block filename");return n;}
std::vector<std::byte> encode(std::size_t block,std::size_t count){if(count>(SIZE_MAX-header)/entry)throw std::length_error("block batch too large");std::vector<std::byte> out(header+entry*count);std::memcpy(out.data(),magic.data(),8);put(out,8,block);put(out,16,count);return out;}
void apply(BlockStore::State& state,std::span<const std::byte> b,std::size_t id,std::size_t originals_limit,std::size_t connections_limit){
 if(b.size()<header||std::memcmp(b.data(),magic.data(),8)||get(b,8)!=id||get(b,16)!=(b.size()-header)/entry||(b.size()-header)%entry)throw std::runtime_error("invalid VRS block batch");
 SessionPhase next;
 if(!next_session_phase(state.sealed?SessionPhase::ended:SessionPhase::active,SessionOperation::append,next))throw std::logic_error("sealed VRS block is immutable");
 for(std::size_t at=header;at<b.size();at+=entry){const auto kind=get(b,at);
  if(kind==3){
   if(get(b,16)!=1||!next_session_phase(SessionPhase::active,SessionOperation::end,next))throw std::runtime_error("invalid block seal");
   state.sealed=next==SessionPhase::ended;continue;
  }
  auto left=getid(b,at+8);if(!named_digest(left))throw std::runtime_error("unnamed block endpoint");
  if(kind==1){RecordAddress address{getid(b,at+72),get(b,at+104),get(b,at+112),getid(b,at+120)};if(!head_address_valid(address))throw std::runtime_error("invalid original binding");auto [where,inserted]=state.originals.emplace(left,address);if(!inserted&&where->second!=address)throw std::runtime_error("original binding changed");}
  else if(kind==2){auto right=getid(b,at+40);if(!named_digest(right)||left==right)throw std::runtime_error("invalid relation endpoints");BlockStore::Pair pair{left,right};const TernaryCount before{get(b,at+72),get(b,at+80),get(b,at+88)},after{get(b,at+96),get(b,at+104),get(b,at+112)};const auto delivery=get(b,at+120),status=get(b,at+128);if(status>2)throw std::runtime_error("invalid verdict encoding");
   auto found=state.connections.find(pair);if(found!=state.connections.end()&&(found->second.counts!=before||delivery<=found->second.delivery))throw std::runtime_error("stale/duplicate block update");if(found==state.connections.end()&&before!=TernaryCount{})throw std::runtime_error("missing connection history");
   auto expected=before;if(!accumulate_verdict(expected,static_cast<EvidenceStatus>(status))||expected!=after)throw std::runtime_error("count transition not produced by core verdict");
   state.connections.insert_or_assign(pair,BlockStore::Connection{after,delivery,static_cast<EvidenceStatus>(status)});
  }else throw std::runtime_error("unknown block entry");
  if(state.originals.size()>originals_limit||state.connections.size()>connections_limit)throw std::length_error("logical block entry limit");
 }
 if(state.batches==UINT64_MAX)throw std::overflow_error("block sequence exhausted");
 ++state.batches;
}
}
struct BlockStore::Impl {
 struct Owner {std::mutex mutex;State state;bool loaded=false,failed=false;std::uint64_t next=0,tail=ExperienceBlock::header_bytes;std::optional<ExperienceBlock> writer;};
 std::filesystem::path root;Config config;int lockfd=-1;std::unique_ptr<StorageBudget> storage;
 mutable std::mutex directory_mutex;std::map<std::size_t,std::shared_ptr<Owner>> owners;
 Impl(std::filesystem::path path,Config c):root(std::move(path)),config(c){
  if(!c.max_originals||!c.max_connections||c.segment_bytes<header+entry+ExperienceBlock::record_overhead+ExperienceBlock::header_bytes+source.size()+session.size()+media.size())throw std::invalid_argument("block store limits");
  std::filesystem::create_directories(root);lockfd=open((root/"writer.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
  if(lockfd<0)throw std::runtime_error("block store lock open");
  if(flock(lockfd,LOCK_EX|LOCK_NB)){close(lockfd);lockfd=-1;throw std::runtime_error("block store already owned");}
  try{std::uint64_t bytes=std::filesystem::exists(root/"layout.block")?std::filesystem::file_size(root/"layout.block"):0;
   for(auto& dir:std::filesystem::directory_iterator(root)){auto name=dir.path().filename().string();if(!dir.is_directory()||!name.starts_with("block-"))continue;const auto id=number(std::string_view(name).substr(6));owners.emplace(id,std::make_shared<Owner>());
    for(auto& file:std::filesystem::directory_iterator(dir.path()))if(file.path().extension()==".block"||file.path().extension()==".payload"){auto n=file.file_size();if(n>UINT64_MAX-bytes)throw std::overflow_error("store size");bytes+=n;}
   }
   storage=std::make_unique<StorageBudget>(c.storage_bytes,bytes);
   const auto manifest=root/"layout.block";
   if(std::filesystem::exists(manifest)){
    auto block=ExperienceBlock::open_reader(manifest);auto extent=block.inspect();if(extent.unfinished_bytes||extent.complete_records!=1)throw std::runtime_error("incomplete block layout");MemoryBudget memory(4096);auto row=block.read(block.location_at(ExperienceBlock::header_bytes),4096,memory);auto b=row.view().content;
    if(b.size()!=32||std::memcmp(b.data(),"SWGCLAY1",8)||get(b,8)!=config.segment_bytes||get(b,16)!=config.max_originals||get(b,24)!=config.max_connections)throw std::runtime_error("block layout configuration mismatch");
   }else {if(!owners.empty())throw std::runtime_error("missing block layout");std::array<std::byte,32> b{};std::memcpy(b.data(),"SWGCLAY1",8);put(b,8,config.segment_bytes);put(b,16,config.max_originals);put(b,24,config.max_connections);auto block=ExperienceBlock::create(manifest,Sha256::of(std::as_bytes(std::span(root.native()))),4096,storage.get());(void)block.append({0,0,session,source,"application/x-swegca-layout-v1",b});}
  }catch(...){close(lockfd);lockfd=-1;throw;}
 }
 ~Impl(){if(lockfd>=0)close(lockfd);}
 std::shared_ptr<Owner> owner(std::size_t id){std::lock_guard lock(directory_mutex);auto& value=owners[id];if(!value)value=std::make_shared<Owner>();return value;}
 std::filesystem::path directory(std::size_t id)const{return root/("block-"+std::to_string(id));}
 void recover(std::size_t id,Owner& o){
  if(o.loaded)return;
  auto dir=directory(id);if(!std::filesystem::exists(dir)){o.loaded=true;return;}
  std::map<std::uint64_t,std::filesystem::path> paths;for(auto& file:std::filesystem::directory_iterator(dir))if(file.path().extension()==".block")paths.emplace(number(file.path().stem().string()),file.path());
  State recovered;std::optional<ExperienceBlock> writable;std::uint64_t next=0;
  for(auto& [index,path]:paths){if(index!=next++)throw std::runtime_error("missing block segment");auto block=ExperienceBlock::open_reader(path);if(block.capacity()!=config.segment_bytes)throw std::runtime_error("segment capacity mismatch");auto extent=block.inspect();MemoryBudget memory(config.segment_bytes);std::uint64_t at=ExperienceBlock::header_bytes;
   for(std::uint64_t i=0;i<extent.complete_records;++i){auto address=block.location_at(at);auto stored=block.read(address,config.segment_bytes,memory);auto v=stored.view();if(v.sequence!=recovered.batches||v.source!=source||v.session!=session||v.media_type!=media)throw std::runtime_error("block batch lineage");apply(recovered,v.content,id,config.max_originals,config.max_connections);at+=address.bytes;}
   // Preserve a torn tail. Future appends go into a NEW bounded segment.
   if(index==paths.rbegin()->first&&!extent.unfinished_bytes&&!recovered.sealed){writable.emplace(ExperienceBlock::open_writer(path,storage.get()));o.tail=extent.complete_bytes;}
  }
  o.state=std::move(recovered);o.next=next;o.writer=std::move(writable);o.loaded=true;
 }
 void append(std::size_t id,std::vector<std::byte> body){auto o=owner(id);std::lock_guard lock(o->mutex);if(o->failed)throw std::runtime_error("block writer requires reopen");recover(id,*o);
  const auto size=ExperienceBlock::record_overhead+session.size()+source.size()+media.size()+body.size();if(size>config.segment_bytes-ExperienceBlock::header_bytes)throw std::length_error("batch exceeds segment; submit smaller batches");
  auto prepared=o->state;apply(prepared,body,id,config.max_originals,config.max_connections);
  try{
   if(!o->writer||o->tail>config.segment_bytes-size){o->writer.reset();auto dir=directory(id);std::filesystem::create_directories(dir);const auto path=dir/(std::to_string(o->next)+".block");Sha256 h;h.update(root.string());h.update(path.filename().string());h.update(std::to_string(id));o->writer.emplace(ExperienceBlock::create(path,h.finish(),config.segment_bytes,storage.get()));++o->next;o->tail=ExperienceBlock::header_bytes;
    int fd=open(root.c_str(),O_DIRECTORY|O_RDONLY|O_CLOEXEC);if(fd<0)throw std::runtime_error("block root open");int result=fsync(fd);close(fd);if(result)throw std::runtime_error("block root sync");
   }
   const auto address=o->writer->append({o->state.batches,0,session,source,media,body});o->tail=address.offset+address.bytes;o->state=std::move(prepared);if(o->state.sealed)o->writer.reset();
  }catch(...){o->failed=true;throw;}
 }
};
BlockStore::BlockStore(const std::filesystem::path& root,Config config):impl_(std::make_unique<Impl>(root,config)){}
BlockStore::~BlockStore()=default;
void BlockStore::append(std::size_t block,std::span<const OctahedralBlocks::Applied> updates){commit(block,{},updates);}
void BlockStore::originals(std::size_t block,std::span<const std::pair<Id,architecture::RecordAddress>> items){commit(block,items,{});}
void BlockStore::commit(std::size_t block,std::span<const std::pair<Id,architecture::RecordAddress>> items,std::span<const OctahedralBlocks::Applied> updates){
 if(items.empty()&&updates.empty())return;
 if(items.size()>impl_->config.max_originals||updates.size()>impl_->config.max_connections)throw std::length_error("batch entry limit");
 auto b=encode(block,items.size()+updates.size());
 for(std::size_t i=0;i<items.size();++i){const auto& [id,a]=items[i];auto at=header+i*entry;put(b,at,1);putid(b,at+8,id);putid(b,at+72,a.block);put(b,at+104,a.offset);put(b,at+112,a.bytes);putid(b,at+120,a.digest);}
 for(std::size_t i=0;i<updates.size();++i){const auto& u=updates[i];auto at=header+(items.size()+i)*entry;put(b,at,2);putid(b,at+8,u.pair.left);putid(b,at+40,u.pair.right);put(b,at+72,u.previous.accept);put(b,at+80,u.previous.reject);put(b,at+88,u.previous.abstain);put(b,at+96,u.current.accept);put(b,at+104,u.current.reject);put(b,at+112,u.current.abstain);put(b,at+120,u.delivery);put(b,at+128,static_cast<unsigned>(u.judgment.status()));}
 impl_->append(block,std::move(b));
}
void BlockStore::seal(std::size_t block){
 if(read(block).sealed)return;
 auto body=encode(block,1);put(body,header,3);impl_->append(block,std::move(body));
}
BlockStore::State BlockStore::read(std::size_t block){auto o=impl_->owner(block);std::lock_guard lock(o->mutex);if(o->failed)throw std::runtime_error("block writer requires reopen");impl_->recover(block,*o);return o->state;}
std::vector<std::size_t> BlockStore::blocks()const{std::lock_guard lock(impl_->directory_mutex);std::vector<std::size_t> out;for(auto& [id,owner]:impl_->owners){(void)owner;out.push_back(id);}return out;}
StorageBudget& BlockStore::storage_budget(){return *impl_->storage;}
std::uint64_t BlockStore::storage_used()const{return impl_->storage->used();}
}
