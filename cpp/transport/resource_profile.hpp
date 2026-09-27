#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>
#endif

namespace swegca::transport {
#ifdef __linux__
inline std::uint64_t profile_memory_limit(std::uint64_t requested){
    const auto page=::sysconf(_SC_PAGESIZE);
    if(page<=0)throw std::runtime_error("memory page size unavailable");
    const auto effective=requested-requested%static_cast<std::uint64_t>(page);
    if(!effective)throw std::invalid_argument("memory profile is smaller than one page");
    return effective;
}
inline cpu_set_t profile_cpus(std::string_view text){
    cpu_set_t result;CPU_ZERO(&result);
    std::istringstream input{std::string(text)};std::string token;bool any=false;
    while(input>>token){
        unsigned value=0;
        for(char c:token){if(c<'0'||c>'9'||value>=CPU_SETSIZE)throw std::invalid_argument("invalid cpuAffinity");value=value*10+unsigned(c-'0');}
        if(value>=CPU_SETSIZE)throw std::invalid_argument("cpuAffinity exceeds platform mask");
        CPU_SET(value,&result);any=true;
    }
    if(!any)throw std::invalid_argument("cpuAffinity is empty");
    return result;
}
inline void verify_resource_profile(std::uint64_t ram,std::string_view cpus){
    std::ifstream membership("/proc/self/cgroup");std::string line,path;
    while(std::getline(membership,line))if(line.starts_with("0::/"))path=line.substr(3);
    if(path.empty())throw std::runtime_error("cgroup v2 membership unavailable");
    const auto group=std::filesystem::path("/sys/fs/cgroup")/path.substr(1);
    const auto value=[&](const char* name){std::ifstream file(group/name);std::string text;if(!(file>>text))throw std::runtime_error("cgroup resource control unavailable");return text;};
    if(value("memory.max")!=std::to_string(profile_memory_limit(ram))||value("memory.swap.max")!="0")
        throw std::runtime_error("VRS process memory limits do not match requested profile");
    const auto requested=profile_cpus(cpus);cpu_set_t actual;
    if(::sched_getaffinity(0,sizeof(actual),&actual)<0)throw std::system_error(errno,std::generic_category(),"read VRS affinity");
    if(!CPU_EQUAL(&requested,&actual))throw std::runtime_error("VRS CPU affinity differs from profile");
}
[[noreturn]] inline void launch_profiled_command(const std::vector<std::string>& command,
    std::uint64_t ram,std::string_view cpus){
    (void)profile_cpus(cpus);
    if(command.empty()||!ram)throw std::invalid_argument("empty resource profile command or memory limit");
    std::vector<std::string> args{"systemd-run","--user","--pipe","--wait","--collect","--quiet",
        "--working-directory="+std::filesystem::current_path().string(),
        "--property=MemoryMax="+std::to_string(profile_memory_limit(ram)),"--property=MemorySwapMax=0",
        "--property=CPUAffinity="+std::string(cpus),"--property=OOMPolicy=kill",
        "--"};
    args.insert(args.end(),command.begin(),command.end());
    std::vector<char*> pointers;for(auto& arg:args)pointers.push_back(arg.data());pointers.push_back(nullptr);
    ::execvp("systemd-run",pointers.data());
    throw std::system_error(errno,std::generic_category(),"launch VRS resource profile");
}
[[noreturn]] inline void launch_resource_profile(std::string_view mode,const char* root,const char* config,
    std::uint64_t ram,std::string_view cpus){
    launch_profiled_command({std::filesystem::read_symlink("/proc/self/exe").string(),
        "bounded-"+std::string(mode),std::filesystem::absolute(root).string(),std::filesystem::absolute(config).string()},ram,cpus);
}
#else
[[noreturn]] inline void launch_profiled_command(const std::vector<std::string>&,std::uint64_t,std::string_view){throw std::runtime_error("limited profile requires Linux cgroup v2");}
inline void verify_resource_profile(std::uint64_t,std::string_view){throw std::runtime_error("limited profile requires Linux cgroup v2");}
[[noreturn]] inline void launch_resource_profile(std::string_view,const char*,const char*,std::uint64_t,std::string_view){throw std::runtime_error("limited profile requires Linux cgroup v2");}
#endif
} // namespace swegca::transport
