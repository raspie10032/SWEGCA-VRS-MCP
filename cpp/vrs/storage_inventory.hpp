#pragma once
#include "vrs/memory_budget.hpp"
#include "vrs/storage_budget.hpp"
#include <filesystem>
#include <unordered_set>
#include <sys/stat.h>
#include <cerrno>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace swegca::vrs {
// Serialize cooperating Runtime owners before their inventory snapshot.
// Lower-level standalone stores must not mutate a Runtime-owned root.
class StorageRoot final {
public:
    explicit StorageRoot(const std::filesystem::path& root) {
        fd_=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd_<0)throw std::system_error(errno,std::generic_category(),"open VRS storage root");
        if(::flock(fd_,LOCK_EX|LOCK_NB)<0){
            const auto error=errno;::close(fd_);fd_=-1;
            throw std::system_error(error,std::generic_category(),"lock VRS storage root");
        }
    }
    ~StorageRoot(){if(fd_>=0)::close(fd_);}
    StorageRoot(const StorageRoot&)=delete;
    StorageRoot& operator=(const StorageRoot&)=delete;
private:
    int fd_=-1;
};
// Cold startup only, before any writer is admitted. Caller exclusively owns
// the root. Count logical extents once per inode, including incomplete files.
inline std::uint64_t stored_bytes(const std::filesystem::path& root,MemoryBudget& memory) {
    if(std::filesystem::is_symlink(root)||!std::filesystem::is_directory(root))
        throw std::invalid_argument("VRS storage root must be a directory");
    // Preserve full device/inode equality even for single-link files: bind
    // mounts can expose those more than once. Hash collisions never imply
    // identity, and every node/bucket remains charged to the caller budget.
    struct InodeHash {
        std::size_t operator()(const std::pair<dev_t,ino_t>& key) const noexcept {
            const auto device=std::hash<dev_t>{}(key.first);
            const auto inode=std::hash<ino_t>{}(key.second);
            return device^(inode+std::size_t(0x9e3779b9U)+(device<<6)+(device>>2));
        }
    };
    std::pmr::unordered_set<std::pair<dev_t,ino_t>,InodeHash> seen(&memory);
    std::uint64_t total=0;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root)) {
        struct stat info{};
        if(::lstat(entry.path().c_str(),&info)<0)
            throw std::system_error(errno,std::generic_category(),"inventory VRS storage");
        if(S_ISDIR(info.st_mode))continue;
        if(!S_ISREG(info.st_mode)||info.st_size<0)
            throw std::runtime_error("VRS storage contains a non-regular file");
        if(!seen.emplace(info.st_dev,info.st_ino).second)continue;
        const auto bytes=static_cast<std::uint64_t>(info.st_size);
        if(bytes>UINT64_MAX-total)throw StorageLimit();
        total+=bytes;
    }
    return total;
}
} // namespace swegca::vrs
