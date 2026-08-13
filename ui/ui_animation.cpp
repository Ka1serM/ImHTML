#include "ui_animation.h"

#include <cstring>
#include <cstdint>

#include "imgui.h"
#include "im_anim.h"
#include "litehtml/element.h"

namespace {
const ImGuiID kHover = ImHashStr("imhtmlui.element.hover");
const ImGuiID kPress = ImHashStr("imhtmlui.element.press");
const ImGuiID kClick = ImHashStr("imhtmlui.element.click");
const ImGuiID kReveal = ImHashStr("imhtmlui.element.reveal");

bool is_button(const litehtml::element& element) {
    const char* tag = element.get_tagName();
    return tag && (std::strcmp(tag, "button") == 0 || std::strcmp(tag, "a") == 0);
}

bool is_card(const litehtml::element& element) {
    const char* tag = element.get_tagName();
    return tag && std::strcmp(tag, "article") == 0;
}

float tween(ImGuiID id, ImGuiID channel, float target, float duration, float dt) {
    return iam_tween_float(id, channel, target, duration,
                           iam_ease_preset(iam_ease_out_cubic), iam_policy_crossfade, dt, 0.0f);
}
}  // namespace

void ImAnimUiAnimation::set_document(const std::string& document, int generation) {
    document_ = document;
    generation_ = generation;
}

void ImAnimUiAnimation::reset() {
    document_.clear();
    generation_++;
}

void ImAnimUiAnimation::begin_frame(float delta_seconds) {
    delta_seconds_ = delta_seconds > 0.0f ? delta_seconds : 1.0f / 60.0f;
}

ImHTML::ElementLayerTransform ImAnimUiAnimation::transform(
    const std::shared_ptr<litehtml::element>& element, const ImRect& bounds) {
    ImHTML::ElementLayerTransform output;
    if (!element) return output;

    const bool button = is_button(*element);
    const bool card = is_card(*element);
    // Most DOM nodes are static. Avoid constructing an animation identity and
    // querying ImAnim for every one of them on every frame.
    if (!button && !card) return output;

    const char* id = element->get_attr("id", "");
    ImGuiID seed = ImHashStr(document_.c_str(), 0, static_cast<ImGuiID>(generation_));
    const auto element_address = reinterpret_cast<std::uintptr_t>(element.get());
    const ImGuiID element_id = id && id[0]
                                    ? ImHashStr(id, 0, seed)
                                    : ImHashData(&element_address, sizeof(element_address), seed);
    const bool hovered = ImGui::IsMouseHoveringRect(bounds.Min, bounds.Max);

    if (button) {
        const float hover = tween(element_id, kHover,
                                  hovered ? 1.0f : 0.0f,
                                  0.30f, delta_seconds_);
        const float press = tween(element_id, kPress,
                                  hovered &&
                                      ImGui::IsMouseDown(ImGuiMouseButton_Left) ? 1.0f : 0.0f,
                                  0.18f, delta_seconds_);
        const float click = tween(element_id, kClick,
                                  
                                      ImGui::IsMouseClicked(ImGuiMouseButton_Left) ? 1.0f : 0.0f,
                                  0.34f, delta_seconds_);
        // Keep interaction feedback visual-only. Scaling a live litehtml
        // paint layer is unsafe when a switcher replaces its render tree:
        // successive tab clicks can otherwise compound the old transform.
        output.Scale = ImVec2(1.0f, 1.0f);
        output.Brightness = 1.0f + hover * 0.06f - press * 0.045f + click * 0.10f;
        return output;
    }

    if (card) {
        const float reveal = tween(element_id, kReveal, 1.0f, 0.48f, delta_seconds_);
        output.Opacity = reveal;
        output.Scale = ImVec2(1.0f, 1.0f);
    }
    return output;
}
