#include <cstdio>
#include <ctime>
#include <unistd.h>
// Linked only into the diagnostic executable. Timestamp precedes marker I/O;
// no user content, source path or model call is involved.
extern "C" void swegca_recall_entry_probe() noexcept {
    timespec time{};if(::clock_gettime(CLOCK_MONOTONIC,&time))return;
    char text[96];const auto n=std::snprintf(text,sizeof(text),"SWEGCA_RECALL_NS %llu\n",
        static_cast<unsigned long long>(time.tv_sec)*1000000000ULL+time.tv_nsec);
    if(n>0)(void)::write(STDERR_FILENO,text,static_cast<std::size_t>(n));
}
