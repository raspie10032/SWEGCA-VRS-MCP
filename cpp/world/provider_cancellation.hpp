#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace swegca::world {

inline constexpr std::string_view provider_cancellation_source_sha256 =
    "44cd4a55bbff02029d609750d5c3d2b23a4be99ec9decb9cb12ab41083008953";

class ProviderCancelled final : public std::runtime_error {
public:
    ProviderCancelled();
};

// Request-local transport cancellation, never cognitive or model authority.
// No detached evidence field or persistent worker state carries this capability.
class ProviderCancellation final {
public:
    class InterruptRegistration final {
    public:
        InterruptRegistration() = default;
        InterruptRegistration(const InterruptRegistration&) = delete;
        InterruptRegistration& operator=(const InterruptRegistration&) = delete;
        InterruptRegistration(InterruptRegistration&& other) noexcept;
        InterruptRegistration& operator=(InterruptRegistration&& other) noexcept;
        ~InterruptRegistration();

        void reset() noexcept;

    private:
        friend class ProviderCancellation;
        InterruptRegistration(ProviderCancellation* owner, std::size_t key) noexcept;

        ProviderCancellation* owner_{};
        std::size_t key_{};
    };

    class Binding final {
    public:
        Binding() = default;
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;
        Binding(Binding&& other) noexcept;
        Binding& operator=(Binding&& other) noexcept;
        ~Binding();

        void finish();

    private:
        friend class ProviderCancellation;
        Binding(ProviderCancellation* current, ProviderCancellation* previous) noexcept;

        ProviderCancellation* current_{};
        ProviderCancellation* previous_{};
        bool active_{};
    };

    void check() const;
    void cancel();
    [[nodiscard]] InterruptRegistration interruptible(std::function<void()> interrupt);
    [[nodiscard]] Binding bind();

private:
    friend class InterruptRegistration;
    void unregister_interrupt(std::size_t key) noexcept;

    mutable std::mutex lock_;
    bool cancelled_{};
    std::size_t next_key_{1};
    std::unordered_map<std::size_t, std::function<void()>> interrupts_;
};

[[nodiscard]] ProviderCancellation* current_cancellation() noexcept;
void check_cancelled();

}  // namespace swegca::world
