#include "owner_lock.hpp"

#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:27-46
bool try_os_lock(int descriptor) {
#if defined(_WIN32)
    if (_lseek(descriptor, 0, SEEK_SET) < 0)
        throw std::runtime_error("owner_lock_seek_failed");
    if (_locking(descriptor, _LK_NBLCK, 1) == 0) return true;
    if (errno == EACCES || errno == EAGAIN || errno == EDEADLK) return false;
#else
    if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) return true;
    if (errno == EACCES || errno == EAGAIN) return false;
#endif
    throw std::runtime_error("owner_lock_failed");
}

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:48-56
void unlock_os(int descriptor) {
#if defined(_WIN32)
    if (_lseek(descriptor, 0, SEEK_SET) < 0 || _locking(descriptor, _LK_UNLCK, 1) != 0)
        throw std::runtime_error("owner_unlock_failed");
#else
    if (flock(descriptor, LOCK_UN) != 0)
        throw std::runtime_error("owner_unlock_failed");
#endif
}

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:66-76
void close_os(int descriptor) {
#if defined(_WIN32)
    _close(descriptor);
#else
    close(descriptor);
#endif
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:14-25
OwnerLock::OwnerLock(std::filesystem::path path) : path_(std::move(path)) {}

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:101-105
OwnerLock::~OwnerLock() {
    try { release(true); } catch (...) {}
}

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:58-80
void OwnerLock::acquire(std::chrono::milliseconds timeout) {
    std::lock_guard guard(mutex_);
    if (descriptor_ >= 0) {
        ++depth_;
        return;
    }
    if (std::filesystem::create_directories(path_.parent_path()))
        std::filesystem::permissions(path_.parent_path(),
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
#if defined(_WIN32)
    const auto descriptor = _wopen(path_.c_str(), _O_BINARY | _O_RDWR | _O_CREAT,
                                   _S_IREAD | _S_IWRITE);
#else
    const auto descriptor = open(path_.c_str(), O_RDWR | O_CREAT, 0600);
#endif
    if (descriptor < 0) throw std::runtime_error("owner_lock_open_failed");
    try {
#if defined(_WIN32)
        if (_filelengthi64(descriptor) == 0) {
            const char byte = 0;
            if (_write(descriptor, &byte, 1) != 1 || _commit(descriptor) != 0)
                throw std::runtime_error("owner_lock_init_failed");
        }
#endif
        const auto deadline = timeout.count() < 0
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + timeout;
        while (!try_os_lock(descriptor)) {
            if (timeout.count() >= 0 && std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("owner_lock_timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (...) {
        close_os(descriptor);
        throw;
    }
    descriptor_ = descriptor;
    depth_ = 1;
}

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:82-92
void OwnerLock::release(bool force) {
    std::lock_guard guard(mutex_);
    if (descriptor_ < 0) return;
    if (!force && depth_ > 1) {
        --depth_;
        return;
    }
    const auto descriptor = std::exchange(descriptor_, -1);
    depth_ = 0;
    try {
        unlock_os(descriptor);
    } catch (...) {
        close_os(descriptor);
        throw;
    }
    close_os(descriptor);
}

// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:23-25
bool OwnerLock::locked() const {
    std::lock_guard guard(mutex_);
    return descriptor_ >= 0;
}

}  // namespace swegca::vrs
