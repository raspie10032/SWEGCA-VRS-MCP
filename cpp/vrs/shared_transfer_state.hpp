#pragma once
#include <charconv>
#include <cerrno>
#include <new>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swegca::vrs {
// Host-owned, RAM-only schedule. This shares resource accounting, never an
// experience verdict. A proc-fd locator survives child descriptor filtering.
class SharedTransferState final {
public:
    static constexpr const char* environment="SWEGCA_IO_OWNER";
    struct State {
        std::uint64_t magic=0,rate=0;
        pthread_mutex_t mutex{};
        std::int64_t next=0;
        std::uint64_t requested=0;
    };
    explicit SharedTransferState(std::uint64_t rate) {
        if(!rate)throw std::invalid_argument("zero shared transfer rate");
        fd_=::memfd_create("swegca-transfer",MFD_CLOEXEC|MFD_ALLOW_SEALING);
        if(fd_<0)throw std::runtime_error("shared transfer creation failed");
        try {
            if(::ftruncate(fd_,sizeof(State)))throw std::runtime_error("shared transfer sizing failed");
            map();::new(static_cast<void*>(state_)) State;
            pthread_mutexattr_t attributes;
            if(::pthread_mutexattr_init(&attributes))throw std::runtime_error("shared transfer mutex attributes failed");
            const auto shared=::pthread_mutexattr_setpshared(&attributes,PTHREAD_PROCESS_SHARED);
            const auto robust=::pthread_mutexattr_setrobust(&attributes,PTHREAD_MUTEX_ROBUST);
            const auto initialized=(shared||robust)? -1 : ::pthread_mutex_init(&state_->mutex,&attributes);
            ::pthread_mutexattr_destroy(&attributes);
            if(initialized)throw std::runtime_error("shared transfer mutex initialization failed");
            state_->rate=rate;state_->magic=magic;
            if(::fcntl(fd_,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL)<0)
                throw std::runtime_error("shared transfer size sealing failed");
        }catch(...){release();throw;}
    }
    SharedTransferState(std::string_view locator,std::uint64_t rate) {
        const auto separator=locator.find(':');
        if(separator==locator.npos)throw std::invalid_argument("invalid shared transfer owner");
        const auto pid=number(locator.substr(0,separator));const auto descriptor=number(locator.substr(separator+1));
        if(!pid)throw std::invalid_argument("invalid shared transfer pid");
        const auto path="/proc/"+std::to_string(pid)+"/fd/"+std::to_string(descriptor);
        fd_=::open(path.c_str(),O_RDWR|O_CLOEXEC);
        if(fd_<0)throw std::runtime_error("shared transfer owner unavailable");
        try {
            struct stat status{};const int seals=::fcntl(fd_,F_GET_SEALS);
            if(::fstat(fd_,&status)||status.st_size!=static_cast<off_t>(sizeof(State))||seals<0||
                (seals&(F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL))!=(F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL))
                throw std::runtime_error("invalid shared transfer object");
            map();
            if(state_->magic!=magic||state_->rate!=rate)
                throw std::invalid_argument("shared transfer rate or format mismatch");
        }catch(...){release();throw;}
    }
    SharedTransferState(const SharedTransferState&)=delete;
    SharedTransferState& operator=(const SharedTransferState&)=delete;
    ~SharedTransferState(){release();}
    [[nodiscard]] std::string locator() const{return std::to_string(::getpid())+":"+std::to_string(fd_);}
    [[nodiscard]] State& state() const noexcept{return *state_;}
    class Lock final {
    public:
        explicit Lock(State& state):state_(state){
            const auto result=::pthread_mutex_lock(&state_.mutex);
            if(result==EOWNERDEAD){
                // Incomplete accounting cannot be repaired by resetting credit.
                // Leave the mutex unrecoverable so every participant fails closed.
                ::pthread_mutex_unlock(&state_.mutex);
                throw std::runtime_error("shared transfer owner died during accounting");
            }
            if(result)throw std::runtime_error("shared transfer accounting unavailable");
        }
        Lock(const Lock&)=delete;
        ~Lock(){::pthread_mutex_unlock(&state_.mutex);}
    private:State& state_;
    };
private:
    static constexpr std::uint64_t magic=0x53574743494f3031ULL;
    static unsigned number(std::string_view text){
        unsigned result=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
        if(text.empty()||parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())
            throw std::invalid_argument("invalid shared transfer locator");
        return result;
    }
    void map(){
        auto* mapped=::mmap(nullptr,sizeof(State),PROT_READ|PROT_WRITE,MAP_SHARED,fd_,0);
        if(mapped==MAP_FAILED)throw std::runtime_error("shared transfer mapping failed");
        state_=static_cast<State*>(mapped);
    }
    void release() noexcept{
        if(state_)::munmap(state_,sizeof(State));
        if(fd_>=0)::close(fd_);
        state_=nullptr;fd_=-1;
    }
    int fd_=-1;State* state_=nullptr;
};
} // namespace swegca::vrs
