#include "transport/json.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
using namespace swegca::transport;
namespace {
bool value_option(std::string_view arg){return arg=="-c"||arg=="--config"||arg=="--enable"||arg=="--disable";}
bool assigned_option(std::string_view arg){return arg.starts_with("--config=")||arg.starts_with("--enable=")||arg.starts_with("--disable=")||(arg.starts_with("-c")&&arg.size()>2);}
bool help(std::string_view arg){return arg=="--help"||arg=="-h"||arg=="--version"||arg=="-V";}
// Recognize the installed desktop's CLI grammar. Ambiguous server startup is
// rejected rather than silently launching an unobserved transport.
bool is_server(int argc,char** argv){
    int command=1;
    for(;command<argc;++command){
        const std::string_view arg(argv[command]);
        if(value_option(arg)){if(++command==argc)throw std::invalid_argument("missing global option value");continue;}
        if(assigned_option(arg))continue;
        if(help(arg))return false;
        if(arg.starts_with('-'))throw std::invalid_argument("unsupported leading backend option");
        break;
    }
    if(command==argc||std::string_view(argv[command])!="app-server")return false;
    bool tooling=false;
    for(int index=command+1;index<argc;++index){
        const std::string_view arg(argv[index]);
        if(help(arg))return false;
        if(value_option(arg)||arg=="--code-mode-host"){
            if(++index==argc)throw std::invalid_argument("missing app-server option value");
            continue;
        }
        if(assigned_option(arg)||arg.starts_with("--code-mode-host="))continue;
        if(arg=="--listen"){
            if(++index==argc||std::string_view(argv[index])!="stdio://")throw std::invalid_argument("VRS desktop wrapper requires stdio transport");
            continue;
        }
        if(arg.starts_with("--listen=")){if(arg!="--listen=stdio://")throw std::invalid_argument("VRS desktop wrapper requires stdio transport");continue;}
        if(arg=="--stdio"||arg=="--strict-config"||arg=="--analytics-default-enabled")continue;
        if(arg=="generate-json-schema"||arg=="generate-ts"||arg=="help"){tooling=true;break;}
        if(arg=="daemon"&&index+2==argc&&std::string_view(argv[index+1])=="version")return false;
        throw std::invalid_argument("unsupported app-server invocation");
    }
    return !tooling;
}
std::string path(const Json& config,std::string_view key){
    const auto value=config.at(key).string();
    if(value.empty()||value.front()!='/'||value.find('\0')!=std::string_view::npos)throw std::invalid_argument("absolute configuration path required");
    return std::string(value);
}
void not_self(const std::string& executable){
    struct stat self{},target{};
    if(::stat("/proc/self/exe",&self)||::stat(executable.c_str(),&target))throw std::runtime_error("configured executable unavailable");
    if(self.st_dev==target.st_dev&&self.st_ino==target.st_ino)throw std::invalid_argument("recursive wrapper configuration");
}
}
int main(int argc,char** argv){
    try{
        const auto* file=std::getenv("SWEGCA_DESKTOP_CONFIG");
        if(!file||!*file)throw std::invalid_argument("SWEGCA_DESKTOP_CONFIG required");
        swegca::vrs::MemoryBudget memory(1<<20);
        std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("desktop configuration unavailable");
        std::pmr::string text(&memory);char ch;
        while(input.get(ch)){if(text.size()==65536)throw std::length_error("desktop configuration too large");text+=ch;}
        const auto config=parse_json(text,memory);
        auto backend=path(config,"backend");not_self(backend);
        std::vector<std::string> owned;
        if(is_server(argc,argv)){
            auto host=path(config,"host");not_self(host);
            const auto mode=config.at("mode").string();
            if(mode!="create"&&mode!="open"&&mode!="limited-create"&&mode!="limited-open")throw std::invalid_argument("invalid VRS mode");
            owned={host,path(config,"proxy"),path(config,"vrs"),std::string(mode),path(config,"root"),
                   path(config,"resourceConfig"),path(config,"proxyConfig"),backend};
        }else owned={backend};
        for(int index=1;index<argc;++index)owned.emplace_back(argv[index]);
        std::vector<char*> arguments;for(auto& value:owned)arguments.push_back(value.data());arguments.push_back(nullptr);
        ::execv(arguments[0],arguments.data());throw std::runtime_error("configured executable failed to start");
    }catch(const std::exception& e){std::fprintf(stderr,"VRS backend wrapper stopped: %s\n",e.what());return 1;}
}
