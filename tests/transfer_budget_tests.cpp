#include "vrs/transfer_budget.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
using namespace swegca::vrs;
static unsigned checks=0;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
template<class E,class F>void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
int main(){
 constexpr auto chunk=TransferBudget::chunk_bytes;
 throws<std::invalid_argument>([]{TransferBudget bad(0);});
 TransferBudget budget(chunk); // one full chunk per second
 CHECK(budget.try_acquire(chunk,0)==0);
 CHECK(budget.requested()==chunk);
 CHECK(budget.try_acquire(chunk,0)==1000000000);
 CHECK(budget.requested()==chunk); // waiting consumes nothing
 CHECK(budget.try_acquire(chunk,999999999)==1000000000);
 CHECK(budget.try_acquire(chunk,1000000000)==1000000000);
 CHECK(budget.requested()==2*chunk);
 // A late wake gets only a single chunk of idle credit, never catch-up debt.
 CHECK(budget.try_acquire(chunk,100000000000)==100000000000);
 CHECK(budget.try_acquire(chunk,100000000000)==101000000000);
 throws<std::invalid_argument>([&]{(void)budget.try_acquire(chunk+1,0);});
 TransferBudget extreme(UINT64_MAX);
 CHECK(extreme.try_acquire(chunk,0)==0);
 CHECK(extreme.try_acquire(chunk,0)==1);
 throws<std::overflow_error>([&]{(void)extreme.try_acquire(chunk,INT64_MAX);});
 TransferBudget shared(chunk);
 std::atomic<unsigned> admitted{0};
 auto worker=[&]{for(unsigned i=0;i<100;++i)if(shared.try_acquire(chunk/4,0)==0)++admitted;};
 std::thread one(worker),two(worker);one.join();two.join();
 CHECK(admitted==4&&shared.requested()==chunk);
 TransferBudget live(100*chunk);
 const auto start=std::chrono::steady_clock::now();
 live.wait(chunk);live.wait(chunk);
 CHECK(std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(9));
 CHECK(live.requested()==2*chunk);
 // Independent processes must compete for the same single-chunk credit.
 CHECK(std::getenv(SharedTransferState::environment)==nullptr);
 {
  SharedTransferState owner(chunk);const auto locator=owner.locator();
  CHECK(::setenv(SharedTransferState::environment,locator.c_str(),1)==0);
  TransferBudget aggregate(chunk);
  pid_t children[4];
  for(auto& child:children){
   child=::fork();CHECK(child>=0);
   if(child==0){
    TransferBudget participant(chunk);unsigned count=0;
    for(unsigned n=0;n<20;++n)if(participant.try_acquire(chunk/4,0)==0)++count;
    ::_exit(count);
   }
  }
  unsigned total=0;
  for(auto child:children){int status=0;CHECK(::waitpid(child,&status,0)==child&&WIFEXITED(status));total+=WEXITSTATUS(status);}
  CHECK(total==4&&aggregate.requested()==chunk);
  CHECK(aggregate.try_acquire(chunk,0)==1000000000);
  throws<std::invalid_argument>([&]{TransferBudget wrong_rate(chunk+1);});
  const auto abandoned=::fork();CHECK(abandoned>=0);
  if(abandoned==0){::pthread_mutex_lock(&owner.state().mutex);::_exit(0);}
  int status=0;CHECK(::waitpid(abandoned,&status,0)==abandoned&&WIFEXITED(status));
  throws<std::runtime_error>([&]{(void)aggregate.requested();});
  throws<std::runtime_error>([&]{(void)aggregate.try_acquire(1,1000000000);});
  CHECK(::unsetenv(SharedTransferState::environment)==0);
 }
 CHECK(::setenv(SharedTransferState::environment,"invalid",1)==0);
 throws<std::invalid_argument>([&]{TransferBudget malformed(chunk);});
 CHECK(::unsetenv(SharedTransferState::environment)==0);
 std::printf("transfer budget tests: %u checks passed\n",checks);
}
