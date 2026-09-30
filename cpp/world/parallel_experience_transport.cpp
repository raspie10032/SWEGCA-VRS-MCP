#include "world/parallel_experience_transport.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/provider_transport.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <typeinfo>
#include <utility>

namespace swegca::world {
namespace {

std::string hex(const architecture::DigestBytes& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 15U]);
    }
    return result;
}

std::string digest(const std::string_view bytes) {
    architecture::Sha256 hash;
    hash.update(bytes);
    return hex(hash.finish());
}

bool request_id_valid(const std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    const auto initial = static_cast<unsigned char>(value.front());
    if (!((initial >= 'A' && initial <= 'Z') ||
          (initial >= 'a' && initial <= 'z') ||
          (initial >= '0' && initial <= '9'))) return false;
    return std::ranges::all_of(value.substr(1), [](const unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
               byte == ':' || byte == '-';
    });
}

bool digest_valid(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

bool ascii_space(const unsigned char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
           byte == '\f' || byte == '\v';
}

std::string trim(const std::string_view value) {
    const auto begin = std::ranges::find_if_not(value, ascii_space);
    const auto end = std::ranges::find_if_not(value | std::views::reverse, ascii_space).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

std::string nonempty(const std::string_view value, const char* error) {
    auto result = trim(value);
    if (result.empty()) throw std::invalid_argument(error);
    return result;
}

JsonValue optional(const std::optional<std::string>& value) {
    return value ? JsonValue(*value) : JsonValue(nullptr);
}

std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t batch_wait_ns(const double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0 ||
        milliseconds >
            static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
                1'000'000.0)
        throw std::invalid_argument("batch wait cannot be negative");
    return static_cast<std::int64_t>(milliseconds * 1'000'000.0);
}

JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

}  // namespace

JsonValue::Object proposal_authority_false() {
    return {{"semantic", false}, {"world", false}, {"action", false},
            {"persistent_write", false}, {"model_update", false},
            {"distribution", false}, {"p3", false}};
}

DetachedProposalRequest::DetachedProposalRequest(
    std::string request_id_value, std::string source_episode_id_value,
    std::string full_current_pair_snapshot_id_value,
    std::string source_revision_value, std::string prompt_utf8_value,
    std::string current_evidence_json_utf8_value)
    : request_id(std::move(request_id_value)),
      source_episode_id(std::move(source_episode_id_value)),
      full_current_pair_snapshot_id(std::move(full_current_pair_snapshot_id_value)),
      source_revision(std::move(source_revision_value)),
      prompt_utf8(std::move(prompt_utf8_value)),
      current_evidence_json_utf8(std::move(current_evidence_json_utf8_value)),
      request_sha256([&] {
          if (!request_id_valid(request_id))
              throw std::invalid_argument("request ID format changed");
          if (trim(source_episode_id).empty())
              throw std::invalid_argument("source episode ID must be non-empty text");
          if (!digest_valid(full_current_pair_snapshot_id))
              throw std::invalid_argument(
                  "full-current pair snapshot ID must be one SHA-256");
          if (trim(source_revision).empty())
              throw std::invalid_argument("source revision must be non-empty text");
          if (prompt_utf8.empty())
              throw std::invalid_argument("detached prompt must be non-empty bytes");
          const auto evidence = provider_decode(current_evidence_json_utf8);
          if (!evidence.is_object())
              throw std::invalid_argument(
                  "detached current evidence must be one JSON object");
          return digest(semantic_canonical_json(JsonValue::Object{
              {"schema_version", "rozephine-detached-proposal-request-v1"},
              {"request_id", request_id},
              {"source_episode_id", source_episode_id},
              {"full_current_pair_snapshot_id", full_current_pair_snapshot_id},
              {"source_revision", source_revision},
              {"prompt_sha256", digest(prompt_utf8)},
              {"current_evidence_sha256", digest(current_evidence_json_utf8)}}));
      }()) {}

DetachedProposalRequest DetachedProposalRequest::detach(
    std::string request_id, std::string source_episode_id,
    std::string full_current_pair_snapshot_id, std::string source_revision,
    std::string prompt_value, JsonValue::Object current_evidence) {
    return DetachedProposalRequest(
        std::move(request_id), std::move(source_episode_id),
        std::move(full_current_pair_snapshot_id), std::move(source_revision),
        nonempty(prompt_value, "prompt must be non-empty text"),
        semantic_canonical_json(JsonValue(std::move(current_evidence))));
}

std::string DetachedProposalRequest::prompt() const { return prompt_utf8; }

JsonValue::Object DetachedProposalRequest::receipt() const {
    return {
        {"schema_version", "rozephine-detached-proposal-request-receipt-v1"},
        {"request_id", request_id}, {"request_sha256", request_sha256},
        {"source_episode_id", source_episode_id},
        {"source_revision", source_revision},
        {"full_current_pair_snapshot_id", full_current_pair_snapshot_id},
        {"prompt_utf8_bytes", static_cast<std::int64_t>(prompt_utf8.size())},
        {"prompt_sha256", digest(prompt_utf8)},
        {"current_evidence_utf8_bytes",
         static_cast<std::int64_t>(current_evidence_json_utf8.size())},
        {"current_evidence_sha256", digest(current_evidence_json_utf8)},
        {"detached_transient_snapshot", true},
        {"main_mutable_reference_retained", false}, {"proposal_only", true},
        {"authority", proposal_authority_false()}};
}

ProposalTransportResult::ProposalTransportResult(
    std::string request_id_value, std::string request_sha256_value,
    std::string source_episode_id_value,
    std::string full_current_pair_snapshot_id_value, std::string status_value,
    std::string batch_id_value, const std::size_t batch_size_value,
    const std::int64_t queue_wait_ns_value,
    const std::int64_t engine_elapsed_ns_value,
    const std::int64_t completed_at_ns_value,
    std::optional<std::string> proposal_value,
    std::optional<std::string> error_type_value,
    std::optional<std::string> error_value)
    : request_id(std::move(request_id_value)),
      request_sha256(std::move(request_sha256_value)),
      source_episode_id(std::move(source_episode_id_value)),
      full_current_pair_snapshot_id(std::move(full_current_pair_snapshot_id_value)),
      status(std::move(status_value)), batch_id(std::move(batch_id_value)),
      batch_size(batch_size_value), queue_wait_ns(queue_wait_ns_value),
      engine_elapsed_ns(engine_elapsed_ns_value),
      completed_at_ns(completed_at_ns_value), proposal(std::move(proposal_value)),
      error_type(std::move(error_type_value)), error(std::move(error_value)) {
    if (status != "completed_proposal" && status != "failed_retained")
        throw std::invalid_argument("proposal transport status changed");
    if (status == "completed_proposal") {
        if (!proposal || trim(*proposal).empty())
            throw std::invalid_argument("proposal must be non-empty text");
        if (error_type || error)
            throw std::invalid_argument("completed proposal contains an error");
    } else if (proposal || !error_type || error_type->empty() || !error || error->empty()) {
        throw std::invalid_argument("retained failure fields are incomplete");
    }
    if (!batch_size || queue_wait_ns < 0 || engine_elapsed_ns < 0 ||
        completed_at_ns < 0)
        throw std::invalid_argument("proposal transport timing changed");
}

JsonValue::Object ProposalTransportResult::receipt() const {
    return {
        {"schema_version", "rozephine-proposal-transport-result-v1"},
        {"request_id", request_id}, {"request_sha256", request_sha256},
        {"source_episode_id", source_episode_id},
        {"full_current_pair_snapshot_id", full_current_pair_snapshot_id},
        {"status", status}, {"batch_id", batch_id},
        {"batch_size", static_cast<std::int64_t>(batch_size)},
        {"queue_wait_ns", queue_wait_ns}, {"engine_elapsed_ns", engine_elapsed_ns},
        {"completed_at_ns", completed_at_ns}, {"proposal", optional(proposal)},
        {"error_type", optional(error_type)}, {"error", optional(error)},
        {"worker_context_destroyed_after_return", true},
        {"counts_as_new_experience", 0}, {"actual_outcome_count", 0},
        {"growth_claimed", false}, {"proposal_only", true},
        {"authority", proposal_authority_false()}};
}

ResidentMicrobatchCoordinator::ResidentMicrobatchCoordinator(
    std::shared_ptr<ResidentProposalEngine> engine,
    const std::size_t maximum_queue_depth,
    const std::size_t maximum_batch_size, const double maximum_batch_wait_ms,
    Clock clock_ns)
    : engine_(std::move(engine)), maximum_queue_depth_(maximum_queue_depth),
      maximum_batch_size_(maximum_batch_size),
      maximum_batch_wait_ns_(batch_wait_ns(maximum_batch_wait_ms)),
      clock_ns_(clock_ns ? std::move(clock_ns) : Clock(steady_now_ns)) {
    if (!engine_ || !maximum_queue_depth_ || !maximum_batch_size_)
        throw std::invalid_argument("queue and batch bounds must be positive");
    worker_ = std::thread([this] { run(); });
}

ResidentMicrobatchCoordinator::~ResidentMicrobatchCoordinator() {
    try { close(); } catch (...) { std::terminate(); }
}

std::shared_future<ProposalTransportResult> ResidentMicrobatchCoordinator::submit(
    DetachedProposalRequest request, const double timeout_seconds) {
    if (!std::isfinite(timeout_seconds) || timeout_seconds < 0)
        throw std::invalid_argument("backpressure timeout cannot be negative");
    std::unique_lock lock(mutex_);
    if (closed_) throw std::runtime_error("proposal coordinator is closed");
    if (const auto existing = futures_.find(request.request_id);
        existing != futures_.end()) {
        if (digests_.at(request.request_id) != request.request_sha256) {
            ++conflicting_duplicates_;
            throw DuplicateRequestConflict(
                "request ID was reused with different detached content");
        }
        ++coalesced_duplicates_;
        return existing->second;
    }
    const auto until = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(timeout_seconds);
    while (queue_.size() >= maximum_queue_depth_) {
        if (!timeout_seconds || space_.wait_until(lock, until) == std::cv_status::timeout) {
            ++backpressure_rejections_;
            throw BackpressureRejected("bounded proposal queue remained full");
        }
        if (closed_) throw std::runtime_error("proposal coordinator is closed");
        if (const auto existing = futures_.find(request.request_id);
            existing != futures_.end()) {
            if (digests_.at(request.request_id) != request.request_sha256) {
                ++conflicting_duplicates_;
                throw DuplicateRequestConflict(
                    "request ID was reused with different detached content");
            }
            ++coalesced_duplicates_;
            return existing->second;
        }
    }
    auto promise = std::make_shared<std::promise<ProposalTransportResult>>();
    auto future = promise->get_future().share();
    futures_.emplace(request.request_id, future);
    digests_.emplace(request.request_id, request.request_sha256);
    queue_.push_back({std::move(request), std::move(promise), clock_ns_()});
    ++accepted_unique_;
    available_.notify_one();
    return future;
}

void ResidentMicrobatchCoordinator::run() {
    while (true) {
        std::vector<QueuedRequest> batch;
        {
            std::unique_lock lock(mutex_);
            available_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty() && stop_) return;
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
            space_.notify_all();
            const auto deadline = clock_ns_() + maximum_batch_wait_ns_;
            while (batch.size() < maximum_batch_size_) {
                if (!queue_.empty()) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                    space_.notify_all();
                    continue;
                }
                const auto remaining = deadline - clock_ns_();
                if (remaining <= 0 || stop_) break;
                if (available_.wait_for(lock, std::chrono::nanoseconds(remaining)) ==
                    std::cv_status::timeout) break;
            }
            ++active_batches_;
        }
        execute_batch(std::move(batch));
        {
            std::lock_guard lock(mutex_);
            --active_batches_;
            if (queue_.empty() && !active_batches_) idle_.notify_all();
        }
    }
}

void ResidentMicrobatchCoordinator::execute_batch(std::vector<QueuedRequest> batch) {
    const auto started_ns = clock_ns_();
    std::vector<std::string> request_digests;
    std::vector<DetachedProposalRequest> requests;
    request_digests.reserve(batch.size());
    requests.reserve(batch.size());
    for (const auto& item : batch) {
        request_digests.push_back(item.request.request_sha256);
        requests.push_back(item.request);
    }
    std::size_t batch_index{};
    {
        std::lock_guard lock(mutex_);
        batch_index = ++batch_count_;
        largest_batch_ = std::max(largest_batch_, batch.size());
    }
    const auto batch_id = digest(semantic_canonical_json(JsonValue::Object{
        {"schema_version", "rozephine-resident-proposal-batch-v1"},
        {"model_instance_id", engine_->model_instance_id()},
        {"batch_index", static_cast<std::int64_t>(batch_index)},
        {"request_sha256", strings(request_digests)}}));
    std::vector<std::string> proposals;
    std::optional<std::string> error_type;
    std::optional<std::string> error;
    try {
        proposals = engine_->generate_batch(requests);
        if (proposals.size() != batch.size())
            throw std::invalid_argument(
                "resident engine output count did not match batch");
        for (auto& proposal : proposals) {
            proposal = trim(proposal);
            if (proposal.empty())
                throw std::invalid_argument(
                    "resident engine proposal must be non-empty text");
        }
    } catch (const std::exception& failure) {
        proposals.clear();
        error_type = typeid(failure).name();
        error = failure.what();
    } catch (...) {
        proposals.clear();
        error_type = "Exception";
        error = "unknown exception";
    }
    const auto completed_ns = clock_ns_();
    const auto elapsed_ns = completed_ns - started_ns;
    for (std::size_t index = 0; index < batch.size(); ++index) {
        auto& item = batch[index];
        ProposalTransportResult result(
            item.request.request_id, item.request.request_sha256,
            item.request.source_episode_id,
            item.request.full_current_pair_snapshot_id,
            error ? "failed_retained" : "completed_proposal", batch_id,
            batch.size(), started_ns - item.queued_at_ns, elapsed_ns,
            completed_ns, error ? std::nullopt
                                : std::optional<std::string>(proposals[index]),
            error_type, error);
        const bool succeeded = result.status == "completed_proposal";
        item.promise->set_value(std::move(result));
        std::lock_guard lock(mutex_);
        if (succeeded) ++completed_;
        else ++failed_;
    }
}

void ResidentMicrobatchCoordinator::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.empty() && !active_batches_; });
}

void ResidentMicrobatchCoordinator::close() {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        closed_ = true;
    }
    wait_idle();
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        available_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

JsonValue::Object ResidentMicrobatchCoordinator::receipt() const {
    std::lock_guard lock(mutex_);
    return {
        {"schema_version", "rozephine-resident-microbatch-coordinator-v1"},
        {"model_instance_id", engine_->model_instance_id()},
        {"model_load_count", static_cast<std::int64_t>(engine_->model_load_count())},
        {"maximum_queue_depth", static_cast<std::int64_t>(maximum_queue_depth_)},
        {"maximum_batch_size", static_cast<std::int64_t>(maximum_batch_size_)},
        {"maximum_batch_wait_ns", maximum_batch_wait_ns_},
        {"accepted_unique_request_count", static_cast<std::int64_t>(accepted_unique_)},
        {"coalesced_duplicate_submission_count",
         static_cast<std::int64_t>(coalesced_duplicates_)},
        {"conflicting_duplicate_rejection_count",
         static_cast<std::int64_t>(conflicting_duplicates_)},
        {"backpressure_rejection_count",
         static_cast<std::int64_t>(backpressure_rejections_)},
        {"completed_proposal_count", static_cast<std::int64_t>(completed_)},
        {"failed_retained_count", static_cast<std::int64_t>(failed_)},
        {"engine_batch_count", static_cast<std::int64_t>(batch_count_)},
        {"largest_observed_batch_size", static_cast<std::int64_t>(largest_batch_)},
        {"closed", closed_}, {"one_resident_engine", true},
        {"manager_or_worker_owns_persistent_cognition", false},
        {"diagnostic_rows_count_as_new_experience", 0},
        {"growth_claimed", false}, {"proposal_only", true},
        {"authority", proposal_authority_false()}};
}

}  // namespace swegca::world
