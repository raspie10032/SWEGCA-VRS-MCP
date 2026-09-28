#include "vrs/codec_stream.hpp"
#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <atomic>
#include <semaphore>
#include <mutex>
#include <stdexcept>
extern char** environ;
namespace swegca::vrs {
namespace {
struct FD {int value;explicit FD(int n):value(n){if(n<0)throw std::runtime_error(std::strerror(errno));}~FD(){if(value>=0)::close(value);}FD(const FD&)=delete;};
struct CodecFailure:std::runtime_error{using std::runtime_error::runtime_error;};
void command(int source,std::vector<std::string> args,const std::function<void(std::span<const std::byte>)>& consume,MemoryBudget& memory,std::size_t chunk){
 static std::counting_semaphore<10> codec_slots(10);
 struct Lease {std::counting_semaphore<10>& slots;explicit Lease(std::counting_semaphore<10>& s):slots(s){slots.acquire();}~Lease(){slots.release();}} lease(codec_slots);
 int pipe[2];if(pipe2(pipe,O_CLOEXEC))throw std::runtime_error("codec pipe");FD input(pipe[0]),output(pipe[1]);
 posix_spawn_file_actions_t actions;if(posix_spawn_file_actions_init(&actions))throw std::runtime_error("codec spawn actions");
 auto add=[&](int rc){if(rc){posix_spawn_file_actions_destroy(&actions);throw std::runtime_error("codec spawn action");}};
 add(posix_spawn_file_actions_adddup2(&actions,source,198));
 add(posix_spawn_file_actions_adddup2(&actions,output.value,STDOUT_FILENO));
 add(posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));
 std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);pid_t pid;
 const int rc=posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ);posix_spawn_file_actions_destroy(&actions);
 if(rc)throw CodecFailure("cannot launch "+args[0]+": "+std::strerror(rc));
 ::close(output.value);output.value=-1;int status=0;
 try{
  std::pmr::vector<std::byte> buffer(&memory);buffer.resize(chunk);
  for(;;){auto n=::read(input.value,buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;if(n<0)throw std::runtime_error("codec pipe read");if(!n)break;consume({buffer.data(),std::size_t(n)});}
 }catch(...){kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}throw;}
 pid_t waited;do{waited=waitpid(pid,&status,0);}while(waited<0&&errno==EINTR);
 if(waited<0||!WIFEXITED(status)||WEXITSTATUS(status))throw CodecFailure("codec failed: "+args[0]);
}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;}
}
CodecStreamReceipt stream_codec_file(const std::filesystem::path& path,MemoryBudget& memory,const CodecConsumer& consume,std::size_t chunk){
 if(!consume||!chunk||chunk>(16ULL<<20))throw std::invalid_argument("codec consumer/chunk");
 FD source(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));struct stat before{},after{};
 if(fstat(source.value,&before)||!S_ISREG(before.st_mode))throw std::runtime_error("not a regular source");
 CodecStreamReceipt result;result.source=std::filesystem::absolute(path);
 constexpr auto input="/proc/self/fd/198";
 const auto small=[&](std::vector<std::string> args,std::size_t limit){std::string out;command(source.value,std::move(args),[&](auto bytes){if(bytes.size()>limit-out.size())throw std::runtime_error("codec metadata limit");out.append(reinterpret_cast<const char*>(bytes.data()),bytes.size());},memory,65536);return out;};
 result.media=small({"file","-L","--brief","--mime-type",input},4096);
 while(!result.media.empty()&&(result.media.back()=='\n'||result.media.back()=='\r'))result.media.pop_back();
 // pread does not alter the descriptor used by concurrently running codecs.
 std::exception_ptr original_error;std::atomic<bool> cancelled=false;
 std::jthread original([&]{try{
  std::pmr::vector<std::byte> buffer(&memory);buffer.resize(chunk);architecture::Sha256 hash;std::uint64_t offset=0;
  while(!cancelled){auto n=::pread(source.value,buffer.data(),buffer.size(),offset);if(n<0&&errno==EINTR)continue;if(n<0)throw std::runtime_error("source read");if(!n)break;std::span<const std::byte> bytes(buffer.data(),std::size_t(n));hash.update(bytes);consume({result.media,0,offset,bytes,true});offset+=n;}
  if(!cancelled){result.identity=hash.finish();result.original_bytes=offset;}
 }catch(...){original_error=std::current_exception();cancelled=true;}});
 struct Job {std::string media;std::vector<std::string> args;};std::vector<Job> jobs;
 try{
  if(result.media.starts_with("image/"))jobs.push_back({"image/x-miff",{"magick","-limit","thread","1","-limit","disk","0",input,"-compress","None","miff:-"}});
  else if(result.media.starts_with("audio/")||result.media.starts_with("video/")){
   auto metadata=small({"ffprobe","-v","error","-show_streams","-of","json",input},8ULL<<20);
   auto info=transport::parse_json(metadata,memory);
   for(const auto& stream:info.at("streams").values){
    auto type=stream.find("codec_type");if(!type)continue;const std::string index(stream.at("index").scalar);
    if(type->string()=="subtitle")jobs.push_back({"text/x-ass;stream="+index,{"ffmpeg","-nostdin","-v","error","-threads","1","-i",input,"-map","0:"+index,"-c:s","ass","-f","ass","pipe:1"}});
    else if(type->string()=="video"||type->string()=="audio")jobs.push_back({"application/x-nut;stream="+index,{"ffmpeg","-nostdin","-v","error","-threads","1","-i",input,"-map","0:"+index,"-c:v","rawvideo","-c:a","pcm_f64le","-threads","1","-f","nut","pipe:1"}});
   }
  }else if(result.media=="application/pdf")jobs.push_back({"text/plain",{"pdftotext","-layout",input,"-"}});
 }catch(const CodecFailure& e){result.codec_errors.push_back(e.what());}
 catch(...){cancelled=true;original.join();throw;}
 // Files are scheduled by the caller. Within a container, independent tracks
 // are decoded concurrently. The shared process semaphore bounds all files
 // together, not ten new active codec processes for every file.
 result.views.resize(jobs.size());std::vector<std::string> failures(jobs.size());
 std::atomic<std::size_t> next{0};std::mutex failure_mutex;std::exception_ptr consumer_error;
 auto work=[&]{for(;;){auto i=next.fetch_add(1);if(i>=jobs.size()||cancelled)return;
  auto& view=result.views[i];view.media=jobs[i].media;architecture::Sha256 hash;
  try{command(source.value,std::move(jobs[i].args),[&](auto bytes){if(cancelled)throw std::runtime_error("codec stream cancelled");consume({view.media,i+1,view.bytes,bytes,false});hash.update(bytes);view.bytes+=bytes.size();},memory,chunk);view.complete=true;}
  catch(const CodecFailure& e){failures[i]=e.what();}
  catch(...){std::lock_guard lock(failure_mutex);if(!consumer_error)consumer_error=std::current_exception();cancelled=true;}
  view.digest=hash.finish();
 }};
 {std::vector<std::jthread> workers;try{for(std::size_t i=0;i<std::min<std::size_t>(10,jobs.size());++i)workers.emplace_back(work);}catch(...){cancelled=true;throw;}}
 for(auto& error:failures)if(!error.empty())result.codec_errors.push_back(std::move(error));
 original.join();if(original_error)std::rethrow_exception(original_error);if(consumer_error)std::rethrow_exception(consumer_error);
 if(fstat(source.value,&after)||!same(before,after)||result.original_bytes!=std::uint64_t(before.st_size))throw std::runtime_error("source changed during codec stream; discard provisional observations");
 return result;
}
}
