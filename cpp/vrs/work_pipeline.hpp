#pragma once
#include "vrs/block_collectors.hpp"
#include <map>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace swegca::vrs {
// Scheduling only. Codec, SWEGCA verification and VRS application remain typed
// operations supplied by their owners; this class never invents a verdict.
// All payload ownership survives until the single applier finishes the batch.
template<class File, class Decoded, class Verified>
class WorkPipeline final {
public:
 struct Backend {
  std::string name;
  std::size_t initial_batch=1,maximum_batch=1;
  std::function<std::vector<Verified>(std::span<const std::shared_ptr<const Decoded>>)> verify;
 };
 struct Config {
  std::size_t codec_workers=1,file_queue=128,decoded_queue=128,result_queue=128,apply_batch=32;
  std::chrono::milliseconds partial_flush{100};
 };
 struct Stats {std::uint64_t submitted=0,decoded=0,verified=0,applied=0,apply_batches=0;};
 using Decode=std::function<std::shared_ptr<const Decoded>(File)>;
 using Apply=std::function<void(std::span<const Verified>)>;
 using Route=std::function<std::size_t(const Verified&)>;
 WorkPipeline(Config config,Decode decode,std::vector<Backend> backends,Apply apply,Route route={},std::size_t collector_workers=1)
  :config_(config),decode_(std::move(decode)),apply_(std::move(apply)),backends_(std::move(backends)),route_(std::move(route)) {
  if(!config_.codec_workers||!config_.file_queue||!config_.decoded_queue||!config_.result_queue||!config_.apply_batch||config_.partial_flush.count()<=0||!decode_||!apply_||backends_.empty())throw std::invalid_argument("pipeline configuration");
  for(const auto& b:backends_)if(!b.verify||!b.initial_batch||b.initial_batch>b.maximum_batch)throw std::invalid_argument("pipeline backend");
  codecs_left_=config_.codec_workers;verifiers_left_=backends_.size();
  try {
   // Queues and applier exist before any producer starts.
   if(route_)collectors_=std::make_unique<BlockCollectors>(1,collector_workers,config_.result_queue);
   applier_=std::thread([this]{guard([&]{if(route_)route_loop();else apply_loop();});});
   for(auto& backend:backends_)verifiers_.emplace_back([this,&backend]{guard([&]{verify_loop(backend);});});
   for(std::size_t i=0;i<config_.codec_workers;++i)codecs_.emplace_back([this]{guard([&]{codec_loop();});});
  }catch(...){fail(std::current_exception());join();throw;}
 }
 WorkPipeline(const WorkPipeline&)=delete;
 ~WorkPipeline(){close();join();if(collectors_)collectors_->drain();}
 void submit(File file){
  std::unique_lock l(mutex_);cv_.wait(l,[&]{return error_||closed_||files_.size()<config_.file_queue;});rethrow();
  if(closed_)throw std::logic_error("pipeline input closed");files_.push_back(std::move(file));++stats_.submitted;l.unlock();cv_.notify_all();
 }
 void close(){std::lock_guard l(mutex_);closed_=true;cv_.notify_all();}
 void finish(){close();join();if(collectors_)collectors_->finish();std::lock_guard l(mutex_);rethrow();}
 Stats stats()const{std::lock_guard l(mutex_);return stats_;}
private:
 struct Result {std::shared_ptr<const Decoded> owner;Verified value;};
 template<class F>void guard(F&& fn)noexcept{try{fn();}catch(...){fail(std::current_exception());}}
 void fail(std::exception_ptr e)noexcept{std::lock_guard l(mutex_);if(!error_)error_=e;closed_=true;cv_.notify_all();}
 void rethrow()const{if(error_)std::rethrow_exception(error_);}
 void join(){for(auto& t:codecs_)if(t.joinable())t.join();for(auto& t:verifiers_)if(t.joinable())t.join();if(applier_.joinable())applier_.join();}
 void codec_loop(){
  for(;;){File file;
   {std::unique_lock l(mutex_);cv_.wait(l,[&]{return error_||closed_||!files_.empty();});if(error_)return;if(files_.empty())break;file=std::move(files_.front());files_.pop_front();}cv_.notify_all();
   auto decoded=decode_(std::move(file));if(!decoded)throw std::runtime_error("codec returned no input");
   {std::unique_lock l(mutex_);cv_.wait(l,[&]{return error_||decoded_.size()<config_.decoded_queue;});if(error_)return;decoded_.push_back(std::move(decoded));++stats_.decoded;}cv_.notify_all();
  }
  {std::lock_guard l(mutex_);--codecs_left_;}cv_.notify_all();
 }
 void verify_loop(Backend& backend){
  std::size_t target=backend.initial_batch;
  for(;;){std::vector<std::shared_ptr<const Decoded>> batch;
   {std::unique_lock l(mutex_);cv_.wait(l,[&]{return error_||!codecs_left_||!decoded_.empty();});if(error_)return;if(decoded_.empty())break;
    const auto count=std::min(target,decoded_.size());batch.reserve(count);for(std::size_t i=0;i<count;++i){batch.push_back(std::move(decoded_.front()));decoded_.pop_front();}
   }cv_.notify_all();
   const auto begin=std::chrono::steady_clock::now();auto results=backend.verify(batch);
   const auto elapsed=std::chrono::steady_clock::now()-begin;
   if(results.size()!=batch.size())throw std::runtime_error("SWEGCA backend result count mismatch");
   for(std::size_t i=0;i<results.size();++i){
    std::unique_lock l(mutex_);cv_.wait(l,[&]{return error_||results_.size()<config_.result_queue;});if(error_)return;
    results_.push_back({std::move(batch[i]),std::move(results[i])});++stats_.verified;l.unlock();cv_.notify_all();
   }
   // Logical work count adapts to measured completion and the device memory
   // bound. It is never equated with OS thread count. No padding/repetition.
   if(elapsed<std::chrono::milliseconds(2)&&target<backend.maximum_batch)target+=std::min(target,backend.maximum_batch-target);
   else if(elapsed>std::chrono::milliseconds(20)&&target>1)target=std::max<std::size_t>(1,target/2);
  }
  {std::lock_guard l(mutex_);--verifiers_left_;}cv_.notify_all();
 }
 void route_loop(){
  struct Pending {std::deque<Result> rows;std::chrono::steady_clock::time_point first;};
  std::map<std::size_t,Pending> pending;std::size_t buffered=0;
  for(;;){
   bool done=false;
   {
    std::unique_lock lock(mutex_);
    auto deadline=std::chrono::steady_clock::time_point::max();
    for(const auto& [block,p]:pending){(void)block;deadline=std::min(deadline,p.first+config_.partial_flush);}
    cv_.wait_until(lock,deadline,[&]{return error_||!verifiers_left_||!results_.empty();});
    if(error_)return;
    while(!results_.empty()&&buffered<config_.result_queue){
     auto row=std::move(results_.front());results_.pop_front();auto& p=pending[route_(row.value)];
     if(p.rows.empty())p.first=std::chrono::steady_clock::now();p.rows.push_back(std::move(row));++buffered;
    }
    done=!verifiers_left_&&results_.empty();
   }
   cv_.notify_all();
   for(auto it=pending.begin();it!=pending.end();){auto& p=it->second;
    if(!done&&buffered<config_.result_queue&&p.rows.size()<config_.apply_batch&&std::chrono::steady_clock::now()<p.first+config_.partial_flush){++it;continue;}
    const auto block=it->first;auto [where,created]=routes_.try_emplace(block,0);if(created)where->second=collectors_->add_block();
    const auto count=std::min(config_.apply_batch,p.rows.size());std::vector<Result> owned;owned.reserve(count);
    for(std::size_t i=0;i<count;++i){owned.push_back(std::move(p.rows.front()));p.rows.pop_front();}buffered-=count;
    collectors_->submit(where->second,[this,owned=std::move(owned)]()mutable{
     std::vector<Verified> values;values.reserve(owned.size());for(auto& row:owned)values.push_back(std::move(row.value));
     try{apply_(values);}catch(...){fail(std::current_exception());throw;}
     {std::lock_guard lock(mutex_);stats_.applied+=values.size();++stats_.apply_batches;}
    });
    if(p.rows.empty())it=pending.erase(it);else ++it;
   }
   if(done&&pending.empty())return;
  }
 }
 void apply_loop(){
  for(;;){std::vector<Result> owned;std::vector<Verified> batch;
   {std::unique_lock l(mutex_);cv_.wait(l,[&]{return error_||!verifiers_left_||!results_.empty();});if(error_)return;if(results_.empty())break;
    // Finite flush avoids a partially filled queue deadlocking backpressure.
    cv_.wait_for(l,config_.partial_flush,[&]{return error_||!verifiers_left_||results_.size()>=std::min(config_.apply_batch,config_.result_queue);});if(error_)return;
    const auto count=std::min(config_.apply_batch,results_.size());owned.reserve(count);batch.reserve(count);
    for(std::size_t i=0;i<count;++i){owned.push_back(std::move(results_.front()));results_.pop_front();}
   }cv_.notify_all();
   for(auto& item:owned)batch.push_back(std::move(item.value));
   apply_(batch); // Only this thread has mutation authority. No dispatch lock.
   {std::lock_guard l(mutex_);stats_.applied+=batch.size();++stats_.apply_batches;}cv_.notify_all();
  }
 }
 Config config_;Decode decode_;Apply apply_;std::vector<Backend> backends_;
 Route route_;std::unique_ptr<BlockCollectors> collectors_;std::map<std::size_t,std::size_t> routes_;
 mutable std::mutex mutex_;std::condition_variable cv_;bool closed_=false;std::exception_ptr error_;
 std::size_t codecs_left_=0,verifiers_left_=0;Stats stats_;
 std::deque<File> files_;std::deque<std::shared_ptr<const Decoded>> decoded_;std::deque<Result> results_;
 std::vector<std::thread> codecs_,verifiers_;std::thread applier_;
};
}
