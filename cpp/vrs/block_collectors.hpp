#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
namespace swegca::vrs {
// Many logical VRS blocks, a variable number of available collector workers.
// A block is exclusively leased to one collector; other blocks may apply in
// parallel. This schedules ownership only and never produces a core verdict.
class BlockCollectors final {
public:
 BlockCollectors(std::size_t blocks,std::size_t workers,std::size_t capacity=1024)
  :blocks_(blocks),capacity_(capacity){
  if(!blocks||!workers||!capacity)throw std::invalid_argument("block collector dimensions");
  try{for(std::size_t i=0;i<workers;++i)threads_.emplace_back([this]{run();});}
  catch(...){{std::lock_guard l(mutex_);stop_=true;}cv_.notify_all();for(auto& t:threads_)t.join();throw;}
 }
 ~BlockCollectors(){drain();{std::lock_guard l(mutex_);stop_=true;}cv_.notify_all();for(auto& t:threads_)t.join();}
 BlockCollectors(const BlockCollectors&)=delete;
 std::size_t blocks()const {std::lock_guard l(mutex_);return blocks_.size();}
 std::size_t add_block(){std::lock_guard l(mutex_);if(error_)std::rethrow_exception(error_);if(stop_)throw std::logic_error("collectors closed");blocks_.emplace_back();return blocks_.size()-1;}
 void submit(std::size_t block,std::function<void()> apply){
  std::unique_lock l(mutex_);
  if(block>=blocks_.size()||!apply)throw std::invalid_argument("invalid block application");
 cv_.wait(l,[&]{return error_||stop_||pending_<capacity_;});
  if(error_)std::rethrow_exception(error_);if(stop_)throw std::logic_error("collectors closed");
  auto& b=blocks_[block];b.queue.push_back(std::move(apply));++pending_;
  if(!b.leased){b.leased=true;ready_.push_back(block);}l.unlock();cv_.notify_all();
 }
 // Owner waits only when it needs the completed graph revision/checkpoint.
 // Completion of an individual task never waits for other blocks.
 void finish(){drain();std::lock_guard l(mutex_);if(error_)std::rethrow_exception(error_);}
 void drain()noexcept{std::unique_lock l(mutex_);cv_.wait(l,[&]{return pending_==0;});}
private:
 struct Block {bool leased=false;std::deque<std::function<void()>> queue;};
 void run()noexcept{
  for(;;){std::function<void()> apply;std::size_t index;
   {std::unique_lock l(mutex_);cv_.wait(l,[&]{return stop_||!ready_.empty();});if(ready_.empty())return;
    index=ready_.front();ready_.pop_front();auto& block=blocks_[index];apply=std::move(block.queue.front());block.queue.pop_front();}
   try{apply();}catch(...){std::lock_guard l(mutex_);if(!error_)error_=std::current_exception();}
   {std::lock_guard l(mutex_);auto& block=blocks_[index];--pending_;
    if(block.queue.empty())block.leased=false;else ready_.push_back(index);
   }cv_.notify_all();
  }
 }
 std::vector<Block> blocks_;std::size_t capacity_,pending_=0;bool stop_=false;
 mutable std::mutex mutex_;std::condition_variable cv_;std::exception_ptr error_;
 std::deque<std::size_t> ready_;std::vector<std::thread> threads_;
};
}
