#pragma once
#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/region_partition_kernel.hpp"
#include "vrs/memory_budget.hpp"
#include <algorithm>
#include <map>
#include <vector>
#include <stdexcept>
#include <limits>

namespace swegca::vrs {
// Main-owned address regions. Entries retain their existing graph connections,
// original references and core reports. Splitting does not copy experience data.
// Preparation is read-only; commit transfers preallocated std::map nodes.
template<class Entry>
class ConnectionRegions final {
    using Key=architecture::DigestBytes;
public:
    using Entries=std::pmr::map<Key,Entry>;
private:
    struct Region {
        explicit Region(MemoryBudget& memory):entries(&memory){}
        Entries entries;
    };
    using Regions=std::pmr::map<Key,Region>;
public:
    class Prepared final {
        friend class ConnectionRegions;
        explicit Prepared(const ConnectionRegions& owner):owner_(&owner),regions_(&owner.memory_),replaced_(&owner.memory_){}
        const ConnectionRegions* owner_;
        Regions regions_;
        std::pmr::vector<Key> replaced_;
    public:
        Prepared(const Prepared&)=delete;
        Prepared& operator=(const Prepared&)=delete;
        Prepared(Prepared&&)=default;
    };
    explicit ConnectionRegions(MemoryBudget& memory,std::size_t capacity=256)
        :memory_(memory),capacity_(capacity),regions_(&memory){
        if(!capacity)throw std::invalid_argument("connection region capacity must be positive");
    }
    ConnectionRegions(const ConnectionRegions&)=delete;
    ConnectionRegions& operator=(const ConnectionRegions&)=delete;
    [[nodiscard]] const Entry* find(const Key& key) const noexcept{
        if(regions_.empty())return nullptr;
        const auto region=std::prev(regions_.upper_bound(key));
        const auto entry=region->second.entries.find(key);
        return entry==region->second.entries.end()?nullptr:&entry->second;
    }
    [[nodiscard]] std::size_t region_count() const noexcept{return regions_.size();}
    [[nodiscard]] std::size_t largest_region() const noexcept{
        std::size_t largest=0;for(const auto& [key,region]:regions_){(void)key;largest=std::max(largest,region.entries.size());}return largest;
    }
    [[nodiscard]] Prepared prepare(const Entries& pending) const {
        if(pending.get_allocator().resource()!=&memory_)throw std::invalid_argument("region allocator mismatch");
        Prepared plan(*this);
        auto begin=pending.begin();
        while(begin!=pending.end()){
            const auto region=regions_.empty()?regions_.end():std::prev(regions_.upper_bound(begin->first));
            const auto next=region==regions_.end()?regions_.end():std::next(region);
            const auto end=next==regions_.end()?pending.end():pending.lower_bound(next->first);
            // Both maps are already ordered. Walk their distinct union rather
            // than copying every digest into a second, batch-sized directory.
            const Entries* existing=region==regions_.end()?nullptr:&region->second.entries;
            const auto visit=[&](auto&& consume){
                auto incoming=begin;
                const auto old_end=existing?existing->end():typename Entries::const_iterator{};
                auto old=existing?existing->begin():old_end;
                while(incoming!=end||old!=old_end){
                    if(old==old_end){consume(incoming->first);++incoming;}
                    else if(incoming==end){consume(old->first);++old;}
                    else if(old->first<incoming->first){consume(old->first);++old;}
                    else {
                        consume(incoming->first);
                        if(old->first==incoming->first)++old;
                        ++incoming;
                    }
                }
            };
            std::size_t count=0;
            visit([&](const Key&){
                if(count==std::numeric_limits<std::size_t>::max())
                    throw std::length_error("connection region count overflow");
                ++count;
            });
            if(region==regions_.end()||count>capacity_){
                const Key lower=region==regions_.end()?Key{}:region->first;
                if(region!=regions_.end())plan.replaced_.push_back(lower);
                std::size_t offset=0,next_boundary=0;
                visit([&](const Key& key){
                    if(offset==next_boundary){
                        const auto boundary=architecture::kernel::region_partition_end(offset,count,capacity_);
                        if(!boundary)throw std::logic_error("core rejected connection partition");
                        plan.regions_.try_emplace(offset?key:lower,memory_);
                        next_boundary=*boundary;
                    }
                    ++offset;
                });
            }
            begin=end;
        }
        return plan;
    }
    // Caller validates its prepared Main generation before durable publication.
    // Only the originating pending key set may be committed. No allocation,
    // evidence evaluation or fallible I/O is performed after publication.
    void commit(Prepared& plan,Entries& pending) noexcept {
        if(plan.owner_!=this||pending.get_allocator().resource()!=&memory_)std::terminate();
        for(const auto& [key,entry]:pending){
            (void)entry;
            if(!regions_.empty())std::prev(regions_.upper_bound(key))->second.entries.erase(key);
        }
        for(const auto& lower:plan.replaced_){
            const auto old=regions_.find(lower);
            while(!old->second.entries.empty()){
                auto node=old->second.entries.extract(old->second.entries.begin());
                auto destination=std::prev(plan.regions_.upper_bound(node.key()));
                destination->second.entries.insert(std::move(node));
            }
            regions_.erase(old);
        }
        regions_.merge(plan.regions_);
        while(!pending.empty()){
            auto node=pending.extract(pending.begin());
            auto destination=std::prev(regions_.upper_bound(node.key()));
            destination->second.entries.insert(std::move(node));
        }
        plan.owner_=nullptr;
    }
private:
    MemoryBudget& memory_;
    std::size_t capacity_;
    Regions regions_;
};
} // namespace swegca::vrs
