#include "vrs/main_sources.hpp"
#include <algorithm>

namespace swegca::vrs {
using namespace architecture;
namespace {
std::string name(const DigestBytes& id) {
    constexpr char digits[]="0123456789abcdef";
    std::string result; result.reserve(72);
    for (auto byte : id) { const auto n=std::to_integer<unsigned>(byte);result+=digits[n>>4];result+=digits[n&15]; }
    return result+".session";
}
DigestBytes parse(std::string_view filename) {
    if (filename.size()!=72 || !filename.ends_with(".session")) throw std::runtime_error("invalid Main source filename");
    DigestBytes id{};
    const auto digit=[](char c)->unsigned {
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        throw std::runtime_error("noncanonical Main source identity");
    };
    for(std::size_t i=0;i<id.size();++i)id[i]=std::byte((digit(filename[2*i])<<4)|digit(filename[2*i+1]));
    if(!kernel::named_digest(id))throw std::runtime_error("empty Main source identity");
    return id;
}
void require_file(const std::filesystem::path& path) {
    const auto status=std::filesystem::symlink_status(path);
    if(!std::filesystem::is_regular_file(status))throw std::runtime_error("Main source publication is not a regular file");
}
}
MainSources::MainSources(const std::filesystem::path& root,MemoryBudget& memory,std::uint64_t limit)
    :root_(root),memory_(memory),read_limit_(limit),sources_(&memory) {}
MainSources::Source::Source(const std::filesystem::path& root,const DigestBytes& id,MemoryBudget& budget,std::uint64_t limit)
    :memory(budget),read_limit(limit) {
    void* storage=memory.allocate(sizeof(SessionStore),alignof(SessionStore));
    try { store=new(storage) SessionStore(SessionStore::open(root,id,memory)); }
    catch(...) { memory.deallocate(storage,sizeof(SessionStore),alignof(SessionStore));throw; }
    if(!kernel::main_session_readable(store->phase(),store->usable())) {
        store->~SessionStore();memory.deallocate(store,sizeof(SessionStore),alignof(SessionStore));store=nullptr;
        throw std::runtime_error("Main source is not ended and published");
    }
}
MainSources::Source::~Source() {
    release_cache();
    if(store){store->~SessionStore();memory.deallocate(store,sizeof(SessionStore),alignof(SessionStore));}
}
const SessionRuntime& MainSources::Source::runtime() {
    if(!cache){
        void* storage=memory.allocate(sizeof(SessionRuntime),alignof(SessionRuntime));
        try { cache=new(storage) SessionRuntime(*store,memory,read_limit); }
        catch(...) { memory.deallocate(storage,sizeof(SessionRuntime),alignof(SessionRuntime));throw; }
    }
    return *cache;
}
void MainSources::Source::release_cache() noexcept {
    if(cache){cache->~SessionRuntime();memory.deallocate(cache,sizeof(SessionRuntime),alignof(SessionRuntime));cache=nullptr;}
}
std::pmr::vector<DigestBytes> MainSources::published() const {
    std::pmr::vector<DigestBytes> ids(&memory_);
    const auto directory=root_/"main";
    if(!std::filesystem::exists(directory))return ids;
    if(std::filesystem::is_symlink(directory)||!std::filesystem::is_directory(directory))
        throw std::runtime_error("invalid Main source directory");
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        const auto filename=entry.path().filename().string();
        if(!filename.ends_with(".session"))continue;
        require_file(entry.path());ids.push_back(parse(filename));
    }
    std::sort(ids.begin(),ids.end());return ids;
}
const SessionRuntime& MainSources::resolve(const DigestBytes& id) {
    if(!kernel::named_digest(id))throw std::invalid_argument("empty Main source identity");
    require_file(root_/"main"/name(id));
    auto [where,inserted]=sources_.try_emplace(id,root_,id,memory_,read_limit_);
    (void)inserted;
    // During replay, at most one decoded source cache is retained. The graph
    // keeps stable store pointers, not pointers to these runtime caches.
    for(auto& [other,source]:sources_)if(other!=id)source.release_cache();
    return where->second.runtime();
}
void MainSources::release_caches() noexcept { for(auto& [id,source]:sources_){(void)id;source.release_cache();} }
std::size_t MainSources::merge_published(PersistentMainGraph& graph,std::uint64_t seed,std::uint64_t step) {
    std::size_t merged=0;
    try {
        for(const auto& id:published()) {
            if(graph.graph().has_source(id))continue;
            if(graph.merge(resolve(id),seed,step))++merged;
            release_caches();
        }
    } catch(...) { release_caches();throw; }
    return merged;
}
} // namespace swegca::vrs
