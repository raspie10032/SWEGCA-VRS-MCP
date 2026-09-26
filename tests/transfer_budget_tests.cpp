#include "vrs/transfer_budget.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
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
 std::printf("transfer budget tests: %u checks passed\n",checks);
}
