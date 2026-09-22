#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>

namespace swegca::vrs {

class OwnerLock {
public:
    // SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:14-25
    explicit OwnerLock(std::filesystem::path path);
    OwnerLock(const OwnerLock&) = delete;
    OwnerLock& operator=(const OwnerLock&) = delete;

    // SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:101-105
    ~OwnerLock();

    // A negative timeout waits until the OS lock is available.
    // SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:58-80
    void acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    // SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:82-92
    void release(bool force = false);

    // SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:23-25
    [[nodiscard]] bool locked() const;

private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    int descriptor_ = -1;
    std::size_t depth_ = 0;
};

}  // namespace swegca::vrs
