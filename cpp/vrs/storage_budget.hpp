#pragma once
#include "vrs/transfer_budget.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace swegca::vrs {
class StorageLimit final : public std::runtime_error {
public:
    StorageLimit():std::runtime_error("VRS storage budget exhausted"){}
};

// VRS resource accounting, not an evidence verdict. One owner shares this
// across writers. Existing physical bytes must be accounted before reopening
// writers; hard-linked originals must be counted only once by that owner.
class StorageBudget final {
public:
    explicit StorageBudget(std::uint64_t limit, std::uint64_t existing_bytes=0, std::uint64_t bytes_per_second=625000000)
        :limit_(limit),used_(existing_bytes),transfer_(bytes_per_second) {
        if(existing_bytes>limit)throw StorageLimit();
    }
    StorageBudget(const StorageBudget&)=delete;
    StorageBudget& operator=(const StorageBudget&)=delete;
    [[nodiscard]] std::uint64_t used() const noexcept{return used_.load(std::memory_order_relaxed);}
    [[nodiscard]] std::uint64_t limit() const noexcept{return limit_;}

    [[nodiscard]] TransferBudget& transfer() noexcept{return transfer_;}
    [[nodiscard]] const TransferBudget& transfer() const noexcept{return transfer_;}

    class Reservation final {
    public:
        Reservation(StorageBudget* owner,std::uint64_t bytes):owner_(owner),bytes_(bytes){
            if(!owner_)return;
            auto current=owner_->used_.load(std::memory_order_relaxed);
            do {
                if(bytes>owner_->limit_-current)throw StorageLimit();
            } while(!owner_->used_.compare_exchange_weak(current,current+bytes,std::memory_order_relaxed));
        }
        Reservation(const Reservation&)=delete;
        Reservation& operator=(const Reservation&)=delete;
        ~Reservation(){if(owner_&&!retained_)owner_->used_.fetch_sub(bytes_,std::memory_order_relaxed);}
        // Retain before the first write. Failed/partial writes conservatively
        // keep the entire attempted extent charged until cold reconciliation.
        void retain() noexcept{retained_=true;}
    private:
        StorageBudget* owner_;
        std::uint64_t bytes_;
        bool retained_=false;
    };
private:
    friend class ConnectionCatalog;
    // Only the catalog's confirmed removed inode path may return disk bytes.
    void reclaim_removed(std::uint64_t bytes) noexcept {
        auto current=used_.load(std::memory_order_relaxed);
        do {
            if(bytes>current)return; // Never underflow on inconsistent ownership.
        } while(!used_.compare_exchange_weak(current,current-bytes,std::memory_order_relaxed));
    }
    const std::uint64_t limit_;
    std::atomic<std::uint64_t> used_;
    TransferBudget transfer_;
};
} // namespace swegca::vrs
