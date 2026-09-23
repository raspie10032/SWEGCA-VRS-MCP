#pragma once

#include "swegca_vrs/core_digest.hpp"
#include "swegca_vrs/core_kernel.hpp"
#include "swegca_vrs/allocation.hpp"

#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_vrs/journal_format.hpp"
#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/identity_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Autonomous cognition as state-transition input (board §3H, §10 step 8):
// observe, hypothesize, request evidence, collect, observe the evidence
// result, verify, remember, propose an action, observe its result. A
// transition is a proposed update of Main's control state and a receipt;
// its memory-write and tool-action intents carry no capability (static
// NoAuthority): a separate Main authority decides whether anything is
// executed (board §8 L3 :482-492, invariant 12).
// Nano-core: `advance_autonomy` is a pure batch-free kernel (no allocation,
// lock, exception or I/O); the shell validates inputs, copies onto Main's
// account and digests.
// Rule: board @cefdc3f §3H :185-194, §8 :482-492; L3
// mosaic_autonomous_cognition.py@5901a5a:1-410.
namespace swegca::vrs {

enum class AutonomyPhase : std::uint8_t {
    observe = 1,
    hypothesize = 2,
    request_evidence = 3,
    collect_evidence = 4,
    observe_evidence_result = 5,
    verify = 6,
    remember = 7,
    act = 8,
    observe_result = 9,
    abstain = 10,
};

enum class AutonomyEventKind : std::uint8_t {
    observation = 1,
    hypothesis = 2,
    evidence_request = 3,
    evidence_action = 4,
    evidence_result = 5,
    verification = 6,
    memory_commit = 7,
    action_proposal = 8,
    action_result = 9,
    hypothesis_abandon = 10,
};

// Every reason the author's transition reports, accepted or not.
enum class AutonomyReason : std::uint8_t {
    accepted = 1,
    duplicate_event,
    hypothesis_not_abandonable,
    hypothesis_mismatch,
    hypothesis_abandoned,
    unexpected_event_for_phase,
    hypothesis_requires_provenance,
    evidence_request_requires_axes,
    collect_with_tool_requires_boolean,
    evidence_tool_collection_not_configured,
    evidence_action_not_allowed,
    evidence_action_not_reversible,
    evidence_action_confidence_too_low,
    evidence_action_requires_requested_axes,
    evidence_result_requires_boolean_success,
    recover_evidence_action,
    evidence_failure_budget_exhausted,
    verification_requires_accumulator_decision,
    hypothesis_rejected,
    additional_evidence_required,
    memory_commit_requires_verified_provenance,
    action_not_allowed,
    action_not_reversible,
    action_confidence_too_low,
    action_requires_verified_memory,
    action_result_requires_boolean_success,
    recover_action,
    failure_budget_exhausted,
};

// The author's reason text.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:189-410
[[nodiscard]] std::string_view autonomy_reason_text(AutonomyReason reason) noexcept;

enum class VerificationStatus : std::uint8_t { none = 0, accept = 1, reject = 2, abstain = 3, abandoned = 4 };

// One payload field as the producer sent it: absent, present with a value of
// another type (the author's `isinstance` failures), or present.
template <class T>
struct PayloadField {
    enum class State : std::uint8_t { absent = 0, wrong_type = 1, present = 2 };
    State state = State::absent;
    T value{};
};

// The payload fields the machine reads; `digest` is the SHA-256 of the
// complete canonical payload, derived by the event parser from its bytes.
// The event's experience kind follows its actual source and lineage.
struct AutonomyPayloadView {
    PayloadField<std::span<const std::string_view>> requested_axes;
    PayloadField<bool> collect_with_tool;
    PayloadField<std::string_view> action;
    PayloadField<bool> success;
    PayloadField<std::string_view> memory_ref;
    PayloadField<std::string_view> content_hash;
    DigestBytes digest{};
};

// One event, borrowed for one step.
struct AutonomyEventView {
    std::string_view event_id;
    AutonomyEventKind kind = AutonomyEventKind::observation;
    std::optional<std::string_view> hypothesis_id;
    std::span<const std::string_view> evidence_refs;
    std::string_view source_family;
    Digest256 context;
    double confidence = 0;  // [0, 1]
    AutonomyPayloadView payload;
};

// Validates an event as the author's AutonomyEvent does: identity texts for
// the event id, source family, hypothesis id and every reference, and
// confidence finite in [0, 1]; the kind in range. Payload values are not
// validated here: the phase that reads one rejects it by the author's reason.
// Throws `autonomy_event_invalid:<field>`. The payload view is the caller's
// reading of the payload `digest` names; binding the two is the adapter's
// duty; the payload digest alone is not the event record's raw digest.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
void validate_autonomy_event(const AutonomyEventView& event);

struct ToolActionTag { static constexpr std::string_view name = "tool_action"; };
using ToolAction = TextIdentity<ToolActionTag>;

// Main's configuration: which tool actions (and evidence-collection tool
// actions) may be proposed, which of them are reversible, and the
// confidence floors and failure budgets.
class AutonomyConfig final {
public:
    struct Input {
        std::span<const std::string_view> allowed_tool_actions;
        std::span<const std::string_view> reversible_tool_actions;
        double minimum_action_confidence = 0.8;
        std::uint32_t maximum_action_failures = 2;
        std::span<const std::string_view> allowed_evidence_tool_actions;
        std::span<const std::string_view> reversible_evidence_tool_actions;
        double minimum_evidence_action_confidence = 0.8;
        std::uint32_t maximum_evidence_action_failures = 2;
    };

    // Checks in the author's __post_init__ order; throws
    // `autonomy_config_invalid:<rule>`, or the identity-rule code of an
    // action that is not an identity text.
    AutonomyConfig(const AllocationContext& memory, const Input& input);

    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] bool allowed(std::string_view action) const noexcept;
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] bool reversible(std::string_view action) const noexcept;
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] bool evidence_allowed(std::string_view action) const noexcept;
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] bool evidence_reversible(std::string_view action) const noexcept;
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] bool evidence_collection_configured() const noexcept { return !evidence_allowed_.empty(); }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] double minimum_action_confidence() const noexcept { return minimum_action_confidence_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] std::uint32_t maximum_action_failures() const noexcept { return maximum_action_failures_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] double minimum_evidence_action_confidence() const noexcept {
        return minimum_evidence_action_confidence_;
    }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] std::uint32_t maximum_evidence_action_failures() const noexcept {
        return maximum_evidence_action_failures_;
    }
    // Covers every field; the sets in increasing order.
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
    [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

private:
    using Actions = journal::LedgerVector<ToolAction>;  // sorted, unique

    Actions allowed_;
    Actions reversible_;
    Actions evidence_allowed_;
    Actions evidence_reversible_;
    double minimum_action_confidence_ = 0.8;
    std::uint32_t maximum_action_failures_ = 2;
    double minimum_evidence_action_confidence_ = 0.8;
    std::uint32_t maximum_evidence_action_failures_ = 2;
    Digest256 digest_{DigestBytes{}};
};

struct HypothesisIdTag { static constexpr std::string_view name = "hypothesis_id"; };
struct AutonomyEventIdTag { static constexpr std::string_view name = "autonomy_event_id"; };
using HypothesisId = TextIdentity<HypothesisIdTag>;
using AutonomyEventId = TextIdentity<AutonomyEventIdTag>;
// A payload string kept as the producer sent it (the author accepts any
// nonempty string), on Main's account.
using PayloadText = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;

// The part of Main's goal and self state the machine owns (the author's
// `autonomy_*`, hypothesis, verification, memory and pending-action keys),
// on Main's account. Its canonical bytes are what the Main writer stores
// in the goal/self state; the digest covers every field.
class AutonomyControl final {
public:
    // Phase observe, step 0, everything else absent or zero.
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:151-152
    [[nodiscard]] static AutonomyControl initial(const AllocationContext& memory);
    // Decodes canonical bytes (`autonomy_control_invalid` otherwise).
    [[nodiscard]] static AutonomyControl decode(const AllocationContext& memory,
                                                std::span<const std::byte> bytes);
    [[nodiscard]] journal::LedgerBytes encode(const AllocationContext& memory) const;
    // SHA-256 of the canonical bytes (scratch charged to `memory`).
    [[nodiscard]] Digest256 digest(const AllocationContext& memory) const;

    AutonomyPhase phase = AutonomyPhase::observe;
    std::uint64_t step = 0;
    std::optional<AutonomyEventId> last_event_id;
    std::optional<HypothesisId> active_hypothesis_id;
    std::optional<HypothesisId> verified_hypothesis_id;
    std::optional<double> hypothesis_confidence;
    journal::LedgerVector<PayloadText> requested_axes;  // in the requester's order
    VerificationStatus verification_status = VerificationStatus::none;
    std::optional<double> verification_lower_bound;
    std::optional<PayloadText> memory_ref;
    std::optional<PayloadText> memory_content_hash;
    std::optional<ToolAction> pending_tool_action;
    std::optional<ToolAction> pending_evidence_tool_action;
    std::uint64_t action_failures = 0;
    std::uint64_t evidence_action_failures = 0;

private:
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:151-152
    explicit AutonomyControl(const AllocationContext& memory)
        : requested_axes(memory.allocator<PayloadText>()) {}
};

// What one accepted step changes in the control state besides phase, step
// and last event (the author's goal_updates and self_updates). Texts view
// the event.
struct AutonomyUpdate {
    bool reset_cycle = false;  // clears hypotheses and memory ref, zeroes both failure counts
    std::optional<std::string_view> active_hypothesis_id;
    std::optional<double> hypothesis_confidence;
    std::optional<std::span<const std::string_view>> requested_axes;  // empty clears
    std::optional<VerificationStatus> verification_status;
    std::optional<std::optional<std::string_view>> verified_hypothesis_id;
    std::optional<double> verification_lower_bound;
    std::optional<std::string_view> memory_ref;
    std::optional<std::string_view> memory_content_hash;
    std::optional<std::optional<std::string_view>> pending_tool_action;
    std::optional<std::optional<std::string_view>> pending_evidence_tool_action;
    std::optional<std::uint64_t> action_failures;
    std::optional<std::uint64_t> evidence_action_failures;
};

// The kernel's result. Intents are what the step proposes, never
// permission: a separate Main authority consumes them.
struct AutonomyStep {
    bool accepted = false;
    AutonomyReason reason = AutonomyReason::accepted;
    AutonomyPhase prior_phase = AutonomyPhase::observe;
    AutonomyPhase next_phase = AutonomyPhase::observe;
    bool memory_write_intent = false;
    std::optional<std::string_view> tool_action_intent;
    std::optional<std::string_view> evidence_tool_action_intent;
    AutonomyUpdate update;
};

// The transition function (author advance_autonomous_cognition): pure, no
// allocation, lock, exception or I/O. `decision` is the accumulator judgment
// for the verify phase, absent otherwise.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:189-410
[[nodiscard]] AutonomyStep advance_autonomy(const AutonomyControl& control, const AutonomyEventView& event,
                                            const AutonomyConfig& config,
                                            const kernel::EvidenceJudgment* decision) noexcept;

class EvidenceDecision;

// One transition as Main keeps it: the successor control state and a
// receipt digest over the prior control, the event, the configuration, the
// decision (when one was given), the step and the successor. Its static
// authority is NoAuthority: an intent it records converts to nothing.
class AutonomyTransition final {
public:
    using Authority = NoAuthority;
    static constexpr bool external_action_authorized = false;
    static constexpr bool memory_write_authorized = false;
    static constexpr bool tool_action_authorized = false;

    AutonomyTransition(AutonomyTransition&&) noexcept = default;
    AutonomyTransition& operator=(AutonomyTransition&&) = delete;
    AutonomyTransition(const AutonomyTransition&) = delete;
    AutonomyTransition& operator=(const AutonomyTransition&) = delete;
    ~AutonomyTransition() = default;

    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] bool accepted() const noexcept { return accepted_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] AutonomyReason reason() const noexcept { return reason_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] AutonomyPhase prior_phase() const noexcept { return prior_phase_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] AutonomyPhase next_phase() const noexcept { return successor_.phase; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] bool memory_write_intent() const noexcept { return memory_write_intent_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] const std::optional<ToolAction>& tool_action_intent() const noexcept { return tool_action_intent_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] const std::optional<ToolAction>& evidence_tool_action_intent() const noexcept {
        return evidence_tool_action_intent_;
    }
    // The proposed control state (the prior one, unchanged, when rejected).
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
    [[nodiscard]] const AutonomyControl& successor() const noexcept { return successor_; }
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

private:
    friend AutonomyTransition advance_autonomous_cognition(const AllocationContext&, const AutonomyControl&,
                                                           const AutonomyEventView&, const AutonomyConfig&,
                                                           const EvidenceDecision*);
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:123-134
    AutonomyTransition(AutonomyControl successor) : successor_(std::move(successor)) {}

    bool accepted_ = false;
    AutonomyReason reason_ = AutonomyReason::accepted;
    AutonomyPhase prior_phase_ = AutonomyPhase::observe;
    bool memory_write_intent_ = false;
    std::optional<ToolAction> tool_action_intent_;
    std::optional<ToolAction> evidence_tool_action_intent_;
    AutonomyControl successor_;
    Digest256 digest_{DigestBytes{}};
};

// The shell around `advance_autonomy`: validates the event, runs the kernel
// on the decision's judgment, copies the successor onto `memory` and
// digests the receipt. A rejected step keeps the prior control state.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:189-410
[[nodiscard]] AutonomyTransition advance_autonomous_cognition(const AllocationContext& memory,
                                                              const AutonomyControl& control,
                                                              const AutonomyEventView& event,
                                                              const AutonomyConfig& config,
                                                              const EvidenceDecision* decision);

}  // namespace swegca::vrs
