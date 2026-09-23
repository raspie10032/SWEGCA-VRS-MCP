#pragma once

#include "swegca_vrs/allocation.hpp"

#include "swegca_vrs/authority_roles.hpp"
#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/identity_types.hpp"
#include "swegca_vrs/published_state_id.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

enum class AuthorityDomain : std::uint8_t {
    cognitive_state_commit = 1,
    semantic_memory_promotion = 2,
    external_action = 3,
    training_model_update = 4,
    distribution = 5,
    p3_promotion = 6,
};

template <AuthorityDomain Domain>
class IssueKey;

template <AuthorityDomain Domain>
class ConsumeKey;

template <>
class IssueKey<AuthorityDomain::cognitive_state_commit> final {
public:
    IssueKey(const IssueKey&) = delete;
    IssueKey(IssueKey&&) = default;
    ~IssueKey() = default;
private:
    IssueKey() = default;
    friend class EvidenceGate;
};

template <>
class ConsumeKey<AuthorityDomain::cognitive_state_commit> final {
public:
    ConsumeKey(const ConsumeKey&) = delete;
    ConsumeKey(ConsumeKey&&) = default;
    ~ConsumeKey() = default;
private:
    ConsumeKey() = default;
    friend class MainStateWriter;
};

template <>
class IssueKey<AuthorityDomain::semantic_memory_promotion> final {
public:
    IssueKey(const IssueKey&) = delete;
    IssueKey(IssueKey&&) = default;
    ~IssueKey() = default;
private:
    IssueKey() = default;
    friend class SemanticMemoryGate;
};

template <>
class ConsumeKey<AuthorityDomain::semantic_memory_promotion> final {
public:
    ConsumeKey(const ConsumeKey&) = delete;
    ConsumeKey(ConsumeKey&&) = default;
    ~ConsumeKey() = default;
private:
    ConsumeKey() = default;
    friend class SemanticMemoryWriter;
};

template <>
class IssueKey<AuthorityDomain::external_action> final {
public:
    IssueKey(const IssueKey&) = delete;
    IssueKey(IssueKey&&) = default;
    ~IssueKey() = default;
private:
    IssueKey() = default;
    friend class ExternalActionGate;
};

template <>
class ConsumeKey<AuthorityDomain::external_action> final {
public:
    ConsumeKey(const ConsumeKey&) = delete;
    ConsumeKey(ConsumeKey&&) = default;
    ~ConsumeKey() = default;
private:
    ConsumeKey() = default;
    friend class ExternalActionExecutor;
};

template <>
class IssueKey<AuthorityDomain::training_model_update> final {
public:
    IssueKey(const IssueKey&) = delete;
    IssueKey(IssueKey&&) = default;
    ~IssueKey() = default;
private:
    IssueKey() = default;
    friend class TrainingModelUpdateGate;
};

template <>
class ConsumeKey<AuthorityDomain::training_model_update> final {
public:
    ConsumeKey(const ConsumeKey&) = delete;
    ConsumeKey(ConsumeKey&&) = default;
    ~ConsumeKey() = default;
private:
    ConsumeKey() = default;
    friend class TrainingModelUpdateExecutor;
};

template <>
class IssueKey<AuthorityDomain::distribution> final {
public:
    IssueKey(const IssueKey&) = delete;
    IssueKey(IssueKey&&) = default;
    ~IssueKey() = default;
private:
    IssueKey() = default;
    friend class DistributionGate;
};

template <>
class ConsumeKey<AuthorityDomain::distribution> final {
public:
    ConsumeKey(const ConsumeKey&) = delete;
    ConsumeKey(ConsumeKey&&) = default;
    ~ConsumeKey() = default;
private:
    ConsumeKey() = default;
    friend class DistributionExecutor;
};

template <>
class IssueKey<AuthorityDomain::p3_promotion> final {
public:
    IssueKey(const IssueKey&) = delete;
    IssueKey(IssueKey&&) = default;
    ~IssueKey() = default;
private:
    IssueKey() = default;
    friend class P3PromotionGate;
};

template <>
class ConsumeKey<AuthorityDomain::p3_promotion> final {
public:
    ConsumeKey(const ConsumeKey&) = delete;
    ConsumeKey(ConsumeKey&&) = default;
    ~ConsumeKey() = default;
private:
    ConsumeKey() = default;
    friend class P3PromotionExecutor;
};

struct CapabilityDescriptor final {
    AuthorityDomain domain;
    std::uint64_t issuer_instance;
    std::uint64_t nonce;
    OwnerId owner;
    PublishedStateId head;
    Digest256 operation;

    auto operator<=>(const CapabilityDescriptor&) const = default;
};

class MainAuthorityLedger;

namespace detail {

class AuthorityRegistry;

// The capability is the only strong owner. The ledger retains a weak reference,
// so abandoning an unused capability cannot keep process-local authority live.
class CapabilityToken final {
public:
    CapabilityToken(const CapabilityToken&) = delete;
    CapabilityToken& operator=(const CapabilityToken&) = delete;
    ~CapabilityToken();

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
    [[nodiscard]] const CapabilityDescriptor& descriptor() const noexcept {
        return descriptor_;
    }

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:95-150
    [[nodiscard]] bool issuer_alive() const noexcept {
        return !registry_.expired();
    }

private:
    friend class ::swegca::vrs::MainAuthorityLedger;
    friend class AuthorityRegistry;

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:28-45
    explicit CapabilityToken(
        CapabilityDescriptor descriptor,
        std::weak_ptr<AuthorityRegistry> registry)
        : descriptor_(std::move(descriptor)),
          registry_(std::move(registry)) {}

    CapabilityDescriptor descriptor_;
    std::weak_ptr<AuthorityRegistry> registry_;
    // Registration activates cleanup only after the ledger's live map owns the
    // weak entry. Pre-registration destruction must not re-enter its mutex.
    bool retired_ = true;
};

}  // namespace detail

// Each Domain specialization is a separate, non-convertible authority type.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:88-99, 135, 139-160; SWEGCA I10.
template <AuthorityDomain Domain>
class AuthorityCapability final {
public:
    AuthorityCapability(const AuthorityCapability&) = delete;
    AuthorityCapability& operator=(const AuthorityCapability&) = delete;

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:95-150
    AuthorityCapability(AuthorityCapability&& other) noexcept = default;

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:95-150
    AuthorityCapability& operator=(AuthorityCapability&& other) noexcept = default;

    ~AuthorityCapability() = default;

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:37-45
    [[nodiscard]] bool valid() const noexcept {
        return token_ != nullptr && token_->issuer_alive();
    }

    // Visible fields can be audited but cannot construct or consume authority.
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
    [[nodiscard]] const CapabilityDescriptor& descriptor() const;

private:
    friend class MainAuthorityLedger;

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:28-45
    explicit AuthorityCapability(std::shared_ptr<detail::CapabilityToken> token)
        : token_(std::move(token)) {
        if (!token_ || token_->descriptor().domain != Domain)
            throw std::invalid_argument("authority_domain_changed");
    }

    std::shared_ptr<detail::CapabilityToken> token_;
};

using CognitiveStateCommitAuthority =
    AuthorityCapability<AuthorityDomain::cognitive_state_commit>;
using SemanticMemoryPromotionAuthority =
    AuthorityCapability<AuthorityDomain::semantic_memory_promotion>;
using ExternalActionAuthority =
    AuthorityCapability<AuthorityDomain::external_action>;
using TrainingModelUpdateAuthority =
    AuthorityCapability<AuthorityDomain::training_model_update>;
using DistributionAuthority =
    AuthorityCapability<AuthorityDomain::distribution>;
using P3PromotionAuthority =
    AuthorityCapability<AuthorityDomain::p3_promotion>;

// Main owns this process-local ledger and never persists it. It starts empty on
// every process start. Public issue/consume entry points require domain-specific
// keys whose constructors belong only to the matching Main-owned role.
// Re-created (user@2026-09-23): exact-once process-local capability nonce.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:88-99, 135, 139-160.
class MainAuthorityLedger final {
public:
    MainAuthorityLedger(const MainAuthorityLedger&) = delete;
    MainAuthorityLedger& operator=(const MainAuthorityLedger&) = delete;
    MainAuthorityLedger(MainAuthorityLedger&&) = delete;
    MainAuthorityLedger& operator=(MainAuthorityLedger&&) = delete;

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:37-45
    ~MainAuthorityLedger();

    // Audit-only count. It exposes no token and grants no authority.
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:95-150
    [[nodiscard]] std::size_t live_capability_count() const;

    template <AuthorityDomain Domain>
    [[nodiscard]] AuthorityCapability<Domain> issue(
        IssueKey<Domain>, const OwnerId& owner,
        const PublishedStateId& head, const Digest256& operation);

    template <AuthorityDomain Domain>
    [[nodiscard]] CapabilityDescriptor consume(
        ConsumeKey<Domain>, AuthorityCapability<Domain>&& capability,
        const PublishedStateId& current_head,
        const Digest256& actual_operation);

    // The checks of `consume` without spending: this ledger issued the
    // capability in this domain, it is live and unspent, and it names this
    // publication and operation. Throws as `consume` does. A guarded dry run
    // must hold genuine authority for the exact write it previews.
    template <AuthorityDomain Domain>
    void verify(const ConsumeKey<Domain>&, const AuthorityCapability<Domain>& capability,
                const PublishedStateId& current_head,
                const Digest256& expected_operation) const;

private:
    friend class MainOwner;

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:28-45
    explicit MainAuthorityLedger(const AllocationContext& memory);

    [[nodiscard]] std::shared_ptr<detail::CapabilityToken> issue_token(
        AuthorityDomain domain, const OwnerId& owner,
        const PublishedStateId& head, const Digest256& operation);
    [[nodiscard]] CapabilityDescriptor consume_token(
        AuthorityDomain expected,
        std::shared_ptr<detail::CapabilityToken>&& capability,
        const PublishedStateId& current_head,
        const Digest256& actual_operation);
    void verify_token(AuthorityDomain expected,
                      const std::shared_ptr<detail::CapabilityToken>& capability,
                      const PublishedStateId& current_head,
                      const Digest256& expected_operation) const;

    std::shared_ptr<detail::AuthorityRegistry> registry_;
};

template <AuthorityDomain Domain>
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
const CapabilityDescriptor& AuthorityCapability<Domain>::descriptor() const {
    if (!token_)
        throw std::logic_error("authority_capability_not_live");
    return token_->descriptor();
}

template <AuthorityDomain Domain>
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:28-45
AuthorityCapability<Domain> MainAuthorityLedger::issue(
    IssueKey<Domain>, const OwnerId& owner,
    const PublishedStateId& head, const Digest256& operation) {
    return AuthorityCapability<Domain>(
        issue_token(Domain, owner, head, operation));
}

template <AuthorityDomain Domain>
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
CapabilityDescriptor MainAuthorityLedger::consume(
    ConsumeKey<Domain>, AuthorityCapability<Domain>&& capability,
    const PublishedStateId& current_head,
    const Digest256& actual_operation) {
    return consume_token(Domain, std::move(capability.token_),
                         current_head, actual_operation);
}

template <AuthorityDomain Domain>
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:413-423
void MainAuthorityLedger::verify(
    const ConsumeKey<Domain>&, const AuthorityCapability<Domain>& capability,
    const PublishedStateId& current_head,
    const Digest256& expected_operation) const {
    verify_token(Domain, capability.token_, current_head, expected_operation);
}

}  // namespace swegca::vrs

// The complete EvidenceGate (see authority_roles.hpp).
#include "swegca_vrs/evidence_gate.hpp"
