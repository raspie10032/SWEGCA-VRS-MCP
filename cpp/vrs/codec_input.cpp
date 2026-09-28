#include "vrs/codec_input.hpp"
#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <array>
#include <stdexcept>
#include <cstring>
extern char** environ;
namespace swegca::vrs {
namespace {
struct FD {int value=-1;explicit FD(int fd):value(fd){if(fd<0)throw std::runtime_error(std::strerror(errno));}~FD(){if(value>=0)::close(value);}FD(const FD&)=delete;};
void append_fd(int fd,std::pmr::vector<std::byte>& output){std::array<std::byte,65536> bytes;for(;;){auto n=::read(fd,bytes.data(),bytes.size());if(n<0&&errno==EINTR)continue;if(n<0)throw std::runtime_error("codec read failed");if(!n)return;output.insert(output.end(),bytes.begin(),bytes.begin()+n);}}
void command(int source,std::vector<std::string> args,std::pmr::vector<std::byte>& output){
 int ends[2];if(pipe2(ends,O_CLOEXEC))throw std::runtime_error("codec pipe failed");FD read_end(ends[0]),write_end(ends[1]);
 posix_spawn_file_actions_t actions;if(posix_spawn_file_actions_init(&actions))throw std::runtime_error("spawn actions");
 auto add=[&](int result){if(result){posix_spawn_file_actions_destroy(&actions);throw std::runtime_error("codec spawn action");}};
 add(posix_spawn_file_actions_adddup2(&actions,source,198));
 add(posix_spawn_file_actions_adddup2(&actions,write_end.value,STDOUT_FILENO));
 add(posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));
 std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);pid_t pid;
 auto code=posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ);posix_spawn_file_actions_destroy(&actions);
 if(code)throw std::runtime_error("cannot start codec: "+args[0]);
 ::close(write_end.value);write_end.value=-1;
 int status=0;try{append_fd(read_end.value,output);}catch(...){kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}throw;}
 pid_t waited;do{waited=waitpid(pid,&status,0);}while(waited<0&&errno==EINTR);
 if(waited<0||!WIFEXITED(status)||WEXITSTATUS(status))throw std::runtime_error("codec failed: "+args[0]);
}
}
std::shared_ptr<const CodecInput> decode_file(const std::filesystem::path& path,MemoryBudget& memory){
 auto result=std::make_shared<CodecInput>(memory);result->source=path;FD source(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));struct stat before{},after{};
 if(fstat(source.value,&before)||!S_ISREG(before.st_mode))throw std::runtime_error("input is not a regular file");
 append_fd(source.value,result->original);
 if(fstat(source.value,&after)||before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec)throw std::runtime_error("source changed while reading");
 result->identity=architecture::Sha256::of(result->original);
 // RAM-only immutable descriptor gives seeking codecs a stable input. It is
 // not a durable experience and is closed immediately after codec completion.
 FD snapshot(memfd_create("swegca-codec-input",MFD_CLOEXEC|MFD_ALLOW_SEALING));size_t at=0;
 while(at<result->original.size()){auto n=write(snapshot.value,result->original.data()+at,result->original.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("codec RAM buffer write");at+=n;}
 if(fcntl(snapshot.value,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))throw std::runtime_error("codec RAM seal");
 constexpr auto input="/proc/self/fd/198";std::pmr::vector<std::byte> mime(&memory);
 command(snapshot.value,{"file","-L","--brief","--mime-type",input},mime);
 result->source_media.assign(reinterpret_cast<const char*>(mime.data()),mime.size());while(!result->source_media.empty()&&(result->source_media.back()=='\n'||result->source_media.back()=='\r'))result->source_media.pop_back();
 auto decode=[&](std::string media,std::vector<std::string> args){CodecView view(memory);view.media=std::move(media);try{command(snapshot.value,std::move(args),view.bytes);}catch(const std::runtime_error& e){result->codec_errors.push_back(e.what());return;}result->decoded.push_back(std::move(view));};
 if(result->source_media.starts_with("image/"))decode("image/x-miff",{"magick","-limit","thread","1","-limit","disk","0",input,"-compress","None","miff:-"});
 else if(result->source_media.starts_with("audio/")||result->source_media.starts_with("video/")){decode("video/x-nut;video=raw;audio=pcm_f64le",{"ffmpeg","-nostdin","-v","error","-threads","1","-i",input,"-map","0:v?","-map","0:a?","-c:v","rawvideo","-c:a","pcm_f64le","-threads","1","-f","nut","pipe:1"});
  std::pmr::vector<std::byte> info(&memory);
  try {
   command(snapshot.value,{"ffprobe","-v","error","-show_streams","-of","json",input},info);
   auto streams=transport::parse_json({reinterpret_cast<const char*>(info.data()),info.size()},memory);
   for(const auto& stream:streams.at("streams").values){auto kind=stream.find("codec_type");if(!kind||kind->string()!="subtitle")continue;
    const std::string index(stream.at("index").scalar);
    decode("text/x-ass;stream="+index,{"ffmpeg","-nostdin","-v","error","-threads","1","-i",input,"-map","0:"+index,"-c:s","ass","-f","ass","pipe:1"});
   }
  }catch(const std::runtime_error& e){result->codec_errors.push_back(e.what());}
 }
 else if(result->source_media=="application/pdf")decode("text/plain",{"pdftotext","-layout",input,"-"});
 // Text and other originals are already present without conversion or tags.
 return result;
}
}
