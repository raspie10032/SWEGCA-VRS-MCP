#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

// VRS consumes an allocator supplied by its host. Counting requested bytes,
// setting a memory budget, and deciding whether that budget is exhausted are
// VRS runtime responsibilities. The resource stays alive while any
// container or snapshot still owns an AllocationAdapter copied from it. Weak
// source analogy: the approved flow requires hard host budgets; the abstract
// allocator is native C++ infrastructure for the user's later clarification
// that VRS owns the count and limit.
// SWEGCA: user@2026-09-22:89-92
namespace swegca::vrs {

// The VRS host throws this when its configured allocation budget refuses a
// request. A physical allocation failure remains std::bad_alloc, so a page
// cache can evict only for budget refusal.
// SWEGCA: user@2026-09-22:89-92
class AllocationRefused final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "allocation_budget_refused";
    }
};

class AllocationResource {
public:
    // SWEGCA: user@2026-09-22:89-92
    AllocationResource() = default;
    AllocationResource(const AllocationResource&) = delete;
    AllocationResource& operator=(const AllocationResource&) = delete;
    virtual ~AllocationResource() = default;

    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] virtual void* allocate(std::size_t bytes, std::size_t alignment) = 0;
    virtual void deallocate(void* pointer, std::size_t bytes,
                            std::size_t alignment) noexcept = 0;
};

template <class T>
class AllocationAdapter {
public:
    using value_type = T;

    // Standard containers may inspect whether their value types can be
    // default-constructed, even when every real value is created with Main's
    // allocator. Such a probe must compile, while an actual resource-less
    // construction fails at compile time before an allocator object can exist.
    // SWEGCA: user@2026-09-22:89-92
    template <class U = T>
    AllocationAdapter() {
        static_assert(sizeof(U) == 0, "allocation_resource_missing: construct with the host resource");
    }
    // An allocator remains equal to its source after a move. Declaring the
    // same-type copy operations suppresses an implicit move that would take
    // the shared resource away from the source allocator.
    // SWEGCA: user@2026-09-22:89-92
    AllocationAdapter(const AllocationAdapter&) noexcept = default;
    AllocationAdapter& operator=(const AllocationAdapter&) noexcept = default;
    // SWEGCA: user@2026-09-22:89-92
    explicit AllocationAdapter(std::shared_ptr<AllocationResource> resource)
        : resource_(std::move(resource)) {
        if (!resource_) throw std::invalid_argument("allocation_resource_missing");
    }

    // SWEGCA: user@2026-09-22:89-92
    template <class U>
    AllocationAdapter(const AllocationAdapter<U>& other) noexcept
        : resource_(other.resource_) {}

    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length();
        return static_cast<T*>(resource_->allocate(count * sizeof(T), alignof(T)));
    }

    // SWEGCA: user@2026-09-22:89-92
    void deallocate(T* pointer, std::size_t count) noexcept {
        resource_->deallocate(pointer, count * sizeof(T), alignof(T));
    }

    // SWEGCA: user@2026-09-22:89-92
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
    // SWEGCA: user@2026-09-22:89-92
    explicit AllocationContext(std::shared_ptr<AllocationResource> resource)
        : resource_(std::move(resource)) {
        if (!resource_) throw std::invalid_argument("allocation_resource_missing");
    }
    // A moved context remains usable for later host allocations; moving it
    // copies its shared resource, as moving AllocationAdapter does.
    // SWEGCA: user@2026-09-22:89-92
    AllocationContext(const AllocationContext&) noexcept = default;
    AllocationContext& operator=(const AllocationContext&) noexcept = default;

    // SWEGCA: user@2026-09-22:89-92
    template <class T>
    [[nodiscard]] AllocationAdapter<T> allocator() const {
        return AllocationAdapter<T>(resource_);
    }

private:
    std::shared_ptr<AllocationResource> resource_;
};

}  // namespace swegca::vrs
