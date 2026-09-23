#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

// SWEGCA consumes an allocator supplied by its host. Counting requested bytes,
// setting a memory budget, and deciding whether that budget is exhausted are
// SWEGCA-VRS runtime responsibilities. The resource stays alive while any
// container or snapshot still owns an AllocationAdapter copied from it.
// SWEGCA: user@2026-09-23:1
namespace swegca::architecture {

class AllocationResource {
public:
    // SWEGCA: user@2026-09-23:1
    AllocationResource() = default;
    AllocationResource(const AllocationResource&) = delete;
    AllocationResource& operator=(const AllocationResource&) = delete;
    virtual ~AllocationResource() = default;

    // SWEGCA: user@2026-09-23:1
    [[nodiscard]] virtual void* allocate(std::size_t bytes, std::size_t alignment) = 0;
    virtual void deallocate(void* pointer, std::size_t bytes,
                            std::size_t alignment) noexcept = 0;
};

template <class T>
class AllocationAdapter {
public:
    using value_type = T;

    // SWEGCA: user@2026-09-23:1
    AllocationAdapter() = delete;
    explicit AllocationAdapter(std::shared_ptr<AllocationResource> resource)
        : resource_(std::move(resource)) {
        if (!resource_) throw std::invalid_argument("allocation_resource_missing");
    }

    template <class U>
    AllocationAdapter(const AllocationAdapter<U>& other) noexcept
        : resource_(other.resource_) {}

    // SWEGCA: user@2026-09-23:1
    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length();
        return static_cast<T*>(resource_->allocate(count * sizeof(T), alignof(T)));
    }

    // SWEGCA: user@2026-09-23:1
    void deallocate(T* pointer, std::size_t count) noexcept {
        resource_->deallocate(pointer, count * sizeof(T), alignof(T));
    }

    template <class U>
    [[nodiscard]] bool operator==(const AllocationAdapter<U>& other) const noexcept {
        return resource_ == other.resource_;
    }

private:
    template <class>
    friend class AllocationAdapter;

    std::shared_ptr<AllocationResource> resource_;
};

class AllocationContext final {
public:
    // SWEGCA: user@2026-09-23:1
    explicit AllocationContext(std::shared_ptr<AllocationResource> resource)
        : resource_(std::move(resource)) {
        if (!resource_) throw std::invalid_argument("allocation_resource_missing");
    }

    // SWEGCA: user@2026-09-23:1
    template <class T>
    [[nodiscard]] AllocationAdapter<T> allocator() const {
        return AllocationAdapter<T>(resource_);
    }

private:
    std::shared_ptr<AllocationResource> resource_;
};

}  // namespace swegca::architecture
