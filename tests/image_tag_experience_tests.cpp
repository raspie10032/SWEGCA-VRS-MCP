#include "world/image_tag_experience.hpp"

#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using swegca::world::JsonValue;
using swegca::world::image_tag_experience;
using swegca::world::validate_image_tag_experience;

namespace {

JsonValue::Object content() {
    JsonValue::Array embedding;
    embedding.reserve(384);
    for (int index = 0; index != 384; ++index) embedding.emplace_back(0.125);
    return {
        {"source_sha256", "5feceb66ffc86f38d952786c6d696c79c2dbc239dd4e91b46729d73a27fb57e9"},
        {"source_address", "synthetic-image:0"},
        {"actual_decode_outcome", "image_decoded_and_exhaustively_tiled_embedded"},
        {"native_dimensions", JsonValue::Array{1000, 1000}},
        {"delivered_dimensions", JsonValue::Array{1000, 1000}},
        {"vision_model", JsonValue::Object{{"name", "synthetic-fixture"}}},
        {"tagger_model", JsonValue::Object{{"name", "synthetic-fixture"}}},
        {"views", JsonValue::Array{JsonValue::Object{
            {"embedding", std::move(embedding)},
            {"source_region_original_xywh", JsonValue::Array{0, 0, 1000, 1000}},
            {"model_tensor_dimensions", JsonValue::Array{518, 518}}}}},
        {"tag_threshold", 0.2614},
        {"tags", JsonValue::Array{JsonValue::Object{
            {"name", "blue_hair"}, {"category", 0}, {"score", 0.9}}}},
        {"tag_semantics_verified", false}, {"generation_prompt_used", false}};
}

const JsonValue::Object& observation(const JsonValue& episode) {
    return episode.at("step").at("observation").as_object();
}

bool has_cue(const JsonValue& episode, const std::string_view expected) {
    for (const auto& cue : episode.at("cues").as_array()) {
        if (cue.as_string() == expected) return true;
    }
    return false;
}

void rejects(const std::function<void(JsonValue::Object&)>& mutate) {
    auto value = content();
    mutate(value);
    bool rejected = false;
    try { (void)image_tag_experience(std::move(value), "one-user-folder", 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

void rejects_episode(const std::function<void(JsonValue::Object&)>& mutate) {
    auto episode = image_tag_experience(content(), "one-user-folder", 1);
    auto& root = const_cast<JsonValue::Object&>(episode.as_object());
    mutate(root);
    bool rejected = false;
    try { validate_image_tag_experience(episode); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

}  // namespace

int main() {
    assert(swegca::world::image_tag_experience_source_sha256 ==
           "068534e0163b5f4a17406999d9a0bef707e091a378328f3b41f00f45d9bb41ab");
    assert(swegca::world::media_observation_source_sha256 ==
           "0e40c6706c567255945c0ff32a8a7b7ca04662a0feb31337541d0e64688c380a");
    const auto first = image_tag_experience(content(), "one-user-folder", 1);
    const auto later = image_tag_experience(content(), "one-user-folder", 2);
    assert(first.at("episode_id").as_string() == later.at("episode_id").as_string());
    assert(first.at("revision").as_string() == later.at("revision").as_string());
    assert(first.at("revision").as_string() ==
           "ac0c4787efcc15ff5bf85486893ca18cddc9d5bf7342a52091d16f188d456598");
    assert(has_cue(first, "blue_hair") && has_cue(first, "blue") && has_cue(first, "hair"));
    assert(first.at("step").at("outcome").as_string() == "pending");
    assert(std::get<bool>(observation(first).at("semantics_verified").storage()) == false);
    assert(std::get<bool>(observation(first).at("growth_claimed").storage()) == false);
    validate_image_tag_experience(first);

    struct FloatRevision final { double value; const char* revision; };
    for (const auto probe : {
             FloatRevision{1e-7, "77bb630b6f0632a2a7e74c746da67d5873f4a9e521eede73960512d0291e2237"},
             FloatRevision{1e-4, "d349510c767d6192912c46b19ffa1619faf8a68822e029848d3075f23dc25346"},
             FloatRevision{1e15, "5de629e19dc650570b125f69a762dcdcdbfe5363d2e1da6063c2792f30058770"},
             FloatRevision{1e16, "dd5b9f42256b0d6c94abf0ee04d85ff378bb342887a49f5757591e6ca005dd54"},
             FloatRevision{-0.0, "b8b86c706644df9336005c5c159316889eea5898cda6ed86df7c28265817131b"},
             FloatRevision{1.2e100, "d2550ffa09d2165bf3ef855186e0dc01186894f44b7d21f1cf62975c93fffee0"},
         }) {
        auto value = content();
        value["float_probe"] = probe.value;
        assert(image_tag_experience(std::move(value), "one-user-folder", 1)
                   .at("revision").as_string() == probe.revision);
    }

    rejects([](auto& value) { value["views"] = JsonValue::Array{}; });
    rejects([](auto& value) {
        auto& views = const_cast<JsonValue::Array&>(value.at("views").as_array());
        auto& view = const_cast<JsonValue::Object&>(views[0].as_object());
        auto& embedding = const_cast<JsonValue::Array&>(view.at("embedding").as_array());
        embedding[0] = std::numeric_limits<double>::quiet_NaN();
    });
    rejects([](auto& value) {
        value["native_dimensions"] = JsonValue::Array{2000, 2000};
        value["delivered_dimensions"] = JsonValue::Array{96, 96};
    });
    rejects([](auto& value) { value["delivered_dimensions"] = JsonValue::Array{500, 1000}; });
    rejects([](auto& value) {
        auto& views = const_cast<JsonValue::Array&>(value.at("views").as_array());
        auto& view = const_cast<JsonValue::Object&>(views[0].as_object());
        view["source_region_original_xywh"] = JsonValue::Array{0, 0, 9999, 9999};
    });
    rejects([](auto& value) { value["vision_model"] = JsonValue::Object{}; });
    rejects([](auto& value) {
        auto& tags = const_cast<JsonValue::Array&>(value.at("tags").as_array());
        auto& tag = const_cast<JsonValue::Object&>(tags[0].as_object());
        tag["score"] = 1.1;
    });
    rejects([](auto& value) { value["tag_semantics_verified"] = true; });
    rejects([](auto& value) { value["generation_prompt_used"] = true; });
    rejects_episode([](auto& root) {
        auto& step = const_cast<JsonValue::Object&>(root.at("step").as_object());
        auto& observed = const_cast<JsonValue::Object&>(step.at("observation").as_object());
        observed["growth_claimed"] = true;
    });
    rejects_episode([](auto& root) {
        auto& step = const_cast<JsonValue::Object&>(root.at("step").as_object());
        auto& observed = const_cast<JsonValue::Object&>(step.at("observation").as_object());
        observed["observation_query"] = "steam-game:fake";
        auto& cues = const_cast<JsonValue::Array&>(root.at("cues").as_array());
        cues.emplace_back("steam-game:fake");
    });
    rejects_episode([](auto& root) {
        auto& step = const_cast<JsonValue::Object&>(root.at("step").as_object());
        auto& observed = const_cast<JsonValue::Object&>(step.at("observation").as_object());
        observed["generated_views_are_new_events"] = true;
    });

    auto audio = content();
    audio["concurrent_system_audio"] = JsonValue::Object{{"segment_count", 1}};
    const auto audio_row = image_tag_experience(std::move(audio), "one-user-folder", 1);
    assert(has_cue(audio_row, "media-kind:system_audio"));
    assert(has_cue(audio_row, "시청각"));

    auto generation = content();
    generation["generation_conditioning"] = JsonValue::Object{
        {"output_sha256", "5feceb66ffc86f38d952786c6d696c79c2dbc239dd4e91b46729d73a27fb57e9"},
        {"intent_is_observed_pixel_truth", false},
        {"conditioning_used_as_tag_ground_truth", false},
        {"manifest_sha256", "manifest-fixture"},
        {"conditioning", JsonValue::Object{
            {"prompt", "Blue Hair"}, {"negative_prompt", "low quality"},
            {"checkpoint", "Anima Model"}}}};
    const auto generation_row = image_tag_experience(std::move(generation), "one-user-folder", 1);
    assert(has_cue(generation_row, "generation-conditioning:manifest-fixture"));
    assert(has_cue(generation_row, "conditioning:blue"));
    assert(has_cue(generation_row, "conditioning:hair"));

    rejects([](auto& value) {
        value["generation_conditioning"] = JsonValue::Object{
            {"output_sha256", "6b86b273ff34fce19d6b804eff5a3f5747ada4eaa22f1d49c01e52ddb7875b4b"},
            {"intent_is_observed_pixel_truth", false},
            {"conditioning_used_as_tag_ground_truth", false},
            {"manifest_sha256", "manifest-fixture"},
            {"conditioning", JsonValue::Object{}}};
    });

    std::cout << "image tag experience tests passed\n";
}
