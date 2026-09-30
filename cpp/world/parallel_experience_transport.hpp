#pragma once

#include "world/cognitive_state.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view parallel_experience_transport_source_sha256 =
    "de0d584a297d096f396b9a98cffc462327956ac18216eba58b6b2f1a0f481b6a";

[[nodiscard]] JsonValue::Object proposal_authority_false();

class DetachedProposalRequest final {
public:
    DetachedProposalRequest(
        std::string request_id, std::string source_episode_id,
        std::string full_current_pair_snapshot_id, std::string source_revision,
        std::string prompt_utf8, std::string current_evidence_json_utf8);

    [[nodiscard]] static DetachedProposalRequest detach(
        std::string request_id, std::string source_episode_id,
        std::string full_current_pair_snapshot_id, std::string source_revision,
        std::string prompt, JsonValue::Object current_evidence);

    [[nodiscard]] std::string prompt() const;
    [[nodiscard]] JsonValue::Object receipt() const;

    const std::string request_id;
    const std::string source_episode_id;
    const std::string full_current_pair_snapshot_id;
    const std::string source_revision;
    const std::string prompt_utf8;
    const std::string current_evidence_json_utf8;
    const std::string request_sha256;
};

struct ProposalTransportResult final {
    std::string request_id;
    std::string request_sha256;
    std::string source_episode_id;
    std::string full_current_pair_snapshot_id;
    std::string status;
    std::string batch_id;
    std::size_t batch_size{};
    std::int64_t queue_wait_ns{};
    std::int64_t engine_elapsed_ns{};
    std::int64_t completed_at_ns{};
    std::optional<std::string> proposal;
    std::optional<std::string> error_type;
    std::optional<std::string> error;

    ProposalTransportResult(
        std::string request_id, std::string request_sha256,
        std::string source_episode_id, std::string full_current_pair_snapshot_id,
        std::string status, std::string batch_id, std::size_t batch_size,
        std::int64_t queue_wait_ns, std::int64_t engine_elapsed_ns,
        std::int64_t completed_at_ns,
        std::optional<std::string> proposal = std::nullopt,
        std::optional<std::string> error_type = std::nullopt,
        std::optional<std::string> error = std::nullopt);

    [[nodiscard]] JsonValue::Object receipt() const;
};

class ResidentProposalEngine {
public:
    virtual ~ResidentProposalEngine() = default;
    [[nodiscard]] virtual std::string model_instance_id() const = 0;
    [[nodiscard]] virtual std::size_t model_load_count() const = 0;
    [[nodiscard]] virtual std::vector<std::string> generate_batch(
        const std::vector<DetachedProposalRequest>& requests) = 0;
};

class BackpressureRejected final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DuplicateRequestConflict final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class ResidentMicrobatchCoordinator final {
public:
    using Clock = std::function<std::int64_t()>;

    ResidentMicrobatchCoordinator(
        std::shared_ptr<ResidentProposalEngine> engine,
        std::size_t maximum_queue_depth, std::size_t maximum_batch_size,
        double maximum_batch_wait_ms, Clock clock_ns = {});
    ResidentMicrobatchCoordinator(const ResidentMicrobatchCoordinator&) = delete;
    ResidentMicrobatchCoordinator& operator=(const ResidentMicrobatchCoordinator&) = delete;
    ~ResidentMicrobatchCoordinator();

    [[nodiscard]] std::shared_future<ProposalTransportResult> submit(
        DetachedProposalRequest request, double timeout_seconds = 0.0);
    void wait_idle();
    void close();
    [[nodiscard]] JsonValue::Object receipt() const;

private:
    struct QueuedRequest final {
        DetachedProposalRequest request;
        std::shared_ptr<std::promise<ProposalTransportResult>> promise;
        std::int64_t queued_at_ns{};
    };

    void run();
    void execute_batch(std::vector<QueuedRequest> batch);

    std::shared_ptr<ResidentProposalEngine> engine_;
    const std::size_t maximum_queue_depth_;
    const std::size_t maximum_batch_size_;
    const std::int64_t maximum_batch_wait_ns_;
    const Clock clock_ns_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::condition_variable space_;
    std::condition_variable idle_;
    std::deque<QueuedRequest> queue_;
    std::map<std::string, std::shared_future<ProposalTransportResult>, std::less<>> futures_;
    std::map<std::string, std::string, std::less<>> digests_;
    std::size_t accepted_unique_{};
    std::size_t coalesced_duplicates_{};
    std::size_t backpressure_rejections_{};
    std::size_t conflicting_duplicates_{};
    std::size_t completed_{};
    std::size_t failed_{};
    std::size_t batch_count_{};
    std::size_t largest_batch_{};
    std::size_t active_batches_{};
    bool closed_{};
    bool stop_{};
    std::thread worker_;
};

}  // namespace swegca::world
