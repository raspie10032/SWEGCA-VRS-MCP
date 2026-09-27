#pragma once
#include "transport/socket_frames.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <sys/stat.h>
#include <sys/un.h>

namespace swegca::transport {
// Local adapter transport only. One bounded request per connection. Polling is
// nonblocking and the owner calls it only between complete native messages.
class AgentQuerySocket final {
public:
    static constexpr const char* environment="SWEGCA_QUERY_SOCKET";
    AgentQuerySocket(const char* path,std::pmr::memory_resource& memory):memory_(memory){
        if(!path||!*path)return;
        const std::filesystem::path location(path);
        struct stat parent{};
        if(!location.is_absolute()||::lstat(location.parent_path().c_str(),&parent)||
            !S_ISDIR(parent.st_mode)||parent.st_uid!=::geteuid()||(parent.st_mode&077))
            throw std::invalid_argument("query directory must be private and owned");
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(location.string().size()>=sizeof(address.sun_path))throw std::invalid_argument("query socket path too long");
        std::copy_n(path,location.string().size()+1,address.sun_path);
        path_=location;
        fd_=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        if(fd_<0)throw std::system_error(errno,std::generic_category(),"query socket");
        if(::bind(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address))){
            const auto code=errno;::close(fd_);fd_=-1;
            throw std::system_error(code,std::generic_category(),"query bind");
        }
        if(::lstat(path,&owned_)||::chmod(path,0600)||::listen(fd_,4)){
            cleanup();throw std::runtime_error("query listener setup failed");
        }
    }
    ~AgentQuerySocket(){cleanup();}
    AgentQuerySocket(const AgentQuerySocket&)=delete;
    AgentQuerySocket& operator=(const AgentQuerySocket&)=delete;
    bool enabled() const noexcept{return fd_>=0;}
    template<class Respond> void poll(Respond respond){
        if(fd_<0)return;
        for(auto& slot:clients_)if(!slot){
            const int peer=::accept4(fd_,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if(peer<0){
                if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)break;
                throw std::system_error(errno,std::generic_category(),"query accept");
            }
            ucred credentials{};socklen_t size=sizeof(credentials);
            if(::getsockopt(peer,SOL_SOCKET,SO_PEERCRED,&credentials,&size)||credentials.uid!=::geteuid()){
                ::close(peer);continue;
            }
            try{slot.emplace(peer,memory_);}catch(...){::close(peer);throw;}
            break;
        }
        for(auto& slot:clients_)if(slot){
            auto& client=*slot;
            try{
                if(std::chrono::steady_clock::now()-client.started>std::chrono::seconds(30)){
                    slot.reset();continue;
                }
                if(!client.responded){
                    const auto state=client.reader.poll();
                    if(state==SocketFrames::State::end){slot.reset();continue;}
                    if(state==SocketFrames::State::frame){
                        client.response=respond(client.reader.frame());client.responded=true;
                    }
                }
                if(client.responded){
                    const auto count=::send(client.fd,client.response.data()+client.sent,
                        client.response.size()-client.sent,MSG_DONTWAIT|MSG_NOSIGNAL);
                    if(count>0)client.sent+=static_cast<std::size_t>(count);
                    else if(count<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){slot.reset();continue;}
                    if(client.sent==client.response.size())slot.reset();
                }
            }catch(const std::bad_alloc&){slot.reset();throw;}
            catch(const std::exception&){slot.reset();}
        }
    }
private:
    struct Client {
        Client(int socket,std::pmr::memory_resource& memory)
            :fd(socket),reader(socket,65536,memory),response(&memory){}
        ~Client(){::close(fd);}
        int fd;
        SocketFrames reader;
        std::pmr::string response;
        std::size_t sent=0;
        bool responded=false;
        std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    };
    void cleanup() noexcept{
        for(auto& slot:clients_)slot.reset();
        if(fd_>=0){::close(fd_);fd_=-1;}
        struct stat current{};
        if(!path_.empty()&&!::lstat(path_.c_str(),&current)&&S_ISSOCK(current.st_mode)&&
            current.st_dev==owned_.st_dev&&current.st_ino==owned_.st_ino)::unlink(path_.c_str());
    }
    std::pmr::memory_resource& memory_;
    int fd_=-1;
    std::filesystem::path path_;
    struct stat owned_{};
    std::array<std::optional<Client>,4> clients_;
};
} // namespace swegca::transport
