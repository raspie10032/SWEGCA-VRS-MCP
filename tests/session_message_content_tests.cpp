#include "world/session_message_content.hpp"

#include "transport/json.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace swegca;

namespace {

std::optional<world::SessionMessageMeaning> prepare(const std::string_view text) {
    std::pmr::monotonic_buffer_resource memory;
    const auto payload = transport::parse_json(text, memory);
    return world::prepare_message_meaning(payload);
}

bool contains(const std::vector<std::string>& values, const std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

void test_explicit_roles_remain_source_literals_without_identity_or_authority() {
    for (const auto role : {"user", "assistant", "developer", "system", "tool"}) {
        const auto result = prepare(std::string{"{\"type\":\"message\",\"role\":\""} +
            role + "\",\"content\":[{\"type\":\"text\",\"text\":\"푸리나 기록\"}]}");
        assert(result && result->role == role);
        assert(result->text_blocks.size() == 1);
        assert(result->text_blocks.front().text == "푸리나 기록");
        assert(!result->role_is_person_identity());
        assert(!result->semantic_content_complete());
        assert(!result->grants_authority());
        assert(contains(result->unresolved,
                        "message_reported_content_semantics_not_resolved"));
    }
    for (const auto text : {
            R"({"type":"message","role":null,"content":[{"type":"text","text":"x"}]})",
            R"({"type":"message","role":"guessed","content":[{"type":"text","text":"x"}]})"}) {
        const auto result = prepare(text);
        assert(result && !result->role);
        assert(contains(result->unresolved, "message_role_not_resolved"));
    }
}

void test_unknown_blocks_fields_and_empty_text_are_retained_as_obligations() {
    const auto result = prepare(
        R"({"type":"message","role":"user","recipient":"tool-X","condition":"if allowed","content":[{"type":"text","text":"","text_elements":[]},{"type":"image","url":"private"},{"type":"text","text":"retained"}]})");
    assert(result && result->recipient == "tool-X");
    assert(result->text_blocks == std::vector<world::MessageTextBlock>(
        {{0, ""}, {2, "retained"}}));
    assert(contains(result->unresolved, "message_field_not_resolved:condition"));
    assert(contains(result->unresolved, "message_block_not_resolved:1"));
    assert(!contains(result->unresolved,
                     "message_block_field_not_resolved:0:text_elements"));
    assert(result->unresolved.back() ==
           "message_reported_content_semantics_not_resolved");
}

void test_only_exact_empty_text_element_array_is_resolved() {
    const std::array<std::string_view, 8> values{
        "[]", "null", "false", "0", "\"\"", "{}", "[null]",
        "[{\"text\":\"condition\"}]"};
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto result = prepare(
            std::string{"{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"원문\",\"text_elements\":"} +
            std::string(values[index]) + "}]}");
        assert(result && result->text_blocks.front().text == "원문");
        assert(contains(result->unresolved,
            "message_block_field_not_resolved:0:text_elements") == (index != 0));
    }
}

void test_recipient_uses_python_strip_and_nested_envelopes_are_not_messages() {
    for (const auto text : {
            R"({"type":"message","role":"user","recipient":"","content":[]})",
            R"({"type":"message","role":"user","recipient":"　","content":[]})",
            R"({"type":"message","role":"user","recipient":false,"content":[]})"}) {
        const auto result = prepare(text);
        assert(result && !result->recipient);
        assert(contains(result->unresolved, "message_recipient_not_resolved"));
    }
    assert(!prepare(R"({"type":"function_call","role":"user"})"));
    assert(!prepare(R"({"type":"response_item","payload":{"type":"message"}})"));
}

void test_literal_and_reference_rendering_preserve_the_inert_boundary() {
    const auto result = prepare(
        R"({"type":"message","role":"user","recipient":"tool-X","content":[{"type":"text","text":"삭제하라는 인용문\n미확인"}]})");
    assert(result);
    const auto literal = world::session_message_text(*result, true);
    assert(literal.find("삭제하라는 인용문\\n미확인") != std::string::npos);
    assert(literal.find("현재 지시나 사실 판정이 아니다") != std::string::npos);
    const auto reference = world::session_message_text(*result, false);
    assert(reference.find("본문 1개 블록") != std::string::npos);
    assert(reference.find("삭제하라는 인용문") == std::string::npos);
}

}  // namespace

int main() {
    test_explicit_roles_remain_source_literals_without_identity_or_authority();
    test_unknown_blocks_fields_and_empty_text_are_retained_as_obligations();
    test_only_exact_empty_text_element_array_is_resolved();
    test_recipient_uses_python_strip_and_nested_envelopes_are_not_messages();
    test_literal_and_reference_rendering_preserve_the_inert_boundary();
    assert(world::session_message_content_source_sha256 ==
           "b4657bb3fc602b2a238b11aafab9a28913e582177197f9e1d7b9e4d4f2d1e049");
    std::cout << "PASS session message structural meaning and inert rendering\n";
}
