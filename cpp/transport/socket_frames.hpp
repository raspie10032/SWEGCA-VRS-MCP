#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace swegca::transport {
// One exclusive stream reader. Syntax/framing only; EOF never ends a VRS session.
// The shared resource accounts for the current line. A fixed staging buffer
// preserves already-received bytes even if growing the line allocation fails.
class SocketFrames final {
public:
    enum class State { pending, frame, end };
    SocketFrames(int socket,std::size_t limit,std::pmr::memory_resource& memory)
        :limit_(limit),line_(&memory){
        if(!limit)throw std::invalid_argument("frame limit must be positive");
        int type=0;socklen_t size=sizeof(type);
        if(::getsockopt(socket,SOL_SOCKET,SO_TYPE,&type,&size)<0)throw std::system_error(errno,std::generic_category(),"frame reader socket");
        if(type!=SOCK_STREAM)throw std::invalid_argument("frame reader requires stream socket");
        fd_=::fcntl(socket,F_DUPFD_CLOEXEC,0);
        if(fd_<0)throw std::system_error(errno,std::generic_category(),"duplicate frame reader socket");
    }
    ~SocketFrames(){if(fd_>=0)::close(fd_);}
    SocketFrames(const SocketFrames&)=delete;
    SocketFrames& operator=(const SocketFrames&)=delete;
    [[nodiscard]] State poll(){
        if(failed_)throw std::logic_error("frame stream failed");
        if(ready_)return State::frame;
        if(ended_)return State::end;
        if(begin_==end_){
            const auto count=::recv(fd_,staging_.data(),staging_.size(),MSG_DONTWAIT);
            if(count<0){
                const auto code=errno;
                if(code==EAGAIN||code==EWOULDBLOCK||code==EINTR)return State::pending;
                failed_=true;throw std::system_error(code,std::generic_category(),"receive app-server frame");
            }
            if(!count){
                if(!line_.empty()){failed_=true;throw std::runtime_error("truncated app-server frame");}
                ended_=true;return State::end;
            }
            begin_=0;end_=static_cast<std::size_t>(count);
        }
        const auto delimiter=std::find(staging_.begin()+begin_,staging_.begin()+end_,'\n');
        const auto count=static_cast<std::size_t>(delimiter-(staging_.begin()+begin_));
        if(count>limit_-line_.size()){
            failed_=true;throw std::length_error("app-server frame exceeds configured limit");
        }
        // Strong allocation failure guarantee: no cursor advances until append
        // succeeds. Retrying after memory becomes available loses no socket bytes.
        line_.append(staging_.data()+begin_,count);begin_+=count;
        if(begin_<end_){++begin_;ready_=true;return State::frame;}
        return State::pending;
    }
    [[nodiscard]] std::string_view frame() const{
        if(!ready_)throw std::logic_error("complete frame required");
        return line_;
    }
    void consumed(){
        if(!ready_)throw std::logic_error("no frame to consume");
        ready_=false;line_.clear();
    }
    [[nodiscard]] bool buffered() const noexcept{return ready_||begin_<end_;}
    [[nodiscard]] std::size_t retained_bytes() const noexcept{return line_.size()+end_-begin_;}
private:
    friend class AppServerPump;
    int fd_=-1;
    std::size_t limit_;
    std::pmr::string line_;
    std::array<char,4096> staging_{};
    std::size_t begin_=0,end_=0;
    bool ready_=false,ended_=false,failed_=false;
};
} // namespace swegca::transport
