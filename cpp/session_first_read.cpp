#include "session_first_read.hpp"

#include "keys.hpp"

#include <utility>

namespace swegca::vrs {

// SWEGCA: user@2026-09-22:13-21
// SWEGCA: user@2026-09-22:46-53
SessionFirstRecall recall_session_then_main(
    std::string user_input, const PinnedReadLayer& session,
    const PinnedReadLayer& main, std::int64_t observed_at_ns,
    std::uint64_t navigation_term_budget) {
    const auto cues = lexical_keys(user_input);
    auto session_signal = detect_deja_vu(session.pair.memory(), user_input, cues);
    if (!session_signal.matched_cues.empty()) {
        auto navigation = recall_after_deja_vu_navigation(
            session.pair, session_signal, session.inputs, session.nodes,
            session.regions, session.associations, session.policy,
            observed_at_ns, navigation_term_budget, session.revocations);
        return SessionFirstRecall{ReadLayer::session, std::move(navigation)};
    }
    auto main_signal = detect_deja_vu(main.pair.memory(), std::move(user_input), cues);
    auto navigation = recall_after_deja_vu_navigation(
        main.pair, main_signal, main.inputs, main.nodes,
        main.regions, main.associations, main.policy,
        observed_at_ns, navigation_term_budget, main.revocations);
    return SessionFirstRecall{ReadLayer::main, std::move(navigation)};
}

}  // namespace swegca::vrs
