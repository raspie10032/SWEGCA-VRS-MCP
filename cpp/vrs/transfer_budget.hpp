#pragma once
#include "vrs/shared_transfer_state.hpp"
#include <optional>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace swegca::vrs {
// One aggregate logical read+write allowance. This schedules physical I/O
// requests only; it is not an evidence, routing or session-lifecycle decision.
class TransferBudget final {
public:
    static constexpr std::uint64_t chunk_bytes=1U<<20;
    explicit TransferBudget(std::uint64_t bytes_per_second):rate_(bytes_per_second){
        if(!rate_)throw std::invalid_argument("VRS transfer rate must be positive");
        if(const auto* owner=std::getenv(SharedTransferState::environment);owner&&*owner)
            shared_.emplace(owner,rate_);
    }
    [[nodiscard]] std::uint64_t rate() const noexcept{return rate_;}
    [[nodiscard]] std::uint64_t requested() const {
        if(shared_){SharedTransferState::Lock lock(shared_->state());return shared_->state().requested;}
        std::lock_guard lock(mutex_);return requested_;
    }
    // The monotonic time is explicit so scheduling arithmetic can be verified
    // without wall-clock sleeps. Commits only when admitted now; otherwise
    // returns a retry time in ns without consuming any allowance.
    [[nodiscard]] std::int64_t try_acquire(std::uint64_t bytes,std::int64_t now) {
        if(bytes>chunk_bytes)throw std::invalid_argument("VRS transfer exceeds I/O chunk");
        if(shared_){
            auto& state=shared_->state();SharedTransferState::Lock lock(state);
            return schedule(bytes,now,state.next,state.requested);
        }
        std::lock_guard lock(mutex_);return schedule(bytes,now,next_,requested_);
    }
private:
    [[nodiscard]] std::int64_t schedule(std::uint64_t bytes,std::int64_t now,
        std::int64_t& next_,std::uint64_t& requested_) const {
        const auto scaled=bytes*1000000000ULL;
        const auto service=scaled/rate_+(scaled%rate_!=0);
        const auto base=std::max(now,next_);
        if(base>std::numeric_limits<std::int64_t>::max()-static_cast<std::int64_t>(service)||
            bytes>UINT64_MAX-requested_)throw std::overflow_error("VRS transfer schedule exhausted");
        const auto next=base+static_cast<std::int64_t>(service);
        // At most one chunk of initial/idle credit; idle time cannot bank an
        // unlimited burst. Integer rounding is conservative, including >1GB/s.
        constexpr auto scaled_credit=chunk_bytes*1000000000ULL;
        const auto credit=static_cast<std::int64_t>(scaled_credit/rate_+(scaled_credit%rate_!=0));
        const auto due=std::max(now,next-credit);
        if(due==now){next_=next;requested_+=bytes;}
        return due;
    }
public:
    void wait(std::uint64_t bytes) {
        using Clock=std::chrono::steady_clock;
        for(;;) {
            const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            const auto due=try_acquire(bytes,now);
            if(due==now)return;
            std::this_thread::sleep_until(Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(due))));
            // Recheck real time after waking: late/concurrent sleepers cannot
            // spend a backlog of obsolete reservations in one catch-up burst.
        }
    }
private:
    const std::uint64_t rate_;
    std::optional<SharedTransferState> shared_;
    mutable std::mutex mutex_;
    std::int64_t next_=0;
    std::uint64_t requested_=0;
};
} // namespace swegca::vrs
