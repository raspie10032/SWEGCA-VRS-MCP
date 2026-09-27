#pragma once
#include "vrs/experience_page.hpp"
#include "swegca_architecture/metadata_residency_kernel.hpp"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <bit>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>
#include <type_traits>
#include <utility>

namespace swegca::vrs {
// Physical metadata segments only. Logical order and the core's shuffled
// sample indices remain unchanged. Existing experience addresses never move.
class ExperienceSequence final {
    static std::size_t chunk_index(std::size_t index) noexcept {
        return index<255?std::bit_width(index+1)-1:8+(index-255)/256;
    }
    static std::size_t chunk_offset(std::size_t index) noexcept {
        return index<255?index-((std::size_t{1}<<chunk_index(index))-1):(index-255)%256;
    }
    struct Chunk {
        struct Backing {
            Backing(ExperiencePage value,const architecture::kernel::EvidenceRules& policy)
                :page(std::move(value)),rules(policy){}
            ExperiencePage page;
            architecture::kernel::EvidenceRules rules;
            mutable std::mutex load_mutex;
        };
        Chunk(MemoryBudget& budget,std::size_t count):memory(budget),capacity(count),
            data(static_cast<ExperienceEvidence*>(memory.allocate(count*sizeof(ExperienceEvidence),alignof(ExperienceEvidence)))) {}
        Chunk(const Chunk&)=delete;
        Chunk& operator=(const Chunk&)=delete;
        ~Chunk(){
            release();
            if(backing){std::destroy_at(backing);memory.deallocate(backing,sizeof(Backing),alignof(Backing));}
        }
        void release() noexcept{
            if(auto* value=data.exchange(nullptr)){
                std::destroy_n(value,used);
                memory.deallocate(value,capacity*sizeof(ExperienceEvidence),alignof(ExperienceEvidence));
            }
        }
        ExperienceEvidence* resident() const {
            if(auto* value=data.load(std::memory_order_acquire))return value;
            std::lock_guard lock(backing->load_mutex);
            if(auto* value=data.load(std::memory_order_relaxed))return value;
            auto* value=static_cast<ExperienceEvidence*>(memory.allocate(capacity*sizeof(ExperienceEvidence),alignof(ExperienceEvidence)));
            try { backing->page.restore_into(value,used,backing->rules,memory); }
            catch(...) {
                memory.deallocate(value,capacity*sizeof(ExperienceEvidence),alignof(ExperienceEvidence));
                throw;
            }
            data.store(value,std::memory_order_release);
            return value;
        }
        MemoryBudget& memory;
        std::size_t capacity,used=0;
        mutable std::atomic<ExperienceEvidence*> data;
        Backing* backing=nullptr;
    };
    class ReadCache final {
        MemoryBudget& memory_;
        std::pmr::vector<ExperienceEvidence> values_;
        const Chunk* cached_=nullptr;
    public:
        explicit ReadCache(MemoryBudget& memory):memory_(memory),values_(&memory){}
        const ExperienceEvidence& read(const Chunk& chunk,std::size_t offset) {
            if(const auto* data=chunk.data.load(std::memory_order_acquire))return data[offset];
            if(cached_!=&chunk){
                cached_=nullptr;
                {decltype(values_) empty(&memory_);values_.swap(empty);}
                values_=chunk.backing->page.load(chunk.backing->rules,memory_);
                if(values_.size()!=chunk.used)throw std::runtime_error("experience traversal count changed");
                cached_=&chunk;
            }
            return values_[offset];
        }
    };
public:
    // Traversal scratch owned by one worker. Cold reads do not promote every
    // shuffled segment into the shared resident graph. References last until
    // the next access; the source and its budget must outlive this reader.
    class Reader final {
        friend class ExperienceSequence;
        explicit Reader(const ExperienceSequence& source)
            :source_(source),cache_(source.memory_){}
        const ExperienceSequence& source_;
        ReadCache cache_;
    public:
        Reader(const Reader&)=delete;
        Reader& operator=(const Reader&)=delete;
        [[nodiscard]] std::size_t size() const noexcept{return source_.size_;}
        [[nodiscard]] const ExperienceEvidence& operator[](std::size_t index){
            if(index>=source_.size_)throw std::out_of_range("experience traversal index");
            const auto number=chunk_index(index);const auto& chunk=source_.chunks_[number];
            return cache_.read(*chunk,chunk_offset(index));
        }
    };
    [[nodiscard]] Reader reader() const{return Reader(*this);}
    // Owner captures a sealed segment, worker writes only a private backing,
    // owner publishes after joining. The extra shared owner protects hot data.
    class PagePreparation final {
        friend class ExperienceSequence;
        PagePreparation(std::shared_ptr<Chunk> chunk,const architecture::kernel::EvidenceRules& rules)
            :chunk_(std::move(chunk)),rules_(rules){}
        std::shared_ptr<Chunk> chunk_;
        architecture::kernel::EvidenceRules rules_;
        Chunk::Backing* prepared_=nullptr;
    public:
        PagePreparation(const PagePreparation&)=delete;
        PagePreparation& operator=(const PagePreparation&)=delete;
        PagePreparation(PagePreparation&& other) noexcept
            :chunk_(std::move(other.chunk_)),rules_(other.rules_),prepared_(std::exchange(other.prepared_,nullptr)){}
        ~PagePreparation(){
            if(prepared_){std::destroy_at(prepared_);chunk_->memory.deallocate(prepared_,sizeof(Chunk::Backing),alignof(Chunk::Backing));}
        }
        void write(const std::filesystem::path& path,const architecture::DigestBytes& identity,StorageBudget* storage){
            if(!chunk_||prepared_)throw std::logic_error("invalid page preparation");
            auto* storage_ptr=static_cast<Chunk::Backing*>(chunk_->memory.allocate(sizeof(Chunk::Backing),alignof(Chunk::Backing)));
            try {
                std::construct_at(storage_ptr,ExperiencePage::create(path,identity,
                    std::span<const ExperienceEvidence>(chunk_->data.load(),chunk_->used),chunk_->memory,storage),rules_);
            }catch(...){chunk_->memory.deallocate(storage_ptr,sizeof(Chunk::Backing),alignof(Chunk::Backing));throw;}
            prepared_=storage_ptr;
        }
        // Serialized owner call only, after worker join and outside Main prepare.
        bool commit(){
            if(!chunk_||!prepared_||chunk_->backing)throw std::logic_error("invalid page publication");
            chunk_->backing=std::exchange(prepared_,nullptr);
            const auto action=architecture::kernel::metadata_release(true,chunk_.use_count()-1,true);
            const bool release=action==architecture::kernel::MetadataRelease::release;
            if(release)chunk_->release();
            chunk_.reset();return release;
        }
    };
    [[nodiscard]] std::optional<PagePreparation> prepare_page(std::size_t index,
        const architecture::kernel::EvidenceRules& rules) const {
        if(index>=size_)throw std::out_of_range("experience page prepare index");
        const auto& chunk=chunks_[chunk_index(index)];
        const auto action=architecture::kernel::metadata_release(chunk->used==chunk->capacity,
            chunk.use_count(),chunk->backing!=nullptr);
        if(action==architecture::kernel::MetadataRelease::retain||!chunk->data.load())return std::nullopt;
        if(action==architecture::kernel::MetadataRelease::release){chunk->release();return std::nullopt;}
        return PagePreparation(chunk,rules);
    }
    [[nodiscard]] std::size_t snapshot_directory_bytes(std::size_t begin,std::size_t end) const {
        if(begin>end||end>size_)throw std::out_of_range("experience snapshot range");
        return begin==end?0:(chunk_index(end-1)-chunk_index(begin)+1)*sizeof(std::shared_ptr<Chunk>);
    }
    // Pins existing sealed values, including the current tail. The writer may
    // append later values to that tail, but cannot alter an already sealed slot.
    // Both the data budget and directory budget must outlive this snapshot.
    class Snapshot final {
    public:
        // Read pinned historical metadata without promoting all candidate
        // pages. One decoded page is retained until the next cold page access.
        class Reader final {
            const Snapshot& source_;
            ReadCache cache_;
        public:
            Reader(const Snapshot& source,MemoryBudget& memory):source_(source),cache_(memory){}
            Reader(const Reader&)=delete;
            Reader& operator=(const Reader&)=delete;
            const ExperienceEvidence& operator[](std::size_t index) {
                if(index>=source_.count_)throw std::out_of_range("experience snapshot traversal index");
                index+=source_.begin_;
                const auto& chunk=source_.chunks_[chunk_index(index)-source_.first_chunk_];
                return cache_.read(*chunk,chunk_offset(index));
            }
        };
        Snapshot(const Snapshot&)=delete;
        Snapshot& operator=(const Snapshot&)=delete;
        Snapshot(Snapshot&& other) noexcept:chunks_(std::move(other.chunks_)),count_(std::exchange(other.count_,0)),begin_(other.begin_),first_chunk_(other.first_chunk_){}
        Snapshot& operator=(Snapshot&&)=delete;
        [[nodiscard]] std::size_t size() const noexcept{return count_;}
        [[nodiscard]] std::size_t original_begin() const noexcept{return begin_;}
        [[nodiscard]] const ExperienceEvidence& operator[](std::size_t index) const {
            if(index>=count_)throw std::out_of_range("experience snapshot index");
            index+=begin_;
            return chunks_[chunk_index(index)-first_chunk_]->resident()[chunk_offset(index)];
        }
    private:
        friend class ExperienceSequence;
        Snapshot(const ExperienceSequence& source,MemoryBudget& directory_memory,std::size_t begin,std::size_t end)
            :chunks_(&directory_memory),count_(0),begin_(begin),first_chunk_(0){
            if(begin>end || end>source.size_)throw std::out_of_range("experience snapshot range");
            if(begin==end)return;
            first_chunk_=chunk_index(begin);
            chunks_.assign(source.chunks_.begin()+first_chunk_,source.chunks_.begin()+chunk_index(end-1)+1);
            count_=end-begin;
        }
        std::pmr::vector<std::shared_ptr<Chunk>> chunks_;
        std::size_t count_,begin_,first_chunk_;
    };
    class View {
    public:
        struct Iterator {
            using value_type=ExperienceEvidence;
            using difference_type=std::ptrdiff_t;
            using reference=const ExperienceEvidence&;
            using pointer=const ExperienceEvidence*;
            using iterator_category=std::forward_iterator_tag;
            const ExperienceSequence* owner=nullptr;
            std::size_t index=0;
            reference operator*() const{return (*owner)[index];}
            pointer operator->() const{return &(*owner)[index];}
            Iterator& operator++() noexcept{++index;return *this;}
            Iterator operator++(int) noexcept{auto old=*this;++*this;return old;}
            bool operator==(const Iterator&) const noexcept=default;
        };
        [[nodiscard]] std::size_t size() const noexcept{return count_;}
        [[nodiscard]] bool empty() const noexcept{return !count_;}
        [[nodiscard]] const ExperienceEvidence& operator[](std::size_t index) const{return (*owner_)[offset_+index];}
        [[nodiscard]] Iterator begin() const noexcept{return {owner_,offset_};}
        [[nodiscard]] Iterator end() const noexcept{return {owner_,offset_+count_};}
        [[nodiscard]] View subspan(std::size_t offset) const {
            if(offset>count_)throw std::out_of_range("experience view suffix");
            return View(owner_,offset_+offset,count_-offset);
        }
    private:
        friend class ExperienceSequence;
        View(const ExperienceSequence* owner,std::size_t offset,std::size_t count):owner_(owner),offset_(offset),count_(count){}
        const ExperienceSequence* owner_;
        std::size_t offset_,count_;
    };
    explicit ExperienceSequence(MemoryBudget& memory):memory_(memory),chunks_(&memory){}
    ExperienceSequence(const ExperienceSequence&)=delete;
    ExperienceSequence& operator=(const ExperienceSequence&)=delete;
    [[nodiscard]] std::size_t size() const noexcept{return size_;}
    // Aliasing ownership pins only the containing segment; no new control
    // block, address copy, or complete sequence directory is allocated.
    [[nodiscard]] std::shared_ptr<const ExperienceEvidence> pin(std::size_t index) const {
        if(index>=size_)throw std::out_of_range("experience pin index");
        const auto chunk=chunk_index(index);
        return std::shared_ptr<const ExperienceEvidence>(chunks_[chunk], &(*this)[index]);
    }
    [[nodiscard]] Snapshot snapshot(MemoryBudget& directory_memory) const{return Snapshot(*this,directory_memory,0,size_);}
    [[nodiscard]] Snapshot snapshot(MemoryBudget& memory,std::size_t begin,std::size_t end) const {return Snapshot(*this,memory,begin,end);}
    [[nodiscard]] View view() const noexcept{return View(this,0,size_);}
    [[nodiscard]] const ExperienceEvidence& operator[](std::size_t index) const{
        // 1,2,4,...,128 entries, then fixed 256-entry segments. Singleton
        // connections reserve one value, and shuffled access remains direct.
        return chunks_[chunk_index(index)]->resident()[chunk_offset(index)];
    }
    // Serialized owner operation, outside input and merge preparation. Borrowed
    // views must not be used concurrently. Explicit pins/snapshots prevent eviction.
    [[nodiscard]] bool page_candidate(std::size_t& index) const noexcept {
        if(index>=size_)return false;
        const auto& chunk=chunks_[chunk_index(index)];
        index+=chunk->used-chunk_offset(index);
        const auto action=architecture::kernel::metadata_release(chunk->used==chunk->capacity,
            chunk.use_count(),chunk->backing!=nullptr);
        // First backing must actually save RAM; tiny segments are retained.
        return action!=architecture::kernel::MetadataRelease::retain&&chunk->data.load()!=nullptr&&
            architecture::kernel::metadata_page_beneficial(chunk->backing!=nullptr,
                chunk->capacity*sizeof(ExperienceEvidence),sizeof(Chunk::Backing));
    }
    [[nodiscard]] bool page_out(std::size_t index,const std::filesystem::path& path,
        const architecture::DigestBytes& identity,const architecture::kernel::EvidenceRules& rules,
        StorageBudget* storage=nullptr) const {
        if(index>=size_)throw std::out_of_range("experience page-out index");
        const auto& chunk=chunks_[chunk_index(index)];
        using architecture::kernel::MetadataRelease;
        const auto action=architecture::kernel::metadata_release(chunk->used==chunk->capacity,
            chunk.use_count(),chunk->backing!=nullptr);
        if(action==MetadataRelease::retain)return false;
        if(!chunk->data.load(std::memory_order_acquire))return false;
        if(action==MetadataRelease::persist_then_release){
            using Backing=Chunk::Backing;
            auto* backing=static_cast<Backing*>(memory_.allocate(sizeof(Backing),alignof(Backing)));
            try {
                std::construct_at(backing,ExperiencePage::create(path,identity,
                    std::span<const ExperienceEvidence>(chunk->resident(),chunk->used),memory_,storage),rules);
            }catch(...){memory_.deallocate(backing,sizeof(Backing),alignof(Backing));throw;}
            chunk->backing=backing;
        }
        chunk->release();return true;
    }
    // Full segments are immutable. The incomplete tail is copied eagerly so
    // neither sequence can change a shared segment or relocate its own values.
    void share_prefix(const ExperienceSequence& source){
        if(size_||!chunks_.empty()||&memory_!=&source.memory_)
            throw std::logic_error("experience prefix requires empty owner with shared budget");
        decltype(chunks_) prepared(&memory_);prepared.reserve(source.chunks_.size());
        for(const auto& segment:source.chunks_){
            if(segment->used==segment->capacity)prepared.push_back(segment);
            else {
                auto copy=make_chunk(segment->capacity);
                for(std::size_t i=0;i<segment->used;++i){std::construct_at(copy->resident()+i,segment->resident()[i]);++copy->used;}
                prepared.push_back(std::move(copy));
            }
        }
        chunks_.swap(prepared);size_=source.size_;
    }
    void prepare_append(){
        if(!chunks_.empty()&&chunks_.back()->used<chunks_.back()->capacity)return;
        const auto capacity=chunks_.empty()?1:std::min<std::size_t>(256,chunks_.back()->capacity*2);
        // Allocate the segment before changing the directory. If directory
        // growth fails too, the local segment frees its reservation.
        auto next=make_chunk(capacity);chunks_.push_back(std::move(next));
    }
    void commit_append(const ExperienceEvidence& value) noexcept{
        static_assert(std::is_nothrow_copy_constructible_v<ExperienceEvidence>);
        auto& chunk=*chunks_.back();std::construct_at(chunk.data.load(std::memory_order_relaxed)+chunk.used,value);++chunk.used;++size_;
    }
private:
    MemoryBudget& memory_;
    std::shared_ptr<Chunk> make_chunk(std::size_t capacity){
        return std::allocate_shared<Chunk>(std::pmr::polymorphic_allocator<Chunk>(&memory_),memory_,capacity);
    }
    std::pmr::vector<std::shared_ptr<Chunk>> chunks_;
    std::size_t size_=0;
};
} // namespace swegca::vrs
