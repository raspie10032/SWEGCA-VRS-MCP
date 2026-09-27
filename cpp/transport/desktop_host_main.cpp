#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
namespace {
volatile std::sig_atomic_t interrupted=0;
void stop(int){interrupted=1;}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd){}
    Fd(const Fd&)=delete;
    Fd(Fd&& other) noexcept:value(other.value){other.value=-1;}
    ~Fd(){if(value>=0)::close(value);}
    void close(){if(value>=0){::close(value);value=-1;}}
};
std::array<Fd,2> pair(bool pipe=false){
    int raw[2];require((pipe?::pipe2(raw,O_CLOEXEC) : ::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,raw))==0,"endpoint creation failed");
    // Child targets 0..5 must not alias a source descriptor during spawn actions.
    const int first=::fcntl(raw[0],F_DUPFD_CLOEXEC,10);
    const int second=::fcntl(raw[1],F_DUPFD_CLOEXEC,10);
    ::close(raw[0]);::close(raw[1]);
    Fd a(first),b(second);require(first>=0&&second>=0,"endpoint duplication failed");
    return {std::move(a),std::move(b)};
}
struct Child {
    pid_t pid=-1;int status=0;
    bool running(){
        if(pid<0)return false;
        const auto result=::waitpid(pid,&status,WNOHANG);
        if(result==0||(result<0&&errno==EINTR))return true;
        require(result==pid,"child wait failed");pid=-1;return false;
    }
    bool failed(){return !running()&&(!WIFEXITED(status)||WEXITSTATUS(status)!=0);}
    ~Child(){
        if(pid<0)return;
        ::kill(pid,SIGTERM);
        for(unsigned n=0;n<100;++n){
            const auto result=::waitpid(pid,&status,WNOHANG);
            if(result==pid||(result<0&&errno==ECHILD))return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(pid,SIGKILL);while(::waitpid(pid,&status,0)<0&&errno==EINTR){}
    }
    void start(char* const* args,std::initializer_list<std::pair<int,int>> mappings){
        posix_spawn_file_actions_t actions;
        require(::posix_spawn_file_actions_init(&actions)==0,"spawn actions initialization failed");
        try{
            for(const auto [source,target]:mappings)
                require(::posix_spawn_file_actions_adddup2(&actions,source,target)==0,"spawn mapping failed");
            require(::posix_spawn(&pid,args[0],&actions,nullptr,args,environ)==0,"child launch failed");
        }catch(...){::posix_spawn_file_actions_destroy(&actions);throw;}
        ::posix_spawn_file_actions_destroy(&actions);
    }
};
void nonblocking(int fd){const auto flags=::fcntl(fd,F_GETFL);require(flags>=0&&::fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0,"nonblocking setup failed");}
struct Buffer {
    std::array<char,65536> data{};std::size_t begin=0,end=0;
    bool empty() const{return begin==end;}
    void read(int fd,bool& eof){
        if(!empty())return;
        const auto size=::read(fd,data.data(),data.size());
        if(size>0){begin=0;end=static_cast<std::size_t>(size);}
        else if(size==0)eof=true;
        else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)throw std::runtime_error("desktop stream read failed");
    }
    void write(int fd){
        if(empty())return;
        const auto size=::write(fd,data.data()+begin,end-begin);
        if(size>0)begin+=static_cast<std::size_t>(size);
        else if(size<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)throw std::runtime_error("desktop stream write failed");
    }
};
}
int main(int argc,char** argv){
    try{
        require(argc>=8,"usage: swegca-desktop-host PROXY VRS MODE ROOT VRS_CONFIG PROXY_CONFIG BACKEND [ARGS...]");
        ::signal(SIGPIPE,SIG_IGN);
        ::signal(SIGTERM,stop);::signal(SIGINT,stop);
        auto client=pair(),server=pair(),vrs=pair(),ready=pair(true);
        Child backend,store,proxy;
        backend.start(argv+7,{{server[0].value,0},{server[0].value,1}});
        char* store_args[]{argv[2],argv[3],argv[4],argv[5],nullptr};
        store.start(store_args,{{vrs[0].value,0},{vrs[0].value,1}});
        char a[]="3",b[]="4",c[]="5";
        char* proxy_args[]{argv[1],a,b,c,argv[6],nullptr};
        proxy.start(proxy_args,{{client[1].value,3},{server[1].value,4},{vrs[1].value,5},{ready[1].value,1}});
        client[1].close();server[0].close();server[1].close();vrs[0].close();vrs[1].close();ready[1].close();
        std::string handshake;
        while(handshake!="ready\n"){
            require(!interrupted,"desktop host interrupted");
            require(!proxy.failed()&&!store.failed()&&!backend.failed(),"child failed during initialization");
            pollfd fd{ready[0].value,POLLIN,0};const auto result=::poll(&fd,1,100);
            if(result<0&&errno==EINTR)continue;
            require(result>=0,"initialization poll failed");if(!result)continue;
            char ch;require(::read(fd.fd,&ch,1)==1,"proxy closed before ready");
            handshake+=ch;require(handshake.size()<=6,"invalid proxy readiness");
        }
        ready[0].close();nonblocking(0);nonblocking(1);nonblocking(client[0].value);
        Buffer incoming,outgoing;bool input_end=false,output_end=false,write_end=false;
        while(!output_end||!outgoing.empty()||!input_end||!incoming.empty()){
            require(!interrupted,"desktop host interrupted");
            require(!proxy.failed()&&!store.failed()&&!backend.failed(),"child failed during relay");
            if(!backend.running())input_end=true;
            if(input_end&&incoming.empty()&&!write_end){
                require(::shutdown(client[0].value,SHUT_WR)==0,"desktop half-close failed");write_end=true;
            }
            std::array<pollfd,3> fds{{{input_end?-1:0,short(incoming.empty()?POLLIN:0),0},
                {client[0].value,short((!output_end&&outgoing.empty()?POLLIN:0)|(!incoming.empty()?POLLOUT:0)),0},
                {outgoing.empty()?-1:1,POLLOUT,0}}};
            const auto result=::poll(fds.data(),fds.size(),100);
            if(result<0&&errno==EINTR)continue;
            require(result>=0,"desktop relay poll failed");
            for(const auto& fd:fds)require(!(fd.revents&POLLNVAL),"desktop descriptor invalid");
            if(fds[0].revents&(POLLIN|POLLHUP))incoming.read(0,input_end);
            if(fds[1].revents&POLLOUT)incoming.write(client[0].value);
            if(fds[1].revents&(POLLIN|POLLHUP|POLLERR))outgoing.read(client[0].value,output_end);
            if(fds[2].revents&(POLLOUT|POLLERR|POLLHUP))outgoing.write(1);
        }
        require(incoming.empty(),"backend closed with undelivered client input");client[0].close();
        // Only these children are owned. EOF is never an explicit VRS end.
        for(unsigned n=0;n<100;++n){
            require(!interrupted,"desktop host interrupted");
            require(!proxy.failed()&&!store.failed()&&!backend.failed(),"child failed while closing");
            if(!proxy.running()&&!store.running()&&!backend.running())return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("children did not close after EOF");
    }catch(const std::exception& e){std::fprintf(stderr,"desktop host stopped: %s\n",e.what());return 1;}
}
