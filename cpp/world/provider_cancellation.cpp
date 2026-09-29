#include "world/provider_cancellation.hpp"

#include <utility>

namespace swegca::world {
namespace {

thread_local ProviderCancellation* bound_cancellation{};

}  // namespace

ProviderCancelled::ProviderCancelled() : std::runtime_error("provider_cancelled") {}

ProviderCancellation::InterruptRegistration::InterruptRegistration(
    ProviderCancellation* owner, const std::size_t key) noexcept
    : owner_(owner), key_(key) {}

ProviderCancellation::InterruptRegistration::InterruptRegistration(
    InterruptRegistration&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), key_(other.key_) {}

ProviderCancellation::InterruptRegistration&
ProviderCancellation::InterruptRegistration::operator=(
    InterruptRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        key_ = other.key_;
    }
    return *this;
}

ProviderCancellation::InterruptRegistration::~InterruptRegistration() { reset(); }

void ProviderCancellation::InterruptRegistration::reset() noexcept {
    if (!owner_) return;
    owner_->unregister_interrupt(key_);
    owner_ = nullptr;
}

ProviderCancellation::Binding::Binding(
    ProviderCancellation* current, ProviderCancellation* previous) noexcept
    : current_(current), previous_(previous), active_(true) {}

ProviderCancellation::Binding::Binding(Binding&& other) noexcept
    : current_(other.current_), previous_(other.previous_),
      active_(std::exchange(other.active_, false)) {}

ProviderCancellation::Binding& ProviderCancellation::Binding::operator=(
    Binding&& other) noexcept {
    if (this != &other) {
        if (active_) bound_cancellation = previous_;
        current_ = other.current_;
        previous_ = other.previous_;
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

ProviderCancellation::Binding::~Binding() {
    if (active_) bound_cancellation = previous_;
}

void ProviderCancellation::Binding::finish() {
    if (!active_) return;
    current_->check();
    bound_cancellation = previous_;
    active_ = false;
}

void ProviderCancellation::check() const {
    std::scoped_lock guard(lock_);
    if (cancelled_) throw ProviderCancelled();
}

void ProviderCancellation::cancel() {
    // Registered callbacks must be short, nonblocking resource interrupts.
    // Holding the lock prevents unregister/close from racing a callback onto a
    // reused descriptor, matching the source implementation.
    std::scoped_lock guard(lock_);
    if (cancelled_) return;
    cancelled_ = true;
    for (const auto& [_, interrupt] : interrupts_) interrupt();
}

ProviderCancellation::InterruptRegistration ProviderCancellation::interruptible(
    std::function<void()> interrupt) {
    std::scoped_lock guard(lock_);
    if (cancelled_) throw ProviderCancelled();
    const auto key = next_key_++;
    interrupts_.emplace(key, std::move(interrupt));
    return InterruptRegistration(this, key);
}

ProviderCancellation::Binding ProviderCancellation::bind() {
    auto* previous = bound_cancellation;
    bound_cancellation = this;
    try {
        check();
    } catch (...) {
        bound_cancellation = previous;
        throw;
    }
    return Binding(this, previous);
}

void ProviderCancellation::unregister_interrupt(const std::size_t key) noexcept {
    try {
        std::scoped_lock guard(lock_);
        interrupts_.erase(key);
    } catch (...) {
        // Destruction and scope unwinding cannot expose an exception.
    }
}

ProviderCancellation* current_cancellation() noexcept { return bound_cancellation; }

void check_cancelled() {
    if (auto* cancellation = current_cancellation()) cancellation->check();
}

}  // namespace swegca::world
