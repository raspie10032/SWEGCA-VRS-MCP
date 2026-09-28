#include "vrs/block_ingress.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include <sstream>
#include <charconv>
#include <limits>
#include <random>
namespace swegca::vrs {
using namespace architecture;using namespace architecture::kernel;
struct BlockIngress::Impl {
 struct Writer {std::optional<ExperienceBlock> file;std::uint64_t segment=0,tail=0,sequence=0;bool failed=false,sealed=false;std::size_t assigned=0;std::vector<DigestBytes> members;std::map<BlockStore::Pair,BlockStore::Connection> connections;};
 Config config;std::filesystem::path root;MemoryBudget memory;BlockStore store;Notify notify;
 mutable std::mutex topology;std::map<DigestBytes,std::size_t> placements;std::map<std::size_t,std::shared_ptr<Writer>> writers;
 std::map<DigestBytes,RecordAddress> originals;
 std::map<DigestBytes,std::vector<std::pair<std::size_t,BlockStore::Pair>>> portals;
 std::set<std::size_t> available_blocks;std::size_t next_block=0;bool finished=false;
 std::vector<std::shared_ptr<DeviceEvidence>> devices;std::unique_ptr<Flow> flow;
 static std::filesystem::path validated_root(const std::filesystem::path& path){
  if(std::filesystem::exists(path)&&!std::filesystem::is_empty(path)&&!std::filesystem::is_regular_file(path/"layout.block"))throw std::runtime_error("unrecognized ingress root; refusing to overwrite");return path;
 }
 Impl(const std::filesystem::path& path,Config c,Notify n):config(c),root(validated_root(path)),memory(c.memory_bytes),store(root,{64ULL<<20,c.originals_per_block,c.storage_bytes}),notify(std::move(n)){
  if(!c.originals_per_block||!c.cpu_workers||!notify||c.gpu_devices<0)throw std::invalid_argument("block ingress configuration");
  if(c.mix_unknown&&c.originals_per_block>256)throw std::invalid_argument("mixed all-pair blocks must fit connection page limit (<=256 originals)");
  recover();
  std::vector<Flow::Backend> backends;
  const auto rules=make_evidence_rules({});
  auto backend=[&,rules](std::string name,std::shared_ptr<DeviceEvidence> device,std::size_t capacity){
   backends.push_back({name,1,std::max<std::size_t>(1,std::min(capacity,c.queue)),[rules,device,name](auto batch){
    std::vector<EvidenceTally> tallies;std::vector<std::size_t> slots;
    for(std::size_t i=0;i<batch.size();++i)if(!batch[i]->duplicate&&batch[i]->error.empty()){slots.push_back(i);tallies.push_back(batch[i]->request.evidence);}
    std::vector<EvidenceJudgment> judgments(tallies.size());
    if(device)device->judge(rules,tallies,judgments);else cpu_evidence_batch(rules,tallies,judgments);
    std::vector<BlockFileVerified> out;for(auto& item:batch)out.push_back({item,{},name});
    for(std::size_t i=0;i<slots.size();++i)out[slots[i]].judgment=judgments[i];
    std::vector<AssociationEvidence> associations;for(auto i:slots)for(const auto& r:batch[i]->request.relations)associations.push_back(r.evidence);
    std::vector<AssociationJudgment> decisions(associations.size());
    if(device)device->associate(associations,decisions);else for(std::size_t i=0;i<associations.size();++i)decisions[i]=judge_association(associations[i]);
    std::size_t at=0;for(auto i:slots){const auto count=batch[i]->request.relations.size();out[i].relations.assign(decisions.begin()+at,decisions.begin()+at+count);at+=count;}
    return out;
   }});
  };
  if(c.gpu_devices){const auto available=DeviceEvidence::available();if(available<c.gpu_devices)throw std::runtime_error("requested GPUs unavailable");
   for(int i=0;i<c.gpu_devices;++i){auto device=std::make_shared<DeviceEvidence>(i);devices.push_back(device);backend("cuda:"+std::to_string(i),device,device->capacity());}}
  for(std::size_t i=0;i<c.cpu_workers;++i)backend("cpu:"+std::to_string(i),{},4096);
  flow=std::make_unique<Flow>(Flow::Config{c.codec_workers,c.queue,c.queue,c.queue,c.batch,std::chrono::milliseconds(10)},[this](BlockFileRequest request){
   auto decoded=std::make_shared<BlockFileDecoded>();decoded->request=std::move(request);try{decoded->input=decode_file(decoded->request.path,memory);}catch(const std::exception& error){
    decoded->error=error.what();decoded->block=std::numeric_limits<std::size_t>::max();return decoded;
   }
   // Source path is provenance, not experience identity. An attached observation
   // changes the experience; identical transport deliveries do not count twice.
   Sha256 h;h.update(decoded->input->identity);h.update(decoded->request.observation);
   // Hash typed evidence too: callers using the native API need not provide a
   // textual copy, and different observations must never deduplicate together.
   std::ostringstream identity;identity.precision(17);
   for(const auto& relation:decoded->request.relations){h.update(relation.peer);identity<<relation.evidence.support<<':'<<relation.evidence.refute<<';';}
   const auto& evidence=decoded->request.evidence;
   for(std::size_t axis=0;axis<max_axes;++axis)identity<<evidence.axis_support[axis]<<':'<<evidence.axis_refute[axis]<<':'<<evidence.axis_source_diversity[axis]<<';';
   identity<<evidence.source_diversity<<':'<<evidence.context_diversity<<':'<<evidence.recent_count<<':'<<evidence.recent_sum<<':'<<evidence.revision;
   h.update(identity.str());decoded->identity=h.finish();
   for(const auto& relation:decoded->request.relations)if(!named_digest(relation.peer)||relation.peer==decoded->identity)throw std::invalid_argument("invalid relation peer");
   {std::lock_guard lock(topology);auto found=placements.find(decoded->identity);
    if(found!=placements.end()){decoded->block=found->second;decoded->duplicate=true;}
    else {
     const auto boundary=region_partition_end(0,config.originals_per_block,config.originals_per_block);if(!boundary)throw std::logic_error("invalid SWEGCA block partition");
     if(available_blocks.empty()){
      if(next_block==std::numeric_limits<std::size_t>::max())throw std::overflow_error("block IDs exhausted");
      const auto block=next_block++;writers.emplace(block,std::make_shared<Writer>());available_blocks.insert(block);
     }
     decoded->block=*available_blocks.begin();auto& owner=*writers.at(decoded->block);
     if(owner.sealed||owner.assigned>=*boundary)throw std::logic_error("invalid writable block allocation");
     if(++owner.assigned==*boundary)available_blocks.erase(decoded->block);
     if(config.mix_unknown){
      // Enumerate unmeasured pair hypotheses too. No bytes/tags are fabricated
      // as evidence; the core will abstain while the observation is absent.
      for(const auto& peer:owner.members){
       const bool supplied=std::any_of(decoded->request.relations.begin(),decoded->request.relations.end(),[&](const auto& r){return r.peer==peer;});
       if(!supplied)decoded->request.relations.push_back({peer,{}});
      }
      thread_local std::mt19937_64 shuffle(std::random_device{}());
      std::shuffle(decoded->request.relations.begin(),decoded->request.relations.end(),shuffle);
     }
     owner.members.push_back(decoded->identity);
     placements.emplace(decoded->identity,decoded->block);
    }}
   return decoded;
  },std::move(backends),[this](auto batch){apply(batch);},[](const BlockFileVerified& value){return value.decoded->block;},c.collector_workers);
 }
 void recover(){
  for(const auto block:store.blocks()){
   if(block==std::numeric_limits<std::size_t>::max())throw std::overflow_error("block IDs exhausted");
   next_block=std::max(next_block,block+1);
   const auto state=store.read(block);auto writer=std::make_shared<Writer>();
   writer->connections=state.connections;writer->sealed=state.sealed;writer->assigned=state.originals.size();
   std::map<DigestBytes,std::filesystem::path> payloads;
   for(const auto& entry:std::filesystem::directory_iterator(root/("block-"+std::to_string(block)))){
    if(entry.path().extension()!=".payload")continue;
    const auto name=entry.path().stem().string();std::uint64_t segment;
    auto [end,error]=std::from_chars(name.data(),name.data()+name.size(),segment);
    if(error!=std::errc{}||end!=name.data()+name.size()||std::to_string(segment)!=name||segment==UINT64_MAX)throw std::runtime_error("invalid payload segment");
    writer->segment=std::max(writer->segment,segment+1);
    if(entry.file_size()<ExperienceBlock::header_bytes){
     Sha256 h;h.update(entry.path().string());const auto expected=h.finish();
     for(const auto& [id,address]:state.originals){(void)id;if(address.block==expected)throw std::runtime_error("incomplete committed payload header");}
     continue; // Interrupted creation, no published reference; preserve it.
    }
    const auto file=ExperienceBlock::open_reader(entry.path());
    if(!payloads.emplace(file.identity(),entry.path()).second)throw std::runtime_error("duplicate payload block identity");
   }
   for(const auto& [identity,address]:state.originals){
    const auto found=payloads.find(address.block);if(found==payloads.end())throw std::runtime_error("missing committed original payload");
    auto file=ExperienceBlock::open_reader(found->second);
    if(file.location_at(address.offset)!=address)throw std::runtime_error("committed original frame mismatch");
    if(!placements.emplace(identity,block).second)throw std::runtime_error("duplicate committed experience identity");
    originals.emplace(identity,address);writer->members.push_back(identity);
   }
   for(const auto& [pair,connection]:state.connections){(void)connection;portals[pair.left].push_back({block,pair});portals[pair.right].push_back({block,pair});}
   writers.emplace(block,writer);
   if(!writer->sealed&&writer->assigned<config.originals_per_block)available_blocks.insert(block);
   // Never append over an interrupted payload tail. Recovered live blocks
   // start a new physical segment; old bytes and record addresses stay intact.
  }
 }
 void finish(){
  if(finished)return;
  flow->finish(); // All codecs, verifiers and block collectors have drained.
  BlockCollectors closers(1,config.collector_workers,config.queue);
  for(const auto& [block,writer]:writers)if(!writer->sealed){
   const auto slot=closers.add_block();closers.submit(slot,[this,block,writer]{
    if(writer->failed)throw std::runtime_error("cannot seal failed block");
    store.seal(block);writer->file.reset();
    std::lock_guard lock(topology);writer->sealed=true;
   });
  }
  closers.finish();available_blocks.clear();finished=true;
 }
 std::optional<RecordAddress> original(const DigestBytes& id)const{
  std::lock_guard lock(topology);const auto found=originals.find(id);if(found==originals.end())return {};return found->second;
 }
 std::vector<OctahedralBlocks::Candidate> connected(const DigestBytes& id)const{
  std::lock_guard lock(topology);std::vector<OctahedralBlocks::Candidate> out;
  const auto found=portals.find(id);if(found==portals.end())return out;
  for(const auto& [block,pair]:found->second){const auto& state=writers.at(block)->connections.at(pair);
   if(!count_evidence_eligible(state.counts))continue;
   const auto other=pair.left==id?pair.right:pair.left;const auto address=originals.find(other);
   if(address!=originals.end())out.push_back({other,address->second,state.counts});
  }
  return out;
 }
 void apply(std::span<const BlockFileVerified> batch){
  if(batch.empty())return;
  if(!batch.front().decoded->error.empty()){
   for(const auto& row:batch){Receipt receipt{};receipt.ticket=row.decoded->request.ticket;receipt.error=row.decoded->error;receipt.backend="not_verified";notify(receipt);}return;
  }
  const auto block=batch.front().decoded->block;std::shared_ptr<Writer> writer;
  {std::lock_guard lock(topology);writer=writers.at(block);}
  if(writer->failed)throw std::runtime_error("failed block ingress writer");
  try{
   std::vector<std::pair<BlockStore::Id,RecordAddress>> bindings;
   std::vector<Receipt> receipts;
   std::vector<OctahedralBlocks::Applied> changes;std::map<BlockStore::Pair,BlockStore::Connection> next;
   {std::lock_guard lock(topology);next=writer->connections;}
   std::vector<OriginalExperienceView> records;
   std::vector<std::string> sources,proofs;sources.reserve(batch.size());proofs.reserve(batch.size());
   std::vector<std::pair<std::size_t,std::size_t>> originals;
   constexpr std::string_view session="block-ingress";
   for(const auto& result:batch){const auto& d=*result.decoded;
    if(d.block!=block)throw std::logic_error("mixed block application");
    receipts.push_back({d.request.ticket,block,d.duplicate,{},result.judgment.status(),result.backend,d.input->decoded.size(),d.input->codec_errors.size()});
    receipts.back().identity=d.identity;
    if(d.duplicate){if(const auto address=original(d.identity))receipts.back().original=*address;continue;}
    if(writer->sealed)throw std::logic_error("write to sealed ingress block");
    sources.push_back(d.request.source.empty()?d.request.path.string():d.request.source);
    auto add=[&](std::string_view media,std::span<const std::byte> bytes){records.push_back({writer->sequence+records.size(),0,session,sources.back(),media,bytes});};
    originals.emplace_back(receipts.size()-1,records.size());bindings.emplace_back(d.identity,RecordAddress{});
    add(d.input->source_media,d.input->original);
    for(const auto& view:d.input->decoded)add(view.media,view.bytes);
    std::ostringstream proof;proof<<"ticket="<<d.request.ticket<<"\nstatus="<<unsigned(result.judgment.status())<<"\nreason="<<unsigned(result.judgment.reason())<<"\nbackend="<<result.backend<<"\nsource_path="<<d.request.path.string()<<"\nobservation="<<d.request.observation<<"\n";
    for(std::size_t i=0;i<d.request.relations.size();++i){const auto& relation=d.request.relations[i];BlockStore::Pair pair{d.identity,relation.peer};
     auto& state=next[pair];const auto before=state.counts;
     if(!accumulate_verdict(state.counts,result.relations.at(i).status()))throw std::overflow_error("connection counts exhausted");
     if(state.delivery==UINT64_MAX)throw std::overflow_error("delivery exhausted");++state.delivery;
     changes.push_back({pair,before,state.counts,result.relations[i],state.delivery});
     if(!accumulate_verdict(receipts.back().relations,result.relations[i].status()))throw std::overflow_error("receipt count overflow");
     proof<<"peer=";for(auto byte:relation.peer)proof<<"0123456789abcdef"[std::to_integer<unsigned>(byte)>>4]<<"0123456789abcdef"[std::to_integer<unsigned>(byte)&15];proof<<",support="<<relation.evidence.support<<",refute="<<relation.evidence.refute<<'\n';
    }
    for(const auto& error:d.input->codec_errors)proof<<"codec_error="<<error<<'\n';
    proofs.push_back(proof.str());add("application/x-swegca-input-verdict-v1",std::as_bytes(std::span(proofs.back())));
   }
   if(!records.empty()){
    std::uint64_t bytes=0;
    for(const auto& record:records)for(const auto n:{std::uint64_t(ExperienceBlock::record_overhead),std::uint64_t(record.session.size()),std::uint64_t(record.source.size()),std::uint64_t(record.media_type.size()),std::uint64_t(record.content.size())}){
     if(n>config.storage_bytes-bytes)throw StorageLimit();bytes+=n;
    }
    if(bytes>config.storage_bytes-std::min(config.storage_bytes,ExperienceBlock::header_bytes))throw StorageLimit();
    if(!writer->file||bytes>writer->file->capacity()-writer->tail){
     const auto dir=root/("block-"+std::to_string(block));std::filesystem::create_directories(dir);
     const auto path=dir/(std::to_string(writer->segment++)+".payload");Sha256 h;h.update(path.string());
     writer->file.emplace(ExperienceBlock::create(path,h.finish(),std::max<std::uint64_t>(1ULL<<30,bytes+ExperienceBlock::header_bytes),&store.storage_budget()));writer->tail=ExperienceBlock::header_bytes;
    }
    // One payload fdatasync for the whole collector batch, not per file/view.
    const auto addresses=writer->file->append_batch(records);writer->sequence+=records.size();writer->tail=addresses.back().offset+addresses.back().bytes;
    for(std::size_t i=0;i<originals.size();++i){auto [receipt,row]=originals[i];bindings[i].second=addresses[row];receipts[receipt].original=addresses[row];}
    // One journal record publishes original bindings AND count changes.
    store.commit(block,bindings,changes);
    {std::lock_guard lock(topology);
     for(const auto& [id,address]:bindings)this->originals.emplace(id,address);
     for(const auto& [pair,value]:next)if(!writer->connections.contains(pair)){(void)value;portals[pair.left].push_back({block,pair});portals[pair.right].push_back({block,pair});}
     writer->connections=std::move(next);
    }
   }
   for(const auto& receipt:receipts)notify(receipt);
  }catch(...){writer->failed=true;throw;}
 }

};
BlockIngress::BlockIngress(const std::filesystem::path& root,Config c,Notify n):impl_(std::make_unique<Impl>(root,c,std::move(n))){}
BlockIngress::~BlockIngress()=default;
void BlockIngress::submit(BlockFileRequest r){impl_->flow->submit(std::move(r));}
void BlockIngress::finish(){impl_->finish();}
std::optional<RecordAddress> BlockIngress::original(const DigestBytes& id)const{return impl_->original(id);}
std::vector<OctahedralBlocks::Candidate> BlockIngress::connected(const DigestBytes& id)const{return impl_->connected(id);}
BlockIngress::Flow::Stats BlockIngress::stats()const{return impl_->flow->stats();}
}
