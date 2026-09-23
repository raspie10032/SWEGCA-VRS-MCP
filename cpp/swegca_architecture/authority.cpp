#include "swegca_architecture/authority.hpp"

#include <atomic>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {

namespace detail {

class AuthorityRegistry final {
public:
    using RangeEntry = std::pair<const std::uint64_t, std::uint64_t>;
    using Ranges = std::map<std::uint64_t, std::uint64_t, std::less<>,
                           MemoryLedger::Allocator<RangeEntry>>;
    struct LiveToken {
        std::weak_ptr<CapabilityToken> token;
        Ranges::node_type retirement;
    };
    using LiveEntry = std::pair<const std::uint64_t, LiveToken>;

    AuthorityRegistry(const MemoryLedger::Account& memory, std::uint64_t issuer_instance);

    MemoryLedger::Account memory;
    std::mutex mutex;
    std::uint64_t issuer_instance;
    std::uint64_t next_nonce = 1;
    std::map<std::uint64_t, LiveToken, std::less<>,
             MemoryLedger::Allocator<LiveEntry>> live;
    Ranges spent_ranges;
};

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:28-45
AuthorityRegistry::AuthorityRegistry(const MemoryLedger::Account& account,
                                     std::uint64_t issuer_instance_value)
    : memory(account), issuer_instance(issuer_instance_value),
      live(std::less<>{}, account.allocator<LiveEntry>()),
      spent_ranges(std::less<>{}, account.allocator<RangeEntry>()) {}

}  // namespace detail

namespace {

std::atomic<std::uint64_t> next_issuer_instance{1};

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:95-150
std::uint64_t allocate_issuer_instance() {
    auto candidate = next_issuer_instance.load(std::memory_order_relaxed);
    while (candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next_issuer_instance.compare_exchange_weak(
                candidate, candidate + 1, std::memory_order_relaxed))
            return candidate;
    }
    throw std::overflow_error("authority_issuer_space_exhausted");
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:232-241
bool was_spent_locked(const detail::AuthorityRegistry& registry,
                      std::uint64_t nonce) {
    auto following = registry.spent_ranges.upper_bound(nonce);
    if (following == registry.spent_ranges.begin()) return false;
    --following;
    return nonce <= following->second;
}

// Re-created (user@2026-09-23): merged spent ranges grow with live nonce gaps,
// not with the number of successfully consumed capabilities.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:232-241
void mark_spent_locked(detail::AuthorityRegistry& registry,
                       std::uint64_t nonce,
                       detail::AuthorityRegistry::Ranges::node_type retirement) {
    if (was_spent_locked(registry, nonce))
        throw std::logic_error("authority_capability_already_spent");

    auto following = registry.spent_ranges.upper_bound(nonce);
    auto first = nonce;
    auto last = nonce;
    if (following != registry.spent_ranges.begin()) {
        const auto prior = std::prev(following);
        if (prior->second != std::numeric_limits<std::uint64_t>::max() &&
            prior->second + 1 == nonce) {
            first = prior->first;
            registry.spent_ranges.erase(prior);
        }
    }
    if (following != registry.spent_ranges.end() &&
        last != std::numeric_limits<std::uint64_t>::max() &&
        last + 1 == following->first) {
        last = following->second;
        registry.spent_ranges.erase(following);
    }
    // The node was allocated before issuance, so retiring at the budget limit
    // never needs another allocation. Preserve the same merged nonce ranges.
    retirement.key() = first;
    retirement.mapped() = last;
    registry.spent_ranges.insert(std::move(retirement));
}

// Re-created (user@2026-09-23): dropping an unused capability immediately
// closes its nonce instead of relying on a separate cleanup subsystem.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:232-241
void retire_abandoned_token(detail::AuthorityRegistry& registry,
                            std::uint64_t nonce) noexcept {
    std::lock_guard guard(registry.mutex);
    const auto found = registry.live.find(nonce);
    if (found == registry.live.end()) return;
    if (!found->second.token.expired()) return;
    auto retirement = std::move(found->second.retirement);
    registry.live.erase(found);
    try {
        mark_spent_locked(registry, nonce, std::move(retirement));
    } catch (...) {
        std::terminate();
    }
}

}  // namespace

namespace detail {

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:232-241
CapabilityToken::~CapabilityToken() {
    if (retired_) return;
    if (const auto registry = registry_.lock())
        retire_abandoned_token(*registry, descriptor_.nonce);
}

}  // namespace detail

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:28-45
MainAuthorityLedger::MainAuthorityLedger(const MemoryLedger::Account& memory)
    : registry_(std::allocate_shared<detail::AuthorityRegistry>(
          memory.allocator<detail::AuthorityRegistry>(), memory, allocate_issuer_instance())) {}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:37-45
MainAuthorityLedger::~MainAuthorityLedger() {
    registry_.reset();
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:95-150
std::size_t MainAuthorityLedger::live_capability_count() const {
    std::lock_guard guard(registry_->mutex);
    return registry_->live.size();
}

// Re-created (user@2026-09-23): private one-use nonce issuance for the six
// authority domains required by ARCHITECTURE_SPEC.md@5901a5a:88-99.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:232-241
std::shared_ptr<detail::CapabilityToken> MainAuthorityLedger::issue_token(
    AuthorityDomain domain, const OwnerId& owner,
    const StateGeneration& generation, const Digest256& operation) {
    std::lock_guard guard(registry_->mutex);
    if (registry_->next_nonce == 0 ||
        registry_->next_nonce == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("authority_nonce_space_exhausted");
    const auto nonce = registry_->next_nonce++;
    // Reserve retirement storage now; abandoning or consuming a live token
    // must remain possible when Main has no spare allocation budget.
    detail::AuthorityRegistry::Ranges prepared(
        std::less<>{}, registry_->memory.allocator<detail::AuthorityRegistry::RangeEntry>());
    prepared.emplace(nonce, nonce);
    auto retirement = prepared.extract(prepared.begin());
    CapabilityDescriptor descriptor{
        domain, registry_->issuer_instance, nonce, owner, generation, operation};
    auto allocator = registry_->memory.allocator<detail::CapabilityToken>();
    auto* raw = allocator.allocate(1);
    try {
        // MainAuthorityLedger itself retains private construction authority.
        ::new (static_cast<void*>(raw)) detail::CapabilityToken(std::move(descriptor), registry_);
    } catch (...) {
        allocator.deallocate(raw, 1);
        throw;
    }
    // shared_ptr invokes this deleter if allocating its control block fails.
    auto token = std::shared_ptr<detail::CapabilityToken>(raw,
        [allocator](detail::CapabilityToken* value) mutable noexcept {
            value->~CapabilityToken();
            allocator.deallocate(value, 1);
        }, registry_->memory.allocator<detail::CapabilityToken>());
    const auto [position, inserted] = registry_->live.emplace(
        nonce, detail::AuthorityRegistry::LiveToken{token, std::move(retirement)});
    if (!inserted || position->second.token.lock() != token)
        throw std::logic_error("authority_nonce_reused");
    token->retired_ = false;
    return token;
}

// Re-created (user@2026-09-23): consume by rvalue and mark spent before the
// authorized mutation; failed mutations require a new gate decision.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:232-241
CapabilityDescriptor MainAuthorityLedger::consume_token(
    AuthorityDomain expected,
    std::shared_ptr<detail::CapabilityToken>&& capability,
    const StateGeneration& current_generation,
    const Digest256& actual_operation) {
    if (!capability)
        throw std::invalid_argument("authority_capability_not_live");
    const auto& descriptor = capability->descriptor();
    {
        std::lock_guard guard(registry_->mutex);
        if (descriptor.domain != expected ||
            descriptor.issuer_instance != registry_->issuer_instance ||
            capability->registry_.lock() != registry_)
            throw std::invalid_argument(
                "authority_capability_wrong_issuer_or_domain");
        if (was_spent_locked(*registry_, descriptor.nonce))
            throw std::invalid_argument("authority_capability_already_spent");
        const auto found = registry_->live.find(descriptor.nonce);
        if (found == registry_->live.end() ||
            found->second.token.lock() != capability)
            throw std::invalid_argument("authority_capability_unknown");
        auto retirement = std::move(found->second.retirement);
        registry_->live.erase(found);
        mark_spent_locked(*registry_, descriptor.nonce, std::move(retirement));
        capability->retired_ = true;
    }
    auto consumed = std::move(capability->descriptor_);
    capability.reset();
    if (consumed.generation != current_generation ||
        consumed.operation != actual_operation)
        throw std::invalid_argument("authority_capability_binding_mismatch");
    return consumed;
}

}  // namespace swegca::architecture
