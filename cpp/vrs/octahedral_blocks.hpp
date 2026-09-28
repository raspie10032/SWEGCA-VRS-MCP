#pragma once
#include "vrs/block_collectors.hpp"
#include "swegca_architecture/region_partition_kernel.hpp"
#include "swegca_architecture/association_kernel.hpp"
#include "swegca_architecture/ternary_count_kernel.hpp"
#include "swegca_architecture/head_publication_kernel.hpp"
#include <map>
#include <set>
#include <memory>
#include <shared_mutex>
#include <span>
#include <atomic>

namespace swegca::vrs {
// VRS topology and ownership. Memory activation remains a separate consumer.
// No all-pairs array is allocated: only caller-observed relations are routed.
class OctahedralBlocks final {
public:
 using Id=architecture::kernel::Digest;
 using Counts=architecture::kernel::TernaryCount;
 using Verdict=architecture::kernel::AssociationJudgment;
 struct Limits {std::size_t experiences,relations,collector_workers,queued_batches;};
 struct Pair {Id left,right;auto operator<=>(const Pair&)const=default;};
 struct Work {Pair pair;architecture::kernel::AssociationEvidence evidence;std::uint64_t delivery;};
 struct Applied {Pair pair;Counts previous,current;Verdict judgment;std::uint64_t delivery;};
 using Verify=std::function<std::vector<Verdict>(std::span<const Work>)>;
 // One callback at a time per physical relation block. Different blocks run
 // concurrently and must use independent writers. Throwing poisons publication.
 using Persist=std::function<void(std::size_t,std::span<const Applied>)>;
 struct Location {std::size_t block,slot;};
 struct RelationRef {std::size_t block,slot;Pair pair;};
 struct Candidate {Id experience;architecture::RecordAddress original;Counts strength;};
 struct Snapshot {std::size_t experience_blocks,relation_blocks,experiences,relations;};
 OctahedralBlocks(Limits limits,Persist persist)
  :limits_(limits),collectors_(1,limits.collector_workers,limits.queued_batches),persist_(std::move(persist)){
  if(!limits.experiences||!limits.relations||!persist_)throw std::invalid_argument("VRS block limits/persistence");
 }
 ~OctahedralBlocks(){collectors_.drain();}
 OctahedralBlocks(const OctahedralBlocks&)=delete;
 // Upper first-level orchestrator: address partitioning BEFORE candidate work.
 // This is volatile placement, not an experience write or a semantic judgment.
 Location place(const Id& id){
  if(!architecture::kernel::named_digest(id))throw std::invalid_argument("unnamed experience");
  std::unique_lock lock(topology_);return place_locked(id);
 }
 // The codec owner supplies this AFTER verification/persistence, never to
 // authorize a verdict. Stable IDs keep cross-block endpoints intact.
 void publish_original(const Id& id,const architecture::RecordAddress& address){
  if(!architecture::kernel::head_address_valid(address))throw std::invalid_argument("invalid original address");
  std::unique_lock lock(topology_);const auto it=locations_.find(id);if(it==locations_.end())throw std::invalid_argument("unplaced original");
  auto& original=experiences_[it->second.block][it->second.slot].original;
  if(original&&*original!=address)throw std::logic_error("original address already published");
  original=address;
 }
 // Upper second-level orchestrator: gives each bounded block its own work
 // batch. The provided executor allocates that batch to available SWEGCA
 // execution resources. It must return actual typed core results.
 void submit(std::span<const Work> incoming,const Verify& verify){
  if(!verify)throw std::invalid_argument("missing SWEGCA executor");
  std::map<std::size_t,std::vector<std::pair<std::size_t,Work>>> routed;
  {
   std::unique_lock lock(topology_);
   for(const auto& work:incoming){if(work.pair.left==work.pair.right)throw std::invalid_argument("self relation");
    place_locked(work.pair.left);place_locked(work.pair.right);
    auto relation=route_locked(work.pair);routed[relation.block].push_back({relation.slot,work});
   }
  }
  // Block jobs are independent; the callback only collects this block's core
  // results. Caller may invoke submit concurrently for independent inputs.
  for(auto& [block,items]:routed){
   std::vector<Work> work;work.reserve(items.size());for(auto& item:items)work.push_back(item.second);
   auto judgments=verify(work);if(judgments.size()!=work.size())throw std::runtime_error("SWEGCA result count");
   std::shared_ptr<RelationBlock> owner;
   {std::shared_lock lock(topology_);owner=relations_.at(block);}
   collectors_.submit(owner->collector,[this,owner,block,items=std::move(items),judgments=std::move(judgments)]{
    std::unique_lock lock(owner->mutex);
    // Prepare against current block state: no stale strength snapshot is
    // installed. Duplicate transport deliveries do not reinforce twice.
    std::vector<Applied> changes;changes.reserve(items.size());
    std::map<std::size_t,Counts> pending;
    std::map<std::size_t,std::uint64_t> deliveries;
    for(std::size_t i=0;i<items.size();++i){const auto slot=items[i].first;const auto& w=items[i].second;auto& edge=owner->edges.at(slot);
     const auto seen=deliveries.find(slot);
     const auto previous_delivery=seen==deliveries.end()?edge.last_delivery:std::optional<std::uint64_t>(seen->second);
     if(previous_delivery&&w.delivery==*previous_delivery)continue;
     if(previous_delivery&&w.delivery<*previous_delivery)throw std::runtime_error("out-of-order block delivery");
     deliveries[slot]=w.delivery;
     auto found=pending.find(slot);const auto before=found==pending.end()?edge.counts:found->second;auto after=before;
     if(!architecture::kernel::accumulate_verdict(after,judgments[i].status()))throw std::overflow_error("VRS counts exhausted");
     pending[slot]=after;changes.push_back({w.pair,before,after,judgments[i],w.delivery});
    }
    if(changes.empty())return;
    persist_(block,changes); // This block's writer, not a central writer.
    for(const auto& [slot,count]:pending)owner->edges[slot].counts=count;
    for(std::size_t i=0;i<items.size();++i){auto& edge=owner->edges[items[i].first];auto delivery=items[i].second.delivery;if(!edge.last_delivery||delivery>*edge.last_delivery)edge.last_delivery=delivery;}
   });
  }
 }
 void finish(){collectors_.finish();}
 Snapshot snapshot()const{std::shared_lock lock(topology_);return {experiences_.size(),relations_.size(),locations_.size(),edge_index_.size()};}
 // Lower pyramid: the separate memory path starts from a known experience
 // key, follows explicit portal links and receives only connected addresses.
 // No whole-VRS payload read, cross product, or latest-verdict filter.
 std::vector<Candidate> connected(const Id& id)const{
  std::vector<RelationRef> links;
  {std::shared_lock lock(topology_);auto found=portals_.find(id);if(found==portals_.end())return {};links=found->second;}
  std::vector<Candidate> result;
  for(const auto& ref:links){std::shared_ptr<RelationBlock> block;{std::shared_lock lock(topology_);block=relations_.at(ref.block);}
   Counts counts;bool measured=false;{std::shared_lock lock(block->mutex);const auto& edge=block->edges.at(ref.slot);counts=edge.counts;measured=edge.last_delivery.has_value();}
   if(!measured||!architecture::kernel::count_evidence_eligible(counts))continue;
   const auto other=ref.pair.left==id?ref.pair.right:ref.pair.left;
   std::shared_lock lock(topology_);const auto loc=locations_.at(other);const auto& original=experiences_[loc.block][loc.slot].original;
   if(original)result.push_back({other,*original,counts});
  }
  return result;
 }
 Counts counts(const Pair& pair)const{std::shared_ptr<RelationBlock> block;std::size_t slot;
  {std::shared_lock lock(topology_);auto ref=edge_index_.at(pair);block=relations_[ref.block];slot=ref.slot;}
  std::shared_lock lock(block->mutex);return block->edges.at(slot).counts;
 }
private:
 struct Experience {Id id;std::optional<architecture::RecordAddress> original;};
 struct Edge {Pair pair;Counts counts;std::optional<std::uint64_t> last_delivery;};
 struct RelationBlock {std::size_t collector;mutable std::shared_mutex mutex;std::vector<Edge> edges;};
 Location place_locked(const Id& id){
  if(!architecture::kernel::named_digest(id))throw std::invalid_argument("unnamed experience");
  if(auto found=locations_.find(id);found!=locations_.end())return found->second;
  // The core supplies the bounded partition size; no global N*N reservation.
  auto end=architecture::kernel::region_partition_end(0,limits_.experiences,limits_.experiences);
  if(!end)throw std::logic_error("invalid core partition");
  if(experiences_.empty()||experiences_.back().size()==*end)experiences_.emplace_back();
  Location loc{experiences_.size()-1,experiences_.back().size()};experiences_.back().push_back({id,{}});locations_.emplace(id,loc);return loc;
 }
 RelationRef route_locked(const Pair& pair){
  if(auto found=edge_index_.find(pair);found!=edge_index_.end())return found->second;
  const auto left=locations_.at(pair.left).block,right=locations_.at(pair.right).block;
  auto& pages=tiles_[{left,right}];std::shared_ptr<RelationBlock> page;
  if(!pages.empty())page=relations_[pages.back()];
  if(!page||page->edges.size()==limits_.relations){page=std::make_shared<RelationBlock>();page->collector=collectors_.add_block();page->edges.reserve(limits_.relations);pages.push_back(relations_.size());relations_.push_back(page);}
  std::unique_lock lock(page->mutex);RelationRef ref{pages.back(),page->edges.size(),pair};page->edges.push_back({pair,{},{}});
  edge_index_.emplace(pair,ref);portals_[pair.left].push_back(ref);portals_[pair.right].push_back(ref);return ref;
 }
 Limits limits_;BlockCollectors collectors_;Persist persist_;mutable std::shared_mutex topology_;
 std::vector<std::vector<Experience>> experiences_;std::map<Id,Location> locations_;
 std::map<std::pair<std::size_t,std::size_t>,std::vector<std::size_t>> tiles_;
 std::vector<std::shared_ptr<RelationBlock>> relations_;std::map<Pair,RelationRef> edge_index_;
 std::map<Id,std::vector<RelationRef>> portals_;
};
}
