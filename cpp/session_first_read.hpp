#pragma once

#include "exact_journal_replay.hpp"
#include "native_cue_directory.hpp"
#include "portal_navigation.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace swegca::vrs {

// The owner pins every reference to one layer generation for the duration of
// the read. Session and main remain separate sources of original experience.
// SWEGCA: user@2026-09-22:13-21
struct PinnedReadLayer {
    const FullCurrentMemoryVrsSnapshot& pair;
    const EventVrsInputView& inputs;
    const GraphNodeDirectory& nodes;
    const GraphRegionDirectory& regions;
    const CoactivationAssociations& associations;
    const PortalPolicy& policy;
    const std::vector<PortalRevocation>& revocations;
    const NativeJournalReadView& journal;
    const ExactJournalDirectory& original_addresses;
    const NativeCueDirectory& cue_addresses;
    std::int64_t published_row_limit;
};

enum class ReadLayer { session, main };

// navigation.recall contains the complete address set from the selected layer.
// The caller records completion of this prefix before beginning Replay.
// SWEGCA: user@2026-09-22:20-25
struct SessionFirstRecall {
    ReadLayer layer;
    PortalNavigationRecall navigation;
};

// Déjà vu is the first memory-store operation. A session cue match retains
// session ownership even when later Recall yields no candidate; main is read
// only on the anonymous Déjà vu miss. The identical input cues key both layers.
// SWEGCA: user@2026-09-22:13-21
// SWEGCA: user@2026-09-22:46-53
[[nodiscard]] SessionFirstRecall recall_session_then_main(
    std::string user_input, const PinnedReadLayer& session,
    const PinnedReadLayer& main, std::int64_t observed_at_ns,
    std::uint64_t navigation_term_budget);

}  // namespace swegca::vrs
