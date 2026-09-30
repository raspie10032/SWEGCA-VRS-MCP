#pragma once

#include "world/parallel_experience_transport.hpp"
#include "world/provider_transport.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view agent_interface_source_sha256 =
    "c7a266ca707e878572840b3fbf04a4cc544dc2b69e04698cf86750dc9d767a25";

class ProposalAdapter {
public:
    virtual ~ProposalAdapter() = default;
    [[nodiscard]] virtual std::string generate(
        const DetachedProposalRequest& request) = 0;
};

class ChatAdapter final : public ProposalAdapter {
public:
    ChatAdapter(ProviderProfile profile, ProviderPost post = post_provider_json);
    [[nodiscard]] std::string generate(
        const DetachedProposalRequest& request) override;
private:
    ProviderProfile profile_;
    ProviderPost post_;
};

using CodexRun = std::function<std::string(
    const std::vector<std::string>&, std::string_view, double)>;

[[nodiscard]] std::string run_codex_bounded(
    const std::vector<std::string>& command,
    std::string_view prompt, double timeout_seconds);

class CodexExecAdapter final : public ProposalAdapter {
public:
    CodexExecAdapter(ProviderProfile profile, CodexRun run = run_codex_bounded);
    [[nodiscard]] std::string generate(
        const DetachedProposalRequest& request) override;
private:
    ProviderProfile profile_;
    CodexRun run_;
};

using ProposalAdapterFactory =
    std::function<std::unique_ptr<ProposalAdapter>(const ProviderProfile&)>;

class ProviderRegistry final {
public:
    ProviderRegistry(
        std::vector<ProviderProfile> profiles,
        std::map<std::string, ProposalAdapterFactory, std::less<>> factories = {});
    [[nodiscard]] JsonValue::Array describe() const;
    [[nodiscard]] JsonValue::Object propose(
        std::string_view profile, const DetachedProposalRequest& request) const;
private:
    std::map<std::string, ProviderProfile, std::less<>> profiles_;
    std::map<std::string, ProposalAdapterFactory, std::less<>> factories_;
};

class ProfiledProposalEngine final : public ResidentProposalEngine {
public:
    ProfiledProposalEngine(
        std::shared_ptr<const ProviderRegistry> registry, std::string profile);
    [[nodiscard]] std::string model_instance_id() const override;
    [[nodiscard]] std::size_t model_load_count() const override;
    [[nodiscard]] std::vector<std::string> generate_batch(
        const std::vector<DetachedProposalRequest>& requests) override;
private:
    std::shared_ptr<const ProviderRegistry> registry_;
    std::string profile_;
};

class ResidentClient final {
public:
    using Observer = std::function<void(const JsonValue::Object&)>;

    ResidentClient(std::string socket_path, double timeout_seconds = 5.0);
    [[nodiscard]] JsonValue::Object request(
        std::string_view command, JsonValue::Object arguments = {}) const;
    [[nodiscard]] JsonValue::Object scheduled_dialogue(
        JsonValue::Object arguments, Observer trace_sink = {},
        Observer on_part = {}) const;
    [[nodiscard]] JsonValue::Array collect_model_trace(
        std::string request_id, std::string view_id, std::size_t count,
        Observer trace_sink = {}) const;
private:
    std::string socket_path_;
    double timeout_seconds_{};
};

class AgentFacade final {
public:
    using Observer = ResidentClient::Observer;

    AgentFacade(
        std::shared_ptr<ResidentClient> resident,
        std::shared_ptr<ProviderRegistry> providers);
    [[nodiscard]] JsonValue::Object status() const;
    [[nodiscard]] JsonValue::Object respond(
        std::string query, std::string profile, std::string request_id,
        std::string expected_pair_snapshot_id,
        std::string dialogue_mode = "memory_report", Observer on_part = {}) const;
    [[nodiscard]] JsonValue::Object lookup(std::string cue) const;
    [[nodiscard]] JsonValue::Object propose(
        std::string profile, std::string request_id,
        std::string source_episode_id, std::string source_revision,
        std::string expected_pair_snapshot_id, std::string prompt,
        JsonValue::Object current_evidence) const;

    const std::shared_ptr<ResidentClient> resident;
    const std::shared_ptr<ProviderRegistry> providers;
};

[[nodiscard]] AgentFacade load_facade(const std::filesystem::path& path);

}  // namespace swegca::world
