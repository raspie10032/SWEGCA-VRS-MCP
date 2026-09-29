#include "swegca_architecture/sha256.hpp"
#include "world/sensor_definition.hpp"
#include "world/sensor_term_index.hpp"
#include "world/unicode_nfkc.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace swegca::world;

namespace {

ContinuousSensorEvent event(
    const std::size_t index,
    std::string screen,
    std::string audio) {
    return {index, "00:00:0" + std::to_string(index),
            "sensor:" + std::to_string(index),
            "context:" + std::to_string(index), "테스트", 1.0,
            std::move(screen), std::move(audio)};
}

std::string hex(const swegca::architecture::Sha256::Bytes& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        const auto byte = std::to_integer<unsigned>(bytes[index]);
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 15U];
    }
    return result;
}

void append_utf8(std::string& output, const char32_t point) {
    if (point <= 0x7fU) output.push_back(static_cast<char>(point));
    else if (point <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    } else if (point <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    }
}

void update_u32_le(swegca::architecture::Sha256& sha, const std::uint32_t value) {
    const std::array<std::byte, 4> bytes{
        std::byte(value & 0xffU), std::byte((value >> 8U) & 0xffU),
        std::byte((value >> 16U) & 0xffU), std::byte((value >> 24U) & 0xffU)};
    sha.update(bytes);
}

void check_unicode_parity() {
    assert(unicode_nfkc_version() == "16.0.0");
    assert(normalize_nfkc("A\xcc\x8a") == "\xc3\x85");
    assert(normalize_nfkc("\xe1\x84\x80\xe1\x85\xa1") == "가");
    assert(normalize_nfkc("\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8") == "각");
    assert(normalize_nfkc("\xef\xac\x83") == "ffi");
    assert(normalize_nfkc("ＡＢＣ１２３") == "ABC123");

    // Python 3.14 / unicodedata 16.0.0 fixture over every valid scalar.
    swegca::architecture::Sha256 sha;
    for (std::uint32_t point = 0; point <= 0x10ffffU; ++point) {
        if (point >= 0xd800U && point <= 0xdfffU) continue;
        std::string source;
        append_utf8(source, point);
        const auto normalized = normalize_nfkc(source);
        update_u32_le(sha, point);
        update_u32_le(sha, static_cast<std::uint32_t>(normalized.size()));
        sha.update(normalized);
    }
    assert(hex(sha.finish()) ==
           "5650e8743caff1ed039e6e038c35b471260e724d078d675af780f09a86fd5c2d");
}

}  // namespace

int main() {
    check_unicode_parity();

    assert((extract_ocr_terms("ＧＡＭＥ１２３ abc-de 입니다 가나다입니다") ==
            std::vector<std::string>{"GAME123", "abc-de", "입니다", "가나다"}));
    assert((extract_ocr_terms("A\xcc\x8angstro\xcc\x88m") ==
            std::vector<std::string>{"ngstr"}));
    assert((extract_ocr_terms("가나다라마바사아자차카타파하가나다라마바사아자차카") ==
            std::vector<std::string>{
                "가나다라마바사아자차카타파하가나다라마바", "사아자차카"}));

    std::vector<ContinuousSensorEvent> events{
        event(0, "게임 시작", "첫 발화"),
        event(1, "게임 메뉴", "게임 발화"),
        event(2, "설정 화면", "게임 발화")};
    const auto first = update_sensor_term_index(nullptr, events);
    assert(first.reused_events == 0 && first.tokenized_events == 3 &&
           first.expired_events == 0);
    assert(first.index.index_hash ==
           "b015a63540ef7036450f3d914e08128dcfbb978df78b74a81fdad91562018af0");
    assert((first.index.screen_candidates == std::vector<OcrTermCandidate>{
        {"게임", {0, 1}}, {"메뉴", {1}}, {"설정", {2}},
        {"시작", {0}}, {"화면", {2}}}));
    assert((first.index.audio_candidates == std::vector<OcrTermCandidate>{
        {"발화", {0, 1, 2}}, {"게임", {1, 2}}}));

    const auto reused = update_sensor_term_index(&first.index, events);
    assert(reused.reused_events == 3 && reused.tokenized_events == 0 &&
           reused.expired_events == 0);
    assert(reused.index == first.index);

    const auto full_candidates = propose_sensor_definition_candidates(events);
    const auto indexed_candidates = propose_sensor_definition_candidates(
        events, 2, 8, &first.index);
    assert(full_candidates.size() == indexed_candidates.size());
    for (std::size_t index = 0; index != full_candidates.size(); ++index) {
        assert(full_candidates[index].candidate_id == indexed_candidates[index].candidate_id);
        assert(full_candidates[index].modality == indexed_candidates[index].modality);
        assert(full_candidates[index].term == indexed_candidates[index].term);
        assert(full_candidates[index].source_event_indices ==
               indexed_candidates[index].source_event_indices);
        assert(full_candidates[index].evidence_refs == indexed_candidates[index].evidence_refs);
    }

    const std::vector<ContinuousSensorEvent> initial{events[0], events[1]};
    const auto initial_index = update_sensor_term_index(nullptr, initial);
    const auto appended = append_sensor_term_index(initial_index.index, events[2]);
    assert(appended.index.indexed_events == first.index.indexed_events);
    assert(appended.index.screen_candidates == first.index.screen_candidates);
    assert(appended.index.audio_candidates == first.index.audio_candidates);
    assert(appended.index.index_hash == first.index.index_hash);
    assert(appended.reused_events == 2 && appended.tokenized_events == 1 &&
           appended.expired_events == 0);

    std::vector<ContinuousSensorEvent> sliding{
        events[1], events[2], event(3, "게임 도움말", "넷 발화")};
    const auto sliding_index = update_sensor_term_index(&first.index, sliding);
    assert(sliding_index.reused_events == 2);
    assert(sliding_index.tokenized_events == 1);
    assert(sliding_index.expired_events == 1);

    auto changed = events;
    changed[0].screen_ocr = "변경 화면";
    bool stale_rejected = false;
    try {
        validate_sensor_term_index(first.index, changed);
    } catch (const std::invalid_argument&) {
        stale_rejected = true;
    }
    assert(stale_rejected);

    bool duplicate_rejected = false;
    try {
        static_cast<void>(append_sensor_term_index(first.index, events[0]));
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    assert(duplicate_rejected);

    assert(sensor_term_index_source_sha256() ==
           "9f294603250cd6e635c79346eab86d0469fbdfd2728fdabfe4e903194f2638bf");
    std::cout << "sensor term index and Unicode NFKC tests passed\n";
}
