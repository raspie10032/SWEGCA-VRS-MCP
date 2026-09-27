#include "transport/json.hpp"
#include "transport/requirement_anchor.hpp"
#include "transport/stdio_frames.hpp"
#include "vrs/file_observation.hpp"
#include "vrs/memory_budget.hpp"

#include <charconv>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <unistd.h>

using namespace swegca::transport;
using namespace swegca::vrs;
namespace {
std::uint64_t integer(std::string_view text) {
    std::uint64_t value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if(text.empty()||parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())
        throw std::invalid_argument("expected unsigned decimal string");
    return value;
}
void address(const Json& value) {
    if(value.kind!=Json::Kind::object||value.keys.size()!=4)throw std::invalid_argument("invalid inputOriginal");
    for(const auto key:{"block","digest"}){
        const auto text=value.at(key).string();
        if(text.size()!=64)throw std::invalid_argument("invalid inputOriginal digest");
        for(const auto c:text)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))
            throw std::invalid_argument("invalid inputOriginal digest");
    }
    (void)integer(value.at("offset").string());
    if(!integer(value.at("bytes").string()))throw std::invalid_argument("empty inputOriginal");
}
struct File {
    int fd=-1;
    explicit File(const char* path):fd(::open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK)){}
    ~File(){if(fd>=0)::close(fd);}
    File(const File&)=delete;
};
void number(std::pmr::string& out,std::uint64_t value){out+='"';out+=std::to_string(value);out+='"';}
void metadata(std::pmr::string& out,const struct stat& value){
    if(value.st_mode==0){out+="null";return;}
    out+="{\"device\":";number(out,value.st_dev);out+=",\"inode\":";number(out,value.st_ino);
    out+=",\"size\":\"";out+=std::to_string(value.st_size);out+='"';
    out+=",\"mode\":";number(out,value.st_mode);
    // POSIX times can precede the epoch; preserve the signed seconds.
    out+=",\"mtimeSeconds\":\"";out+=std::to_string(value.st_mtim.tv_sec);out+="\",\"mtimeNanos\":";number(out,value.st_mtim.tv_nsec);
    out+=",\"ctimeSeconds\":\"";out+=std::to_string(value.st_ctim.tv_sec);out+="\",\"ctimeNanos\":";number(out,value.st_ctim.tv_nsec);out+='}';
}
void file_result(std::pmr::string& out,const Json& path,const FileReadObservation& value,bool complete){
    out+="{\"path\":";append_json(out,path);out+=",\"readBytes\":";number(out,value.bytes_read);
    out+=",\"digest\":";
    if(complete){
        out+='"';constexpr char hex[]="0123456789abcdef";
        for(const auto byte:value.digest){const auto n=std::to_integer<unsigned>(byte);out+=hex[n>>4];out+=hex[n&15];}
        out+='"';
    }else out+="null";
    out+=",\"before\":";metadata(out,value.before);out+=",\"after\":";metadata(out,value.after);out+='}';
}
std::pmr::string measure(const Json& args,std::uint64_t limit,MemoryBudget& memory,TransferBudget& transfer){
    if(args.kind!=Json::Kind::object||(args.keys.size()!=3&&args.keys.size()!=4))
        throw std::invalid_argument("expected inputOriginal, left, right and optional requirement");
    const auto& input=args.at("inputOriginal");address(input);
    const auto* requirement=args.find("requirement");
    if(args.keys.size()==4&&!requirement)throw std::invalid_argument("unknown observation argument");
    if(requirement)(void)requirement_anchor(*requirement);
    const auto& left_path=args.at("left");const auto& right_path=args.at("right");
    // Validate both paths before opening either one.
    for(const auto* path:{&left_path,&right_path}){
        const auto text=path->string();
        if(text.empty()||text.front()!='/'||text.find('\0')!=text.npos)
            throw std::invalid_argument("file paths must be absolute and contain no NUL");
    }
    File left(left_path.scalar.c_str());const int left_error=left.fd<0?errno:0;
    File right(right_path.scalar.c_str());const int right_error=right.fd<0?errno:0;
    FileEqualityObservation observed;
    if(left_error||right_error)observed.io_error=left_error?left_error:right_error;
    else observed=observe_file_equality(left.fd,right.fd,limit,memory,transfer);
    // The scope is derived from the actual operation and operands. Callers
    // cannot relabel a content comparison as whole-task completion.
    std::pmr::string scope("{\"predicate\":\"equal-file-bytes-v1\",\"left\":",&memory);
    append_json(scope,left_path);scope+=",\"right\":";append_json(scope,right_path);
    if(requirement){scope+=",\"requirement\":";append_requirement(scope,requirement_anchor(*requirement));}
    scope+='}';
    std::pmr::string out("{\"swegcaObservation\":{\"inputOriginal\":",&memory);append_json(out,input);
    if(requirement){out+=",\"requirement\":";append_requirement(out,requirement_anchor(*requirement));}
    out+=",\"scope\":";append_json_string(out,scope);out+=",\"axis\":\"0\",\"outcome\":";
    using swegca::architecture::kernel::EvidenceOutcome;
    append_json_string(out,observed.outcome==EvidenceOutcome::support?"support":
        observed.outcome==EvidenceOutcome::refute?"refute":"insufficient");
    out+=",\"confidence\":";out+=observed.outcome==EvidenceOutcome::insufficient?"0":"1";
    out+=",\"hasExpiry\":false,\"expiresAt\":\"0\"},\"measurement\":{\"complete\":";
    out+=observed.complete?"true":"false";out+=",\"stable\":";out+=observed.stable?"true":"false";
    out+=",\"ioError\":";out+=std::to_string(observed.io_error);
    out+=",\"left\":";file_result(out,left_path,observed.left,observed.complete);
    out+=",\"right\":";file_result(out,right_path,observed.right,observed.complete);
    out+="},\"grantsAuthority\":false}";return out;
}
constexpr std::string_view listing=R"({"tools":[{"name":"observe_file_content_equality","description":"Read two regular files and report only their byte-content equality as a scoped SWEGCA observation. Requires the current inputOriginal reference. Does not verify task completion, permissions, movement, or semantic relevance. Files are read only; use immutable snapshots for atomic comparison.","annotations":{"readOnlyHint":true,"destructiveHint":false},"inputSchema":{"type":"object","properties":{"inputOriginal":{"type":"object","properties":{"block":{"type":"string"},"offset":{"type":"string"},"bytes":{"type":"string"},"digest":{"type":"string"}},"required":["block","offset","bytes","digest"],"additionalProperties":false},"left":{"type":"string"},"right":{"type":"string"},"requirement":{"type":"object","description":"Optional proposed user requirement: exact UTF-8 quote and byte offset within the original params.input array item at textIndex. VRS checks against the sealed user input; this does not prove semantic relevance.","properties":{"textIndex":{"type":"string"},"byteOffset":{"type":"string"},"quote":{"type":"string","minLength":1}},"required":["textIndex","byteOffset","quote"],"additionalProperties":false}},"required":["inputOriginal","left","right"],"additionalProperties":false}}]})";
void result(std::string_view id,std::string_view body){std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":"<<body<<"}\n"<<std::flush;}
void error(std::string_view id,int code,std::string_view text){
    std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"error\":{\"code\":"<<code<<",\"message\":";
    write_json_string(std::cout,text);std::cout<<"}}\n"<<std::flush;
}
void tool_result(std::string_view id,std::string_view body){
    std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":{\"content\":[{\"type\":\"text\",\"text\":";
    write_json_string(std::cout,"File byte-content observation. The input reference, scope and measured evidence are in structuredContent; this is not a task-completion judgment.");
    std::cout<<"}],\"structuredContent\":"<<body<<",\"isError\":false}}\n"<<std::flush;
}
}
int main(int argc,char** argv){
    try{
        if(argc!=4&&(argc!=5||std::string_view(argv[4])!="--require-shared-io"))
            throw std::invalid_argument("usage: swegca-content-observer RAM_BYTES IO_BYTES_PER_SECOND MAX_FILE_BYTES [--require-shared-io]");
        if(argc==5){
            const auto* owner=std::getenv(SharedTransferState::environment);
            if(!owner||!*owner)throw std::runtime_error("shared I/O owner required for desktop observer");
        }
        const auto ram=integer(argv[1]);
        if(ram<(1U<<20)||ram>std::numeric_limits<std::size_t>::max())throw std::invalid_argument("invalid observer RAM budget");
        MemoryBudget memory(static_cast<std::size_t>(ram));TransferBudget transfer(integer(argv[2]));
        const auto limit=integer(argv[3]);StdioFrames frames(STDIN_FILENO,65536,memory);
        bool eof=false,initialized=false,ready=false;
        while(!eof){
            std::pmr::string line(&memory);
            try{line=frames.next(eof);}catch(const StdioFrames::ReadError&){throw;}
            catch(const std::bad_alloc&){throw;}catch(const std::exception&){error("null",-32700,"invalid frame");continue;}
            if(eof)break;
            Json request(&memory);
            try{request=parse_json(line,memory);}catch(const std::bad_alloc&){throw;}
            catch(const std::exception&){error("null",-32700,"invalid JSON");continue;}
            if(request.kind!=Json::Kind::object||!request.find("jsonrpc")||
                request.at("jsonrpc").kind!=Json::Kind::string||request.at("jsonrpc").scalar!="2.0"||
                !request.find("method")||request.at("method").kind!=Json::Kind::string){
                error("null",-32600,"invalid request");continue;
            }
            const auto* id=request.find("id");std::pmr::string encoded_id("null",&memory);
            if(id&&(id->kind==Json::Kind::string||id->kind==Json::Kind::number))encoded_id=encode_json(*id,memory);
            else if(id){error("null",-32600,"invalid request id");continue;}
            try{
                if(request.at("jsonrpc").string()!="2.0")throw std::invalid_argument("invalid JSON-RPC version");
                const auto method=request.at("method").string();
                if(!id){if(method=="notifications/initialized"&&initialized)ready=true;continue;}
                if(method=="initialize"){
                    if(initialized)throw std::invalid_argument("already initialized");
                    const auto& p=request.at("params");(void)p.at("protocolVersion").string();
                    if(p.at("capabilities").kind!=Json::Kind::object||p.at("clientInfo").kind!=Json::Kind::object)
                        throw std::invalid_argument("invalid initialize parameters");
                    initialized=true;result(encoded_id,R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}},"serverInfo":{"name":"swegca-content-observer","version":"0.1"}})");continue;
                }
                if(method=="ping"){result(encoded_id,"{}");continue;}
                if(!ready)throw std::invalid_argument("initialization not completed");
                if(method=="tools/list"){result(encoded_id,listing);continue;}
                if(method!="tools/call"){error(encoded_id,-32601,"unknown method");continue;}
                const auto& p=request.at("params");
                if(p.at("name").string()!="observe_file_content_equality")throw std::invalid_argument("unknown tool");
                tool_result(encoded_id,measure(p.at("arguments"),limit,memory,transfer));
            }catch(const std::bad_alloc&){throw;}catch(const std::exception& e){if(id)error(encoded_id,-32602,e.what());}
            if(!std::cout)return 2;
        }
        return std::cout?0:2;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
