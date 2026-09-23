#pragma once

#include "hot_index.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace swegca::vrs {

// An unpublished successor view. Each row observes earlier rows in the same
// transaction, while the published main index remains unchanged until commit.
class HotIndexPending final : public HotIndexRead {
public:
    // SWEGCA: src/swegca_vrs2/store.py@7536139:342-348
    explicit HotIndexPending(const HotIndexRead& published);

    // SWEGCA: src/swegca_vrs2/store.py@7536139:342-348
    [[nodiscard]] std::pair<std::string, bool> append(const Json& row);
    // SWEGCA: src/swegca_vrs2/store.py@7536139:386-400
    [[nodiscard]] const std::vector<HotIndexAppendPlan>& plans() const {
        ensure_valid();
        return plans_;
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:146-148
    [[nodiscard]] bool contains_episode(std::string_view identifier) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:216-240
    [[nodiscard]] HotIndexEpisodeHeader episode_header(std::string_view identifier) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:216-217
    [[nodiscard]] std::vector<std::string> proposition_ids(std::string_view proposition) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
    [[nodiscard]] std::optional<std::string> successor_of(std::string_view identifier) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:172-174
    [[nodiscard]] std::string snapshot_id() const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:174-174
    [[nodiscard]] std::uint64_t outcome_count(std::string_view outcome) const override;

private:
    void ensure_valid() const;

    const HotIndexRead& published_;
    std::vector<HotIndexAppendPlan> plans_;
    std::unordered_map<std::string, std::size_t> pending_ids_;
    std::unordered_map<std::string, std::string> pending_successors_;
    std::unordered_map<std::string, std::vector<std::string>> pending_propositions_;
    std::unordered_map<std::string, std::uint64_t> pending_outcomes_;
    std::string snapshot_id_;
    bool failed_ = false;
};

}  // namespace swegca::vrs
