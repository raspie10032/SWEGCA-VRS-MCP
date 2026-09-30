#include "world/vrs_dialogue.hpp"

#include "world/vrs_memory_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace swegca::world {
namespace {

const std::vector<std::string> particles{
    "에서는", "에게서", "으로는", "이라는", "라고", "이라", "으로", "에서",
    "에게", "부터", "까지", "처럼", "보다", "은", "는", "이", "가", "을",
    "를", "와", "과", "의", "로", "에", "도", "만"};
const std::set<std::string, std::less<>> question_words{
    "사용자", "로제핀", "무엇", "뭐", "어떤", "어디", "어떻게", "왜", "영어",
    "일본어", "한국어", "은", "는", "이", "가", "을", "를", "와", "과", "의",
    "로", "에", "도", "만", "말해", "알려줘"};

bool continuation(const unsigned char byte) { return (byte & 0xc0U) == 0x80U; }

std::pair<char32_t, std::size_t> codepoint(const std::string_view text, const std::size_t at) {
    const auto first = static_cast<unsigned char>(text[at]);
    if (first < 0x80) return {first, 1};
    std::size_t size = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    if (at + size > text.size()) return {0xfffd, 1};
    char32_t value = first & (size == 2 ? 0x1fU : size == 3 ? 0x0fU : 0x07U);
    for (std::size_t i = 1; i < size; ++i) {
        const auto byte = static_cast<unsigned char>(text[at + i]);
        if (!continuation(byte)) return {0xfffd, 1};
        value = (value << 6U) | (byte & 0x3fU);
    }
    return {value, size};
}

bool word_codepoint(const char32_t value) {
    return (value < 128 && std::isalnum(static_cast<unsigned char>(value))) ||
        (value >= 0xac00 && value <= 0xd7a3) ||
        (value >= 0x3041 && value <= 0x30ff) ||
        (value >= 0x3400 && value <= 0x9fff) || value == 0x3005;
}

std::vector<std::string> tokens(const std::string_view query, const bool literal) {
    std::vector<std::string> result;
    std::string current;
    for (std::size_t at = 0; at < query.size();) {
        const auto [value, size] = codepoint(query, at);
        const bool separator_allowed = literal &&
            (value == '-' || value == '.' || value == '/' || value == ':');
        if (word_codepoint(value) || separator_allowed) {
            current.append(query.substr(at, size));
        } else if (!current.empty()) {
            result.push_back(std::move(current));
            current.clear();
        }
        at += size;
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

bool korean(const std::string_view value) {
    for (std::size_t at = 0; at < value.size();) {
        const auto [cp, size] = codepoint(value, at);
        if (cp < 0xac00 || cp > 0xd7a3) return false;
        at += size;
    }
    return !value.empty();
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (!value.empty() && std::ranges::find(values, value) == values.end())
        values.push_back(std::move(value));
}

std::optional<std::string> strip_particle(const std::string& value) {
    for (const auto& particle : particles)
        if (value.size() > particle.size() && value.ends_with(particle))
            return value.substr(0, value.size() - particle.size());
    return std::nullopt;
}

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

std::vector<std::string> string_array(const JsonValue& value) {
    std::vector<std::string> result;
    for (const auto& item : value.as_array()) result.emplace_back(item.as_string());
    return result;
}

std::string join(const std::vector<std::string>& values, const std::string_view delimiter) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += delimiter;
        result += value;
    }
    return result;
}

}  // namespace

std::vector<std::string> vrs_dialogue_query_keys(const std::string_view query) {
    std::vector<std::string> expanded;
    for (auto token : tokens(query, false)) {
        token = canonical_vrs_cue(token);
        expanded.push_back(token);
        if (korean(token)) if (auto stripped = strip_particle(token)) expanded.push_back(*stripped);
    }
    std::vector<std::string> result;
    append_unique(result, canonical_vrs_cue(query));
    const auto maximum = std::min<std::size_t>(4, expanded.size());
    for (std::size_t width = maximum; width; --width)
        for (std::size_t start = 0; start + width <= expanded.size(); ++start) {
            std::string phrase;
            for (std::size_t i = 0; i < width; ++i) {
                if (!phrase.empty()) phrase.push_back(' ');
                phrase += expanded[start + i];
            }
            append_unique(result, std::move(phrase));
        }
    for (auto token : tokens(query, true)) {
        token = canonical_vrs_cue(token);
        append_unique(result, token);
        if (auto stripped = strip_particle(token)) append_unique(result, *stripped);
    }
    return result;
}

VrsDialogueSession::VrsDialogueSession(
    std::vector<std::string> terms, std::vector<float> score,
    std::vector<std::uint64_t> support, std::vector<std::uint64_t> refute,
    std::vector<std::uint8_t> source_bits, std::vector<std::uint32_t> edge_source,
    std::vector<std::uint32_t> edge_target, std::vector<std::int8_t> edge_sign,
    std::vector<float> vrs_strength,
    std::map<std::string, JsonValue::Object, std::less<>> definitions,
    std::map<std::string, JsonValue::Object, std::less<>> trilingual,
    std::map<std::string, std::uint64_t, std::less<>> experience_stats)
    : score_(std::move(score)), support_(std::move(support)), refute_(std::move(refute)),
      source_bits_(std::move(source_bits)), experience_stats_(std::move(experience_stats)) {
    if (terms.empty() || score_.size() != terms.size() || support_.size() != terms.size() ||
        refute_.size() != terms.size() || source_bits_.size() != terms.size() ||
        edge_source.size() != edge_target.size() || edge_source.size() != edge_sign.size() ||
        edge_source.size() != vrs_strength.size())
        throw std::invalid_argument("VRS state length changed");
    std::set<std::string, std::less<>> original;
    for (auto& term : terms) {
        if (term.empty() || !original.insert(term).second)
            throw std::invalid_argument("VRS source terms must be nonempty and unique");
        terms_.push_back(canonical_vrs_cue(term));
    }
    for (std::size_t i = 0; i < terms_.size(); ++i) {
        const auto found = term_ids_.find(terms_[i]);
        if (found == term_ids_.end() || support_[i] + refute_[i] >
                support_[found->second] + refute_[found->second]) term_ids_[terms_[i]] = i;
    }
    for (auto& [key, value] : definitions)
        definitions_.emplace(canonical_vrs_cue(key), std::move(value));
    for (auto& [key, value] : trilingual)
        trilingual_.emplace(canonical_vrs_cue(key), std::move(value));
    std::vector<std::vector<std::tuple<std::uint32_t, std::int8_t, float>>> adjacent(terms_.size());
    for (std::size_t i = 0; i < edge_source.size(); ++i) {
        if (edge_source[i] >= terms_.size() || edge_target[i] >= terms_.size() ||
            (edge_sign[i] != -1 && edge_sign[i] != 1) ||
            !std::isfinite(vrs_strength[i]) || vrs_strength[i] < 0)
            throw std::invalid_argument("VRS edge values changed");
        adjacent[edge_source[i]].emplace_back(edge_target[i], edge_sign[i], vrs_strength[i]);
        adjacent[edge_target[i]].emplace_back(edge_source[i], edge_sign[i], vrs_strength[i]);
    }
    offsets_.resize(terms_.size() + 1);
    for (std::size_t owner = 0; owner < adjacent.size(); ++owner) {
        auto& rows = adjacent[owner];
        std::ranges::sort(rows, [](const auto& left, const auto& right) {
            return std::get<2>(left) > std::get<2>(right);
        });
        offsets_[owner] = neighbors_.size();
        for (const auto& [neighbor, sign, strength] : rows) {
            neighbors_.push_back(neighbor); signs_.push_back(sign); strengths_.push_back(strength);
        }
    }
    offsets_.back() = neighbors_.size();
}

const std::vector<VrsDialogueTurn>& VrsDialogueSession::history() const noexcept {
    return history_;
}

std::pair<std::vector<std::string>, std::vector<std::string>>
VrsDialogueSession::associations(const std::size_t term_id, const std::size_t limit) const {
    std::vector<std::string> positive, negative;
    for (std::size_t offset = offsets_.at(term_id); offset < offsets_.at(term_id + 1); ++offset) {
        auto& target = signs_[offset] > 0 ? positive : negative;
        append_unique(target, terms_[neighbors_[offset]]);
        if (positive.size() >= limit && negative.size() >= limit) break;
    }
    if (positive.size() > limit) positive.resize(limit);
    if (negative.size() > limit) negative.resize(limit);
    return {std::move(positive), std::move(negative)};
}

std::vector<std::string> VrsDialogueSession::matches(const std::string_view query) const {
    std::set<std::string, std::less<>> unique;
    for (const auto& key : vrs_dialogue_query_keys(query))
        if (!question_words.contains(key) && (term_ids_.contains(key) ||
            definitions_.contains(key) || trilingual_.contains(key))) unique.insert(key);
    std::vector<std::string> result(unique.begin(), unique.end());
    std::ranges::sort(result, [&](const auto& left, const auto& right) {
        const auto words = [](const std::string& value) {
            return static_cast<std::size_t>(std::ranges::count(value, ' ') + 1);
        };
        if (words(left) != words(right)) return words(left) > words(right);
        if (left.size() != right.size()) return left.size() > right.size();
        const auto ls = term_ids_.contains(left) ? support_[term_ids_.at(left)] : 0;
        const auto rs = term_ids_.contains(right) ? support_[term_ids_.at(right)] : 0;
        return ls != rs ? ls > rs : left < right;
    });
    return result;
}

std::vector<JsonValue::Object> VrsDialogueSession::term_evidence(
    const std::string_view term) const {
    std::vector<JsonValue::Object> result;
    const auto definition = definitions_.find(term);
    if (definition != definitions_.end()) result.push_back({
        {"kind", "experience_definition"},
        {"truth_status", find(definition->second, "truth_status") ? *find(definition->second, "truth_status") : JsonValue(nullptr)},
        {"hypothesis_kind", find(definition->second, "hypothesis_kind") ? *find(definition->second, "hypothesis_kind") : JsonValue(nullptr)},
        {"source_occurrences", find(definition->second, "source_occurrences") ? *find(definition->second, "source_occurrences") : JsonValue(nullptr)}});
    const auto translation = trilingual_.find(term);
    if (translation != trilingual_.end()) {
        const auto* senses = find(translation->second, "senses");
        result.push_back({{"kind", "trilingual_dictionary"},
            {"sense_count", static_cast<std::int64_t>(senses ? senses->as_array().size() : 0)}});
    }
    const auto term_id = term_ids_.find(term);
    if (term_id != term_ids_.end()) {
        auto [positive, negative] = associations(term_id->second);
        JsonValue::Array positives, negatives;
        for (const auto& value : positive) positives.emplace_back(value);
        for (const auto& value : negative) negatives.emplace_back(value);
        result.push_back({{"kind", "vrs"}, {"term_id", static_cast<std::int64_t>(term_id->second)},
            {"score", static_cast<double>(score_[term_id->second])},
            {"support", JsonInteger{std::to_string(support_[term_id->second])}},
            {"refute", JsonInteger{std::to_string(refute_[term_id->second])}},
            {"source_bits", static_cast<std::int64_t>(source_bits_[term_id->second])},
            {"positive_neighbors", std::move(positives)}, {"negative_neighbors", std::move(negatives)}});
    }
    return result;
}

std::string VrsDialogueSession::render_term(
    const std::string_view term, const std::vector<JsonValue::Object>& evidence) const {
    const auto translation = trilingual_.find(term);
    if (translation != trilingual_.end()) {
        const auto& senses = translation->second.at("senses").as_array();
        if (senses.size() != 1) return std::string(term) + ": " + std::to_string(senses.size()) +
            "개 의미가 있다. 어느 의미인지 더 말해 달라.";
        const auto& forms = senses.front().at("forms").as_object();
        return std::string(term) + ": 한국어 " + join(string_array(forms.at("kor")), ", ") +
            ", 영어 " + join(string_array(forms.at("eng")), ", ") + ", 일본어 " +
            join(string_array(forms.at("jpn")), ", ") + "로 연결된다.";
    }
    const auto definition = definitions_.find(term);
    if (definition != definitions_.end())
        return std::string(definition->second.at("definition_ko").as_string());
    const JsonValue::Object* vrs{};
    for (const auto& row : evidence)
        if (row.at("kind").as_string() == "vrs") { vrs = &row; break; }
    if (!vrs) return std::string(term) + ": 현재 hot experience에서 근거를 찾지 못했다. 추가 증거가 필요하다.";
    const auto positive = string_array(vrs->at("positive_neighbors"));
    const auto negative = string_array(vrs->at("negative_neighbors"));
    std::string utterance = "경험에서 '" + std::string(term) + "'은 " +
        (positive.empty() ? "추가 관찰 필요" : join(positive, ", ")) + "와 VRS로 연결된다.";
    if (!negative.empty()) utterance += " 반대·실패 연결은 " + join(negative, ", ") +
        "이며 재검증이 필요하다.";
    return utterance;
}

VrsDialogueTurn VrsDialogueSession::respond(std::string query_value) {
    if (query_value.empty()) throw std::invalid_argument("query must not be empty");
    VrsDialogueTurn turn;
    turn.query = query_value;
    const bool recent = query_value.find("방금") != std::string::npos &&
        (query_value.find("물었") != std::string::npos || query_value.find("질문") != std::string::npos ||
         query_value.find("말했") != std::string::npos);
    const bool summary = query_value.find("경험") != std::string::npos &&
        (query_value.find("무엇") != std::string::npos || query_value.find("뭐") != std::string::npos ||
         query_value.find("얼마") != std::string::npos || query_value.find("지금까지") != std::string::npos);
    if (recent) {
        if (history_.empty()) {
            turn.utterance = "이 세션에는 앞선 질문이 없다.";
            turn.status = "clarify_no_prior_turn";
        } else {
            turn.utterance = "방금 질문은 '" + history_.back().query + "'였다.";
            turn.status = "session_context";
        }
        turn.selection_method = "main_owned_bounded_session_history";
    } else if (summary) {
        const std::vector<std::pair<std::string, std::string>> ordered{{"이미지", "image_embedding_count"},
            {"위키 문서", "wiki_document_count"}, {"문학 구절", "literary_passage_count"},
            {"다국어 캡션", "xm3600_matched_caption_count"}, {"VRS 연결", "edge_count"}};
        std::vector<std::string> rows;
        for (const auto& [label, key] : ordered) if (experience_stats_.contains(key))
            rows.push_back(label + " " + std::to_string(experience_stats_.at(key)) + "개");
        turn.utterance = "현재 hot experience에는 " + join(rows, ", ") + "가 연결돼 있다.";
        turn.status = "grounded_experience_summary";
        turn.evidence.push_back({{"kind", "vrs_snapshot_summary"}});
        turn.selection_method = "runtime_query_intent_over_full_hot_snapshot";
    } else {
        turn.candidate_terms = matches(query_value);
        if (!turn.candidate_terms.empty()) {
            turn.selected_term = turn.candidate_terms.front();
            turn.rejected_terms.assign(turn.candidate_terms.begin() + 1, turn.candidate_terms.end());
            turn.evidence = term_evidence(*turn.selected_term);
            turn.utterance = render_term(*turn.selected_term, turn.evidence);
            turn.status = "grounded";
        } else {
            turn.utterance = "현재 hot experience에서 관련 근거를 찾지 못했다. 문맥이나 추가 증거가 필요하다.";
            turn.status = "clarify_not_found";
        }
        turn.selection_method = "runtime_longest_specific_match_then_vrs_evidence";
    }
    history_.push_back(turn);
    return turn;
}

}  // namespace swegca::world
