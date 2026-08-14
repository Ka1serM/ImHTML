#include "imhtml_app.h"

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>

#include <SDL3/SDL.h>
#include "litehtml/document.h"
#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace ImHTML {

std::string Event::attribute(const std::string& name) const {
    if (!target_element_ || name.empty()) return {};
    return target_element_->get_attr(name.c_str(), "");
}

std::string Event::closest(const std::string& selector) const {
    if (application_ == nullptr || selector.empty()) return {};
    const auto matches = application_->query_selector_all(selector);
    for (auto current = target_element_; current; current = current->parent()) {
        if (std::ranges::find(matches, current) != matches.end()) {
            return current->get_attr("id", "");
        }
    }
    return {};
}

namespace {
bool telemetry_enabled() {
    static const bool enabled = std::getenv("IMHTML_TELEMETRY") != nullptr;
    return enabled;
}

bool timing_enabled() {
    static const bool enabled = std::getenv("IMHTML_TIMINGS") != nullptr;
    return enabled;
}

std::string TrimURL(const std::string& url) {
    const auto is_space = [](unsigned char value) { return std::isspace(value) != 0; };
    std::size_t first = 0;
    while (first < url.size() && is_space(static_cast<unsigned char>(url[first]))) {
        ++first;
    }
    std::size_t last = url.size();
    while (last > first && is_space(static_cast<unsigned char>(url[last - 1]))) {
        --last;
    }
    return url.substr(first, last - first);
}

bool IsExternalURL(const std::string& url) {
    const std::size_t scheme_end = url.find(':');
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return url.rfind("//", 0) == 0;
    }
    if (!std::isalpha(static_cast<unsigned char>(url.front()))) {
        return false;
    }
    for (std::size_t index = 1; index < scheme_end; ++index) {
        const unsigned char character = static_cast<unsigned char>(url[index]);
        if (!std::isalnum(character) && character != '+' && character != '-' && character != '.') {
            return false;
        }
    }
    std::string scheme = url.substr(0, scheme_end);
    for (char& character : scheme) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return scheme != "javascript" && scheme != "data" && scheme != "about";
}

bool OpenExternalURL(const std::string& raw_url) {
    const std::string url = TrimURL(raw_url);
    if (!IsExternalURL(url)) {
        return false;
    }
    const std::string browser_url = url.rfind("//", 0) == 0 ? "https:" + url : url;
    if (SDL_OpenURL(browser_url.c_str())) {
        return true;
    }
    std::fprintf(stderr, "[ImHTML] failed to open external URL: %s\n", browser_url.c_str());
    return false;
}

double ParseRangeNumber(const char* text, double fallback) {
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    return end != text && end != nullptr && *end == '\0' && std::isfinite(value) ? value : fallback;
}

bool SetTextNode(const std::shared_ptr<litehtml::element>& target, const std::string& text) {
    if (!target) return false;
    for (const auto& child : target->children()) {
        if (child && child->is_text()) {
            child->set_data(text.c_str());
            return true;
        }
    }
    return false;
}

double RangeStep(const std::shared_ptr<litehtml::element>& range) {
    const char* step = range ? range->get_attr("step", "any") : "any";
    if (step == nullptr || std::strcmp(step, "any") == 0) return 0.0;
    const double parsed = ParseRangeNumber(step, 0.0);
    return parsed > 0.0 ? parsed : 0.0;
}

double QuantizeRangeValue(double value, double minimum, double maximum, double step) {
    value = std::clamp(value, minimum, maximum);
    if (step <= 0.0) return value;
    const double snapped = minimum + std::round((value - minimum) / step) * step;
    return std::clamp(snapped, minimum, maximum);
}

float RangeThumbWidth(float height) {
    return std::max(1.0f, height * 1.35f);
}

std::string SerializeRangeValue(double value) {
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

void InsertStylesheet(std::string& html, const std::string& stylesheet) {
    if (stylesheet.empty()) return;
    const std::string style_element = "<style>" + stylesheet + "</style>";
    const std::size_t head_end = html.find("</head>");
    if (head_end != std::string::npos) {
        html.insert(head_end, style_element);
        return;
    }
    const std::size_t doctype_end = html.find('>');
    html.insert(doctype_end == std::string::npos ? 0 : doctype_end + 1, style_element);
}

std::string FormatRangeValue(double value) {
    const double normalized = std::abs(value) < 0.0005 ? 0.0 : value;
    std::ostringstream stream;
    if (std::abs(normalized - std::round(normalized)) < 0.0005) {
        stream << std::fixed << std::setprecision(0) << normalized;
    } else {
        stream << std::fixed << std::setprecision(std::abs(normalized) < 1.0 ? 3 : 2) << normalized;
    }
    return stream.str();
}

std::string EscapeHtml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped += character; break;
        }
    }
    return escaped;
}

bool IsRangeInput(const std::shared_ptr<litehtml::element>& element) {
    return element && std::strcmp(element->get_tagName(), "input") == 0 &&
           std::strcmp(element->get_attr("type", ""), "range") == 0;
}

std::size_t PreviousUtf8(const std::string& text, std::size_t offset) {
    if (offset == 0) return 0;
    --offset;
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) --offset;
    return offset;
}

std::size_t NextUtf8(const std::string& text, std::size_t offset) {
    if (offset >= text.size()) return text.size();
    ++offset;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) ++offset;
    return offset;
}

void AppendUtf8(std::string& target, unsigned int codepoint) {
    if (codepoint <= 0x7f) {
        target.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        target.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        target.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        target.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        target.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string Utf8Prefix(const std::string& text, std::size_t offset) {
    return text.substr(0, std::min(offset, text.size()));
}

float TextWidth(ImFont* font, float font_size, std::string_view text) {
    if (!font || text.empty()) return 0.0f;
    const std::string copy(text);
    return font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, copy.c_str()).x;
}

litehtml::web_color CssBackgroundColor(const std::shared_ptr<litehtml::element>& element) {
    return element != nullptr ? element->css().get_bg().m_color : litehtml::web_color::transparent;
}

litehtml::web_color CssBorderColor(const std::shared_ptr<litehtml::element>& element) {
    if (element == nullptr) return litehtml::web_color::transparent;
    const auto& borders = element->css().get_borders();
    if (borders.top.color.alpha != 0) return borders.top.color;
    if (borders.right.color.alpha != 0) return borders.right.color;
    if (borders.bottom.color.alpha != 0) return borders.bottom.color;
    if (borders.left.color.alpha != 0) return borders.left.color;
    return element->css().get_color();
}

litehtml::web_color CssAccentColor(const std::shared_ptr<litehtml::element>& element) {
    if (element == nullptr) return litehtml::web_color::transparent;
    const auto& accent = element->css().get_accent_color();
    return accent.auto_value ? element->css().get_color() : accent.color;
}

ImU32 ToImColor(const litehtml::web_color& color) {
    return IM_COL32(color.red, color.green, color.blue, color.alpha);
}

std::shared_ptr<litehtml::element> RangeAncestor(std::shared_ptr<litehtml::element> element) {
    for (; element; element = element->parent()) {
        if (IsRangeInput(element)) return element;
    }
    return nullptr;
}

bool ElementBox(const std::shared_ptr<litehtml::element>& element, litehtml::position& result) {
    if (!element || !element->get_render_item()) return false;
    bool found = false;
    element->get_render_item()->get_rendering_boxes([&](const litehtml::position& box) {
        if (found || box.width <= litehtml::pixel_t(0) || box.height <= litehtml::pixel_t(0)) return;
        result = box;
        found = true;
    });
    return found;
}

bool RangeBox(const std::shared_ptr<litehtml::element>& range, litehtml::position& result) {
    return ElementBox(range, result);
}

bool IsScrollAncestor(const std::shared_ptr<litehtml::element>& element,
                      const std::shared_ptr<litehtml::element>& candidate) {
    for (auto current = element; current; current = current->parent()) {
        if (current.get() == candidate.get()) return true;
    }
    return false;
}

class NativeControlClipScope {
public:
    NativeControlClipScope(const std::vector<ScrollState>& scroll_states,
                           const std::shared_ptr<litehtml::element>& element,
                           const ImVec2& document_origin, ImDrawList* draw_list)
        : draw_list_(draw_list) {
        if (!element || !draw_list_) return;

        for (const auto& state : scroll_states) {
            if (!state.target || !IsScrollAncestor(element, state.target)) continue;

            ImVec2 clip_min(document_origin.x + static_cast<float>(state.viewport_box.x),
                            document_origin.y + static_cast<float>(state.viewport_box.y));
            ImVec2 clip_max(clip_min.x + static_cast<float>(state.viewport_box.width),
                            clip_min.y + static_cast<float>(state.viewport_box.height));
            if (state.has_clip) {
                const ImVec2 ancestor_min(document_origin.x + static_cast<float>(state.clip_box.x),
                                          document_origin.y + static_cast<float>(state.clip_box.y));
                const ImVec2 ancestor_max(ancestor_min.x + static_cast<float>(state.clip_box.width),
                                          ancestor_min.y + static_cast<float>(state.clip_box.height));
                clip_min.x = std::max(clip_min.x, ancestor_min.x);
                clip_min.y = std::max(clip_min.y, ancestor_min.y);
                clip_max.x = std::min(clip_max.x, ancestor_max.x);
                clip_max.y = std::min(clip_max.y, ancestor_max.y);
            }
            clip_max.x = std::max(clip_max.x, clip_min.x);
            clip_max.y = std::max(clip_max.y, clip_min.y);
            draw_list_->PushClipRect(clip_min, clip_max, true);
            ++pushed_clips_;
        }
    }

    NativeControlClipScope(const NativeControlClipScope&) = delete;
    NativeControlClipScope& operator=(const NativeControlClipScope&) = delete;

    ~NativeControlClipScope() {
        if (!draw_list_) return;
        for (int index = 0; index < pushed_clips_; ++index) draw_list_->PopClipRect();
    }

private:
    ImDrawList* draw_list_ = nullptr;
    int pushed_clips_ = 0;
};

std::vector<std::string> SelectOptionValues(const std::shared_ptr<litehtml::element>& select) {
    std::vector<std::string> values;
    if (!select) return values;
    for (const auto& child : select->children()) {
        if (!child || std::strcmp(child->get_tagName(), "option") != 0) continue;
        const char* value = child->get_attr("value", nullptr);
        if (value != nullptr) {
            values.emplace_back(value);
        } else {
            std::string text;
            child->get_text(text);
            values.push_back(std::move(text));
        }
    }
    return values;
}

struct SelectPopupGeometry {
    ImVec2 min;
    ImVec2 max;
    float row_height = 0.0f;
    float padding = 0.0f;
    std::size_t visible_count = 0;
};

struct SelectScrollbarGeometry {
    ImVec2 track_min;
    ImVec2 track_max;
    ImVec2 thumb_min;
    ImVec2 thumb_max;
};

bool GetSelectPopupGeometry(const std::shared_ptr<litehtml::element>& select,
                            const ImVec2& document_origin, std::size_t option_count,
                            SelectPopupGeometry& result) {
    if (!select || option_count == 0) return false;
    litehtml::position box;
    if (!ElementBox(select, box)) return false;

    const ImVec2 control_min(document_origin.x + static_cast<float>(box.x),
                             document_origin.y + static_cast<float>(box.y));
    const ImVec2 control_max(control_min.x + static_cast<float>(box.width),
                             control_min.y + static_cast<float>(box.height));
    const float font_size = static_cast<float>(select->css().get_font_metrics().font_size);
    result.row_height = std::max(static_cast<float>(box.height), font_size * 1.5f);
    result.padding = std::max(4.0f, font_size * 0.5f);
    const float max_height = std::max(result.row_height, static_cast<float>(box.height) * 8.0f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 work_min = viewport->WorkPos;
    const ImVec2 work_max(work_min.x + viewport->WorkSize.x, work_min.y + viewport->WorkSize.y);
    const float gap = std::max(1.0f, font_size * 0.25f);
    const float below = std::max(0.0f, work_max.y - control_max.y - gap);
    const float above = std::max(0.0f, control_min.y - work_min.y - gap);
    const float available = std::max(result.row_height, std::max(above, below));
    const bool open_above = below < std::min(max_height, result.padding * 2.0f +
                                             result.row_height * static_cast<float>(option_count)) && above > below;
    const float height = std::min(max_height, available);
    result.visible_count = std::max<std::size_t>(1, static_cast<std::size_t>(
        std::max(1.0f, std::floor((height - result.padding * 2.0f) / result.row_height))));
    result.visible_count = std::min(option_count, result.visible_count);
    const float actual_height = result.padding * 2.0f + result.row_height * static_cast<float>(result.visible_count);
    const float width = control_max.x - control_min.x;
    const float x = ImClamp(control_min.x, work_min.x, work_max.x - width);
    const float y = open_above ? control_min.y - gap - actual_height : control_max.y + gap;
    result.min = ImVec2(x, y);
    result.max = ImVec2(x + width, y + actual_height);
    return true;
}

bool GetSelectScrollbarGeometry(const std::shared_ptr<litehtml::element>& select,
                                const SelectPopupGeometry& popup, std::size_t option_count,
                                float scroll, SelectScrollbarGeometry& result) {
    if (option_count <= popup.visible_count || popup.visible_count == 0) return false;

    const auto width = select->css().get_scrollbar_width();
    const float thickness = ScrollbarSizePixels(width);
    if (thickness <= 0.0f) return false;
    const float inset = 0.0f;
    const float track_top = popup.min.y + popup.padding;
    const float track_bottom = popup.max.y - popup.padding;
    const float track_height = track_bottom - track_top;
    if (track_height <= 0.0f) return false;

    const float thumb_height = std::min(track_height,
                                        track_height * static_cast<float>(popup.visible_count) /
                                            static_cast<float>(option_count));
    const float travel = std::max(0.0f, track_height - thumb_height);
    const float maximum_start = static_cast<float>(option_count - popup.visible_count);
    const float scroll_ratio = maximum_start > 0.0f ? ImClamp(scroll / maximum_start, 0.0f, 1.0f) : 0.0f;
    const float track_left = popup.max.x - inset - thickness;
    result.track_min = ImVec2(track_left, track_top);
    result.track_max = ImVec2(track_left + thickness, track_bottom);
    result.thumb_min = ImVec2(track_left, track_top + travel * scroll_ratio);
    result.thumb_max = ImVec2(track_left + thickness, result.thumb_min.y + thumb_height);
    return true;
}

}  // namespace

void HtmlDocument::set_stylesheet_provider(std::function<std::string()> provider) { stylesheet_provider_ = std::move(provider); }

void HtmlDocument::register_shell(std::string html) { shell_html_ = std::move(html); }

void HtmlDocument::register_fragment(const std::string& name, FragmentHtmlProvider html_provider) {
    fragments_[name] = std::move(html_provider);
}

void HtmlDocument::register_html_control(std::unique_ptr<HtmlControl> control) {
    if (control) html_controls_.push_back(std::move(control));
}

HtmlDocument::EventListenerId HtmlDocument::on(const std::string& event,
                                                      const std::string& selector,
                                                      EventListener listener) {
    if (event.empty() || selector.empty() || !listener) return 0;
    const EventListenerId id = next_event_listener_id_++;
    event_listeners_.push_back({id, event, selector, std::move(listener)});
    return id;
}

void HtmlDocument::off(const EventListenerId listener_id) {
    if (listener_id == 0) return;
    std::erase_if(event_listeners_, [listener_id](const EventListenerRegistration& registration) {
        return registration.id == listener_id;
    });
}

bool HtmlDocument::initialize(HtmlWindow* application) {
    application_ = application;
    return !shell_html_.empty();
}

std::vector<std::shared_ptr<litehtml::element>> HtmlDocument::query_selector_all(
    const std::string& selector) const {
    if (!doc_ || !doc_->root() || selector.empty()) return {};
    const auto elements = doc_->root()->select_all(selector.c_str());
    return {elements.begin(), elements.end()};
}

std::shared_ptr<litehtml::element> HtmlDocument::query_selector(const std::string& selector) const {
    if (!doc_ || !doc_->root() || selector.empty()) return {};
    return doc_->root()->select_one(selector.c_str());
}

void HtmlDocument::append_html(const std::string& selector, const std::string& html,
                                  const bool replace_existing) {
    if (!doc_ || html.empty()) return;
    const auto target = doc_->root()->select_one(selector.c_str());
    if (!target) return;
    doc_->append_children_from_string(*target, html.c_str(), replace_existing);
    element_cache_.clear();
    id_cache_.clear();
    control_cache_valid_ = false;
    doc_->refresh_styles();
    MarkDocumentLayoutDirty("app");
}

bool HtmlDocument::has_value(const std::string& id) const {
    return !id.empty() && values_.find(id[0] == '#' ? id.substr(1) : id) != values_.end();
}

void HtmlDocument::set_value(const std::string& id, const std::string& value) {
    if (id.empty()) return;
    const std::string key = id[0] == '#' ? id.substr(1) : id;
    values_[key] = value;
    if (const auto element = FindElement("#" + key)) {
        if (element->get_tagName() != nullptr && std::strcmp(element->get_tagName(), "input") == 0 &&
            std::string(element->get_attr("type", "")) == "text") {
            auto& state = text_states_[key];
            if (state.value != value) {
                state.value = value;
                state.cursor = state.anchor = value.size();
                state.scroll_x = 0.0f;
            }
        }
        element->set_attr("data-value", value.c_str());
    }
}

void HtmlDocument::set_checked(const std::string& id, const bool checked_value) {
    if (id.empty()) return;
    const std::string key = id[0] == '#' ? id.substr(1) : id;
    values_[key] = checked_value ? "true" : "false";
    if (const auto element = FindElement("#" + key)) {
        const char* state = checked_value ? "true" : "false";
        element->set_attr("data-checked", state);
        element->set_attr("aria-checked", state);
    }
}

void HtmlDocument::set_options(const std::string& id, std::vector<std::string> options) {
    if (!id.empty()) options_[id[0] == '#' ? id.substr(1) : id] = std::move(options);
}

const std::vector<std::string>& HtmlDocument::options(const std::string& id) const {
    const std::string key = id.empty() ? id : (id[0] == '#' ? id.substr(1) : id);
    const auto found = options_.find(key);
    static const std::vector<std::string> empty;
    return found == options_.end() ? empty : found->second;
}

std::string HtmlDocument::value(const std::string& selector) const {
    const auto element = query_selector(selector);
    if (element) {
        const char* current = element->get_attr("data-value", nullptr);
        if (current == nullptr) current = element->get_attr("data-range-value", nullptr);
        if (current == nullptr) current = element->get_attr("value", "");
        if (current != nullptr) return current;
    }
    const std::string key = selector.empty() ? selector : (selector[0] == '#' ? selector.substr(1) : selector);
    const auto stored = values_.find(key);
    return stored == values_.end() ? std::string() : stored->second;
}

double HtmlDocument::value_as_number(const std::string& selector, const double fallback) const {
    return ParseRangeNumber(value(selector).c_str(), fallback);
}

bool HtmlDocument::checked(const std::string& selector) const {
    const auto element = query_selector(selector);
    if (element) {
        const char* state = element->get_attr("data-checked", nullptr);
        if (state == nullptr) state = element->get_attr("checked", nullptr);
        if (state != nullptr) return std::string(state) != "false";
    }
    return value(selector) == "true";
}

std::string HtmlDocument::state_value(const std::string& key, const std::string& fallback) const {
    const std::string normalized = !key.empty() && key.front() == '#' ? key.substr(1) : key;
    const auto found = values_.find(normalized);
    return found == values_.end() ? fallback : found->second;
}

void HtmlDocument::set_state(const std::string& key, const std::string& value) {
    set_value(key, value);
}

bool HtmlDocument::dispatch_event(const std::string& id, const std::string& event,
                                     const std::string& key) {
    if (id.empty() || event.empty() || !doc_ || !doc_->root()) return false;
    const auto target = doc_->root()->select_one("#" + id);
    if (!target) return false;

    Event dom_event;
    dom_event.application_ = this;
    dom_event.target_element_ = target;
    dom_event.type_ = event;
    dom_event.target_id_ = id;
    dom_event.value_ = value("#" + id);
    dom_event.checked_ = checked("#" + id);
    dom_event.key_ = key;

    const auto listeners = event_listeners_;
    bool dispatched = false;
    for (auto current = target; current && !dom_event.propagation_stopped(); current = current->parent()) {
        for (const auto& registration : listeners) {
            if (registration.event != event || !registration.listener) continue;
            const auto matched = query_selector_all(registration.selector);
            if (std::ranges::find(matched, current) == matched.end()) continue;
            dom_event.current_target_id_ = current->get_attr("id", "");
            registration.listener(dom_event);
            dispatched = true;
            if (dom_event.propagation_stopped()) break;
        }
    }
    return dispatched;
}

bool HtmlDocument::DispatchHtmlControlEvent(const std::string& id, const std::string& event) {
    if (!application_) return false;
    for (const auto& control : html_controls_) {
        if (control && control->on_event(*this, *application_, id, event)) return true;
    }
    return false;
}

void HtmlDocument::NotifyHtmlControlTextFocus(const std::string& id) {
    if (!application_) return;
    for (const auto& control : html_controls_) {
        if (control) control->on_text_focus(*this, *application_, id);
    }
}

bool HtmlDocument::HandleHtmlControlTextKey(const std::string& id) {
    if (!application_) return false;
    for (const auto& control : html_controls_) {
        if (control && control->on_text_key(*this, *application_, id)) return true;
    }
    return false;
}

bool HtmlDocument::NotifyHtmlControlTextChanged(const std::string& id, const std::string& value) {
    if (!application_) return false;
    bool handled = false;
    for (const auto& control : html_controls_) {
        if (control) handled = control->on_text_changed(*this, *application_, id, value) || handled;
    }
    return handled;
}

void HtmlDocument::RequestStructuralRebuild() { needs_rebuild_ = true; }

void HtmlDocument::MarkElementLayoutDirty(const std::shared_ptr<litehtml::element>& target) {
    if (!target) return;
    target->mark_layout_dirty(true);
    for (auto parent = target->parent(); parent; parent = parent->parent()) {
        parent->mark_layout_dirty(false);
    }
    MarkDocumentLayoutDirty("app");
}

void HtmlDocument::mark_dirty() { RequestStructuralRebuild(); }

void HtmlDocument::set_list(std::string id, std::vector<inja::json> items) {
    if (id.empty()) return;
    const auto found = lists_.find(id);
    if (found != lists_.end() && found->second == items) return;

    if (found != lists_.end() && ApplyListSelectionPaint(id, found->second, items)) {
        lists_[id] = std::move(items);
        ++list_update_count_;
        ++paint_only_list_update_count_;
        return;
    }

    lists_[id] = std::move(items);
    ++list_update_count_;

    if (!doc_ || !doc_->root()) {
        RequestStructuralRebuild();
        return;
    }
    const auto list_element = doc_->root()->select_one("#" + id);
    if (!list_element || !IsElementMounted(list_element)) {
        ++inactive_list_update_count_;
        return;
    }
    pending_list_updates_.insert(id);
}

namespace {

constexpr const char* kSpacerClass = "imhtml-virtual-spacer";
constexpr std::size_t kProbeRows = 48;
constexpr std::size_t kOverscanRows = 4;

std::string SpacerHtml(const float height) {
    if (height <= 0.5f) return {};
    return std::string("<div class=\"") + kSpacerClass + "\" style=\"display:block;height:" +
           std::to_string(static_cast<int>(height + 0.5f)) + "px;\"></div>";
}

bool IsSpacer(const std::shared_ptr<litehtml::element>& element) {
    if (!element) return false;
    const char* classes = element->get_attr("class", "");
    return classes != nullptr && std::strstr(classes, kSpacerClass) != nullptr;
}

}  // namespace

std::string HtmlDocument::ExpandListRows(const std::string& id,
                                        const std::string_view item_template) const {
    const auto list = lists_.find(id);
    if (list == lists_.end() || list->second.empty()) {
        list_windows_[id] = ListWindow{};
        return {};
    }

    const std::vector<inja::json>& items = list->second;
    ListWindow& window = list_windows_[id];
    std::size_t first = 0;
    std::size_t count = items.size();
    if (window.uniform && items.size() > kProbeRows) {
        if (window.row_height > 0.0f) {
            first = std::min(window.first, items.size() - 1);
            count = std::min(window.count, items.size() - first);
            if (count == 0) count = 1;
        } else {
            count = kProbeRows;
        }
    }
    window.first = first;
    window.count = count;
    window.lead = static_cast<float>(first) * window.row_height;
    window.trail = static_cast<float>(items.size() - first - count) * window.row_height;

    PrepareHtmlTemplate(item_template);
    std::string result = SpacerHtml(window.lead);
    for (std::size_t index = first; index < first + count; ++index) {
        inja::json item = items[index];
        item["__index"] = index;
        if (!item.contains("selected")) item["selected"] = false;
        const std::string row = ExpandHtmlTemplate(item_template, item);
        const std::size_t begin = row.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) continue;
        const std::size_t end = row.find_last_not_of(" \t\r\n");
        result.append(row, begin, end - begin + 1);
    }
    result += SpacerHtml(window.trail);
    return result;
}

bool HtmlDocument::UpdateListInPlace(const std::string& id) {
    if (!doc_ || !doc_->root()) return false;
    const auto html_template = list_templates_.find(id);
    if (html_template == list_templates_.end()) return false;
    const auto target = doc_->root()->select_one("#" + id);
    if (!target || !IsElementMounted(target)) return false;
    if (!target->get_render_item()) return false;

    struct ScrollOffset {
        std::shared_ptr<litehtml::element> target;
        litehtml::pixel_t left = litehtml::pixel_t(0);
        litehtml::pixel_t top = litehtml::pixel_t(0);
    };
    std::vector<ScrollOffset> scroll_offsets;
    std::vector<ScrollState> scroll_states;
    CollectScrollStates(doc_, scroll_states);
    for (const ScrollState& state : scroll_states) {
        if (!state.target) continue;
        scroll_offsets.push_back({state.target, state.left, state.top});
    }

    const std::string rows = ExpandListRows(id, html_template->second);
    const auto render_template = [this](const std::map<std::string, std::string>& attributes,
                                        std::string_view children, const HtmlElementContext& context) {
        return RenderListTemplate(attributes, children, context);
    };
    const std::string html = ExpandCustomElements(rows, render_template);

    doc_->append_children_from_string(*target, html.c_str(), true);
    element_cache_.clear();
    id_cache_.clear();
    control_cache_valid_ = false;
    doc_->refresh_styles();
    ApplyStoredAriaSelection();
    doc_->rebuild_render_tree();
    doc_->render(static_cast<litehtml::pixel_t>(ImGui::GetContentRegionAvail().x));
    for (const ScrollOffset& offset : scroll_offsets) {
        if (const auto render = offset.target->get_render_item()) {
            render->h_scroll(offset.left);
            render->v_scroll(offset.top);
        }
    }
    return true;
}

bool HtmlDocument::MeasureListRows(const std::string& id,
                                  const std::shared_ptr<litehtml::element>& list_element) {
    ListWindow& window = list_windows_[id];
    const auto list_render = list_element->get_render_item();
    if (!list_render) return false;

    std::vector<float> tops;
    std::vector<float> heights;
    for (const auto& child : list_render->children()) {
        if (!child || IsSpacer(child->src_el())) continue;
        const litehtml::position placement = child->get_placement();
        if (placement.height <= litehtml::pixel_t(0)) continue;
        tops.push_back(static_cast<float>(placement.y));
        heights.push_back(static_cast<float>(placement.height));
    }

    if (tops.size() != window.count) {
        window.uniform = false;
        return false;
    }
    if (tops.size() == 1) {
        window.row_height = heights[0];
        return true;
    }
    float pitch = tops[1] - tops[0];
    for (std::size_t index = 1; index + 1 < tops.size(); ++index) {
        if (std::abs((tops[index + 1] - tops[index]) - pitch) > 1.0f) {
            window.uniform = false;
            return false;
        }
    }
    if (pitch <= 0.0f) {
        window.uniform = false;
        return false;
    }
    window.row_height = pitch;
    return true;
}

void HtmlDocument::UpdateListWindows() {
    if (!doc_ || !doc_->root() || lists_.empty()) return;

    const std::vector<ScrollState>& states = scroll_states_;

    for (const auto& [id, items] : lists_) {
        ListWindow& window = list_windows_[id];
        if (!window.uniform || items.size() <= kProbeRows) continue;
        const auto list_element = doc_->root()->select_one("#" + id);
        if (!list_element || !IsElementMounted(list_element)) continue;
        const auto list_render = list_element->get_render_item();
        if (!list_render) continue;
        if (!MeasureListRows(id, list_element)) {
            if (window.count != items.size()) pending_list_updates_.insert(id);
            continue;
        }

        const ScrollState* scroller = nullptr;
        for (const litehtml::render_item* walk = list_render.get(); walk != nullptr && scroller == nullptr;
             walk = walk->parent().get()) {
            for (const ScrollState& state : states) {
                if (state.render_target.get() == walk) {
                    scroller = &state;
                    break;
                }
            }
        }
        if (scroller == nullptr) continue;

        const float list_top = static_cast<float>(list_render->get_placement().y);
        const float own_scroll = static_cast<float>(list_render->get_scroll_top());
        const float viewport_top = static_cast<float>(scroller->viewport_box.y);
        const float viewport_height = static_cast<float>(scroller->viewport_box.height);
        const float offset = (viewport_top - list_top) + own_scroll;

        const auto row_at = [&](const float position) {
            const float row = std::max(0.0f, position) / window.row_height;
            return static_cast<std::size_t>(row);
        };
        const std::size_t last_item = items.size() - 1;
        const std::size_t first_visible = std::min(row_at(offset), last_item);
        const std::size_t last_visible = std::min(row_at(offset + viewport_height), last_item);
        const std::size_t first = first_visible > kOverscanRows ? first_visible - kOverscanRows : 0;
        const std::size_t last = std::min(last_item, last_visible + kOverscanRows);
        const std::size_t count = last - first + 1;
        if (first == window.first && count == window.count &&
            std::abs(window.lead - static_cast<float>(first) * window.row_height) < 1.0f) {
            continue;
        }
        window.first = first;
        window.count = count;
        pending_list_updates_.insert(id);
    }
}

void HtmlDocument::ApplyPendingListUpdates() {
    if (pending_list_updates_.empty()) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    const std::unordered_set<std::string> pending = std::move(pending_list_updates_);
    pending_list_updates_.clear();
    if (needs_rebuild_) return;  // The rebuild republishes every list anyway.
    for (const std::string& id : pending) {
        if (UpdateListInPlace(id)) {
            ++in_place_list_update_count_;
        } else {
            RequestStructuralRebuild();
        }
    }
}

std::string HtmlDocument::attribute(const std::string& element_id,
                                       const std::string& name) const {
    if (element_id.empty() || !doc_ || !doc_->root()) return {};
    const auto element = doc_->root()->select_one("#" + element_id);
    if (!element) return {};
    const char* value = element->get_attr(name.c_str(), "");
    return value != nullptr ? std::string(value) : std::string();
}

std::string HtmlDocument::enclosing_list(const std::string& element_id) const {
    if (element_id.empty() || !doc_ || !doc_->root()) return {};
    auto element = doc_->root()->select_one("#" + element_id);
    for (; element; element = element->parent()) {
        if (std::string(element->get_attr("data-list", "")) != "true") continue;
        const char* id = element->get_attr("id", "");
        if (id != nullptr && *id != '\0') return id;
    }
    return {};
}

HtmlDocument::ScrollMetrics HtmlDocument::scroll_metrics(const std::string& id) {
    ScrollMetrics metrics;
    if (id.empty() || !doc_ || !doc_->root()) return metrics;
    const auto target = FindElement("#" + id);
    if (!target) return metrics;

    std::vector<ScrollState> states;
    CollectScrollStates(doc_, states);
    for (const ScrollState& state : states) {
        if (state.target != target) continue;
        metrics.valid = true;
        metrics.top = static_cast<float>(state.top);
        metrics.max_top = static_cast<float>(state.max_top);
        metrics.viewport_width = static_cast<float>(state.viewport_box.width);
        metrics.viewport_height = static_cast<float>(state.viewport_box.height);
        metrics.content_height = static_cast<float>(state.content_size.height);
        break;
    }
    return metrics;
}

bool HtmlDocument::ApplyListSelectionPaint(const std::string& id,
                                          const std::vector<inja::json>& previous,
                                          const std::vector<inja::json>& next) {
    if (!doc_ || !doc_->root() || previous.size() != next.size()) return false;
    const auto list_element = doc_->root()->select_one("#" + id);
    if (!list_element || !IsElementMounted(list_element)) return false;

    std::vector<std::pair<std::string, bool>> changes;
    changes.reserve(next.size());
    for (std::size_t index = 0; index < next.size(); ++index) {
        if (!previous[index].is_object() || !next[index].is_object() ||
            !previous[index].contains("id") || !next[index].contains("id") ||
            previous[index]["id"] != next[index]["id"] ||
            !previous[index].contains("selected") || !next[index].contains("selected")) {
            return false;
        }

        if (previous[index].size() != next[index].size()) return false;
        for (const auto& [key, value] : previous[index].items()) {
            if (key != "selected" && (!next[index].contains(key) || next[index][key] != value)) {
                return false;
            }
        }

        if (!previous[index]["selected"].is_boolean() || !next[index]["selected"].is_boolean()) {
            return false;
        }
        if (!previous[index]["id"].is_string() || !next[index]["id"].is_string()) return false;
        changes.emplace_back(previous[index]["id"].get<std::string>(),
                             next[index]["selected"].get<bool>());
    }

    for (const auto& [row_id, selected] : changes) {
        if (const auto row = doc_->root()->select_one("#" + row_id)) {
            row->set_class("selected", selected);
        }
    }
    doc_->refresh_styles();
    return true;
}

std::string HtmlDocument::RenderListTemplate(const std::map<std::string, std::string>&,
                                        std::string_view children, const HtmlElementContext& context) const {
    if (context.parent_attributes == nullptr) return {};
    const auto marker = context.parent_attributes->find("data-list");
    if (marker == context.parent_attributes->end() || marker->second != "true") return {};
    const auto id = context.parent_attributes->find("id");
    if (id == context.parent_attributes->end()) return {};
    list_templates_[id->second] = std::string(children);
    return ExpandListRows(id->second, children);
}

void HtmlDocument::select(const std::string& group, const std::string& key) {
    if (group.empty() || key.empty()) return;

    if (group == "main") {
        if (!HasFragment(key)) return;
        set_state(group + "-selection", key);
        if (current_fragment_ == key && pending_fragment_.empty()) return;
        pending_fragment_ = key;
        RequestStructuralRebuild();
        return;
    }

    set_state(group + "-selection", key);
    RequestStructuralRebuild();
}

bool HtmlDocument::HasFragment(const std::string& name) const {
    return fragments_.find(name) != fragments_.end();
}

bool HtmlDocument::handle_interaction(const std::string& url) {
    const std::string normalized_url = TrimURL(url);
    if (normalized_url.empty()) {
        return false;
    }

    if (normalized_url.rfind("event:", 0) == 0) {
        const std::size_t separator = normalized_url.find(':', 6);
        if (separator == std::string::npos) return false;
        const std::string event = normalized_url.substr(6, separator - 6);
        const std::string id = normalized_url.substr(separator + 1);
        if (event == "click") {
            const ImGuiIO& io = ImGui::GetIO();
            apply_selection(id, io.KeyCtrl, io.KeyShift);
        }
        if (dispatch_event(id, event)) return true;
        if (DispatchHtmlControlEvent(id, event)) return true;
        if (event == "toggle") return ToggleCheckbox(id);
        return false;
    }
    if (normalized_url.starts_with("#")) {
        return SelectPanelById(normalized_url.substr(1));
    }
    if (OpenExternalURL(normalized_url)) {
        return true;
    }

    return false;
}

std::string HtmlDocument::BuildFragmentHtml(const std::string& name) const {
    const auto found = fragments_.find(name);
    return found == fragments_.end() || !found->second ? std::string() : found->second();
}

void HtmlDocument::SelectInitialFragment() {
    if (!doc_ || !current_fragment_.empty()) return;
    {
        const std::string selected = state_value("main-selection", "");
        if (HasFragment(selected)) {
            current_fragment_ = selected;
            return;
        }
    }
    for (const auto& control : doc_->root()->select_all("[href]")) {
        const auto owner = NearestSwitcher(control);
        if (!owner || std::string(owner->get_attr("data-switcher", "")) != "main") continue;
        const std::string candidate = SwitchControlTarget(control);
        if (HasFragment(candidate)) {
            current_fragment_ = candidate;
            return;
        }
    }
}

void HtmlDocument::RebuildDocument() {
    ++rebuild_count_;
    inline_styles_.clear();
    element_texts_.clear();
    element_cache_.clear();
    id_cache_.clear();
    control_cache_valid_ = false;
    mounted_lazy_panels_.clear();

    if (shell_html_.empty()) {
        doc_.reset();
        content_slot_.reset();
        return;
    }

    doc_.reset();
    content_slot_.reset();
    ResetDocument("app");

    if (!doc_) {
        const int width = static_cast<int>(ImGui::GetContentRegionAvail().x);
        const auto render_template = [this](const std::map<std::string, std::string>& attributes,
                                            std::string_view children, const HtmlElementContext& context) {
            return RenderListTemplate(attributes, children, context);
        };
        std::string expanded_shell = ExpandCustomElements(shell_html_, render_template);
        InsertStylesheet(expanded_shell, stylesheet_provider_ ? stylesheet_provider_() : std::string());
        doc_ = ParseDocument("app", expanded_shell.c_str(), static_cast<float>(width));
        if (!doc_) {
            content_slot_.reset();
            return;
        }
        content_slot_ = doc_->root()->select_one("#content-slot");
        if (const auto sidebar = fragments_.find("sidebar"); sidebar != fragments_.end() && sidebar->second) {
            if (const auto target = doc_->root()->select_one("#sidebar-nav")) {
                const std::string sidebar_html = ExpandCustomElements(sidebar->second(), render_template);
                doc_->append_children_from_string(*target, sidebar_html.c_str(), false);
            }
        }
        SelectInitialFragment();
    }

    if (content_slot_) {
        const std::string fragment_html = BuildFragmentHtml(current_fragment_);
        if (!fragment_html.empty()) {
            const auto render_template = [this](const std::map<std::string, std::string>& attributes,
                                                std::string_view children, const HtmlElementContext& context) {
                return RenderListTemplate(attributes, children, context);
            };
            const std::string expanded_fragment_html = ExpandCustomElements(fragment_html, render_template);
            doc_->append_children_from_string(*content_slot_, expanded_fragment_html.c_str(), true);
        }
    }

    for (const auto& [fragment_id, provider] : fragments_) {
        if (fragment_id == "sidebar" || !provider) continue;
        if (const auto target = doc_->root()->select_one("#" + fragment_id)) {
            const auto owner = NearestSwitcher(target);
            if (owner && std::string(owner->get_attr("data-switcher", "")) != "main") continue;
            const auto render_template = [this](const std::map<std::string, std::string>& attributes,
                                                std::string_view children, const HtmlElementContext& context) {
                return RenderListTemplate(attributes, children, context);
            };
            const std::string fragment_html = ExpandCustomElements(provider(), render_template);
            doc_->append_children_from_string(*target, fragment_html.c_str(), false);
        }
    }

    MountLazyPanels();

    RefreshControlCaches();

    SyncCheckboxes();
    SyncTextInputs();
    SyncSelects();
    SyncRanges();
    ApplySwitchers();
    ApplyStoredAriaSelection();
    doc_->refresh_styles();
    doc_->rebuild_render_tree();
    MarkDocumentDirty("app");
}

void HtmlDocument::RefreshControlCaches() {
    checkbox_inputs_.clear();
    text_inputs_.clear();
    select_elements_.clear();
    range_inputs_.clear();
    if (!doc_ || !doc_->root()) {
        control_cache_valid_ = true;
        return;
    }

    const auto copy = [](const auto& source, auto& destination) {
        destination.assign(source.begin(), source.end());
    };
    copy(doc_->root()->select_all("input[type=checkbox]"), checkbox_inputs_);
    copy(doc_->root()->select_all("input[type=text]"), text_inputs_);
    copy(doc_->root()->select_all("select"), select_elements_);
    copy(doc_->root()->select_all("input[type=range]"), range_inputs_);
    control_cache_valid_ = true;
}

void HtmlDocument::SyncCheckboxes() {
    if (!doc_ || !doc_->root()) return;

    for (const auto& checkbox : checkbox_inputs_) {
        const std::string id = checkbox->get_attr("id", "");
        if (id.empty()) continue;
        const bool initial = checkbox->get_attr("checked", nullptr) != nullptr;
        const std::string stored = state_value(id, "");
        const bool checked = stored.empty() ? initial : stored == "true";
        if (!has_value(id)) set_state(id, checked ? "true" : "false");
        const char* serialized = checked ? "true" : "false";
        if (std::strcmp(checkbox->get_attr("data-checked", ""), serialized) != 0) {
            checkbox->set_attr("data-checked", serialized);
        }
        if (std::strcmp(checkbox->get_attr("aria-checked", ""), serialized) != 0) {
            checkbox->set_attr("aria-checked", serialized);
        }
    }
}

void HtmlDocument::SyncTextInputs() {
    if (!doc_ || !doc_->root()) return;

    for (const auto& input : text_inputs_) {
        const std::string id = input->get_attr("id", "");
        if (id.empty()) continue;

        auto& state = text_states_[id];
        if (active_text_id_ == id) continue;

        const std::string initial = input->get_attr("value", "");
        const bool has_state = has_value(id);
        const std::string value = state_value(id, initial);
        if (!has_state) set_state(id, initial);
        if (state.value != value) {
            state.value = value;
            state.cursor = state.anchor = state.value.size();
            state.scroll_x = 0.0f;
        }
        if (std::strcmp(input->get_attr("data-value", ""), state.value.c_str()) != 0) {
            input->set_attr("data-value", state.value.c_str());
        }
    }
}

void HtmlDocument::DrawTextInputs(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root()) return;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;

    const auto& inputs = text_inputs_;
    const ImRect visible_rect(draw_list->GetClipRectMin(), draw_list->GetClipRectMax());
    const ImVec2 mouse_pos = ImGui::GetMousePos();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 mouse(mouse_pos.x + window_pos.x, mouse_pos.y + window_pos.y);
    const bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    std::shared_ptr<litehtml::element> clicked_input;
    litehtml::position clicked_box;
    const auto cursor_at_mouse = [&](const std::shared_ptr<litehtml::element>& input,
                                     const litehtml::position& box,
                                     const TextInputState& state) {
        const auto render = input->get_render_item();
        const float font_size = static_cast<float>(input->css().get_font_metrics().font_size);
        const float left = document_origin.x + static_cast<float>(box.x) +
                           (render ? static_cast<float>(render->get_borders().left + render->get_paddings().left) : 0.0f);
        const float relative_x = std::max(0.0f, mouse.x - left + state.scroll_x);
        std::size_t cursor = 0;
        while (cursor < state.value.size() &&
               TextWidth(ImGui::GetFont(), font_size,
                         Utf8Prefix(state.value, NextUtf8(state.value, cursor))) < relative_x) {
            cursor = NextUtf8(state.value, cursor);
        }
        return cursor;
    };
    for (const auto& input : inputs) {
        litehtml::position box;
        if (!ElementBox(input, box)) continue;
        const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                         document_origin.y + static_cast<float>(box.y));
        const ImVec2 max(min.x + static_cast<float>(box.width), min.y + static_cast<float>(box.height));
        if (mouse_clicked && mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y) {
            clicked_input = input;
            clicked_box = box;
            break;
        }
    }

    if (mouse_clicked && active_text_id_ != "" && (!clicked_input || clicked_input->get_attr("id", "") != active_text_id_)) {
        if (doc_->root()->select_one("#" + active_text_id_)) {
            dispatch_event(active_text_id_, "change");
        }
        active_text_id_.clear();
        text_mouse_selecting_ = false;
    }
    if (clicked_input) {
        active_text_id_ = clicked_input->get_attr("id", "");
        NotifyHtmlControlTextFocus(active_text_id_);
        auto& state = text_states_[active_text_id_];
        state.cursor = state.anchor = cursor_at_mouse(clicked_input, clicked_box, state);
        text_mouse_selecting_ = true;
    }

    if (text_mouse_selecting_ && !active_text_id_.empty() && (mouse_down || mouse_released)) {
        for (const auto& input : inputs) {
            if (input->get_attr("id", "") != active_text_id_) continue;
            litehtml::position box;
            if (ElementBox(input, box)) {
                auto& state = text_states_[active_text_id_];
                state.cursor = cursor_at_mouse(input, box, state);
            }
            break;
        }
    }
    if (mouse_released) {
        text_mouse_selecting_ = false;
    }

    SDL_Window* text_input_window = SDL_GetKeyboardFocus();
    if (text_input_window != nullptr) {
        if (!active_text_id_.empty()) {
            if (!SDL_TextInputActive(text_input_window)) SDL_StartTextInput(text_input_window);
        } else if (SDL_TextInputActive(text_input_window)) {
            SDL_StopTextInput(text_input_window);
        }
    }

    bool any_active = false;
    for (const auto& input : inputs) {
        const std::string id = input->get_attr("id", "");
        if (id.empty()) continue;
        auto& state = text_states_[id];

        litehtml::position box;
        if (!ElementBox(input, box)) continue;
        const auto render = input->get_render_item();
        if (!render) continue;

        const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                         document_origin.y + static_cast<float>(box.y));
        const ImVec2 max(min.x + static_cast<float>(box.width), min.y + static_cast<float>(box.height));
        const bool active = active_text_id_ == id;
        const bool visible = visible_rect.Overlaps(ImRect(min, max));
        if (!visible && !active) continue;
        const auto& font_metrics = input->css().get_font_metrics();
        const float font_size = static_cast<float>(font_metrics.font_size);
        const float line_height = static_cast<float>(font_metrics.height);
        const litehtml::web_color text_color = input->css().get_color();
        const ImU32 text_u32 = IM_COL32(text_color.red, text_color.green, text_color.blue, text_color.alpha);
        const ImU32 selection_u32 = ToImColor(CssBorderColor(input));
        const ImU32 placeholder_u32 = ToImColor(text_color);
        const auto& padding = render->get_paddings();
        const auto& border = render->get_borders();
        const float content_left = min.x + static_cast<float>(border.left + padding.left);
        const float content_right = max.x - static_cast<float>(border.right + padding.right);
        const float content_width = std::max(1.0f, content_right - content_left);
        const float text_y = min.y + (max.y - min.y - line_height) * 0.5f;
        if (active) any_active = true;
        std::string value_before = state.value;
        if (active) active_text_id_ = id;

        if (active) {
            if (!HandleHtmlControlTextKey(id)) {
                const bool extend = ImGui::GetIO().KeyShift;
                auto move_cursor = [&](std::size_t cursor) {
                    state.cursor = cursor;
                    if (!extend) state.anchor = state.cursor;
                };
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                const std::size_t selection_start = std::min(state.cursor, state.anchor);
                move_cursor(!extend && state.cursor != state.anchor
                                 ? selection_start
                                 : (state.cursor > 0 ? PreviousUtf8(state.value, state.cursor) : 0));
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                const std::size_t selection_end = std::max(state.cursor, state.anchor);
                move_cursor(!extend && state.cursor != state.anchor
                                 ? selection_end
                                 : NextUtf8(state.value, state.cursor));
            } else if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) {
                move_cursor(0);
            } else if (ImGui::IsKeyPressed(ImGuiKey_End, true)) {
                move_cursor(state.value.size());
            } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
                const std::size_t start = std::min(state.cursor, state.anchor);
                const std::size_t end = std::max(state.cursor, state.anchor);
                const std::size_t erase_start = start == end && start > 0 ? PreviousUtf8(state.value, start) : start;
                state.value.erase(erase_start, end - erase_start);
                state.cursor = state.anchor = erase_start;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) {
                const std::size_t start = std::min(state.cursor, state.anchor);
                const std::size_t end = std::max(state.cursor, state.anchor);
                const std::size_t erase_end = start == end ? NextUtf8(state.value, end) : end;
                state.value.erase(start, erase_end - start);
                state.cursor = state.anchor = start;
            } else if (ImGui::IsKeyPressed(ImGuiKey_A) && ImGui::GetIO().KeyCtrl) {
                state.anchor = 0;
                state.cursor = state.value.size();
            } else if (ImGui::IsKeyPressed(ImGuiKey_C) && ImGui::GetIO().KeyCtrl) {
                const std::size_t start = std::min(state.cursor, state.anchor);
                const std::size_t end = std::max(state.cursor, state.anchor);
                if (start != end) ImGui::SetClipboardText(state.value.substr(start, end - start).c_str());
            } else if (ImGui::IsKeyPressed(ImGuiKey_X) && ImGui::GetIO().KeyCtrl) {
                const std::size_t start = std::min(state.cursor, state.anchor);
                const std::size_t end = std::max(state.cursor, state.anchor);
                if (start != end) {
                    ImGui::SetClipboardText(state.value.substr(start, end - start).c_str());
                    state.value.erase(start, end - start);
                    state.cursor = state.anchor = start;
                }
            } else if (ImGui::IsKeyPressed(ImGuiKey_V) && ImGui::GetIO().KeyCtrl) {
                if (const char* clipboard = ImGui::GetClipboardText(); clipboard != nullptr) {
                    const std::size_t start = std::min(state.cursor, state.anchor);
                    const std::size_t end = std::max(state.cursor, state.anchor);
                    state.value.replace(start, end - start, clipboard);
                    state.cursor = start + std::strlen(clipboard);
                    state.anchor = state.cursor;
                }
            } else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                dispatch_event(id, "change");
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                const auto current = std::ranges::find_if(inputs, [&](const auto& candidate) {
                    return candidate->get_attr("id", "") == id;
                });
                if (current != inputs.end()) {
                    auto next = std::next(current);
                    if (next == inputs.end()) next = inputs.begin();
                    active_text_id_ = (*next)->get_attr("id", "");
                }
            }

                for (const ImWchar character : ImGui::GetIO().InputQueueCharacters) {
                    if (character < 0x20 || character == 0x7f) continue;
                    std::string inserted;
                    AppendUtf8(inserted, static_cast<unsigned int>(character));
                    const std::size_t start = std::min(state.cursor, state.anchor);
                    const std::size_t end = std::max(state.cursor, state.anchor);
                    state.value.replace(start, end - start, inserted);
                    state.cursor = state.anchor = start + inserted.size();
                }
            }
        }

        if (active && state.value != value_before) {
            input->set_attr("data-value", state.value.c_str());
            if (!NotifyHtmlControlTextChanged(id, state.value)) {
                if (!dispatch_event(id, "input")) {
                    set_state(id, state.value);
                }
            }
        }

        // Keep keyboard handling above for an active control that scrolled
        // out of view, but do not emit a native overlay draw command for it.
        if (!visible) continue;

        const float cursor_width = TextWidth(ImGui::GetFont(), font_size, Utf8Prefix(state.value, state.cursor));
        const float selection_start = TextWidth(ImGui::GetFont(), font_size,
                                                 Utf8Prefix(state.value, std::min(state.cursor, state.anchor)));
        const float selection_end = TextWidth(ImGui::GetFont(), font_size,
                                               Utf8Prefix(state.value, std::max(state.cursor, state.anchor)));
        if (active) {
            if (cursor_width - state.scroll_x > content_width) state.scroll_x = cursor_width - content_width;
            if (cursor_width - state.scroll_x < 0.0f) state.scroll_x = cursor_width;
            state.scroll_x = std::max(0.0f, state.scroll_x);
        }

        NativeControlClipScope scroll_clip(scroll_states_, input, document_origin, draw_list);
        draw_list->PushClipRect(ImVec2(content_left, min.y), ImVec2(content_right, max.y), true);
        const float draw_x = content_left - state.scroll_x;
        if (state.cursor != state.anchor) {
            draw_list->AddRectFilled(ImVec2(draw_x + selection_start, min.y + 2.0f),
                                     ImVec2(draw_x + selection_end, max.y - 2.0f), selection_u32);
        }
        const char* placeholder = input->get_attr("placeholder", "");
        if (state.value.empty() && !active && placeholder != nullptr && placeholder[0] != '\0') {
            draw_list->AddText(ImGui::GetFont(), font_size, ImVec2(content_left, text_y),
                               placeholder_u32, placeholder);
        } else {
            draw_list->AddText(ImGui::GetFont(), font_size, ImVec2(draw_x, text_y), text_u32, state.value.c_str());
        }
        if (active && (static_cast<int>(ImGui::GetTime() * 2.0) % 2 == 0)) {
            const float cursor_x = draw_x + cursor_width;
            const float caret_height = std::min(line_height, std::max(1.0f, max.y - min.y - 6.0f));
            const float caret_top = min.y + (max.y - min.y - caret_height) * 0.5f;
            draw_list->AddLine(ImVec2(cursor_x, caret_top),
                               ImVec2(cursor_x, caret_top + caret_height), text_u32, 1.0f);
        }
        draw_list->PopClipRect();
    }

    if (!any_active) active_text_id_.clear();
}

void HtmlDocument::SyncSelects() {
    if (!doc_ || !doc_->root()) return;

    bool options_changed = false;
    for (const auto& select : select_elements_) {
        const std::string id = select->get_attr("id", "");
        if (id.empty()) continue;

        const std::vector<std::string>& options = this->options(id);
        const auto known = select_options_.find(id);
        const bool missing_dom_options = select->select_all("option").size() != options.size();
        if (known == select_options_.end() || known->second != options || missing_dom_options) {
            std::string html;
            for (const std::string& option : options) {
                const std::string escaped = EscapeHtml(option);
                html += "<option value=\"" + escaped + "\">" + escaped + "</option>";
            }
            doc_->append_children_from_string(*select, html.c_str(), true);
            select_options_[id] = options;
            options_changed = true;
        }

        const std::string stored = state_value(id, "");
        const auto selected = std::find(options.begin(), options.end(), stored);
        const std::string current = selected != options.end()
                                        ? stored
                                        : (options.empty() ? std::string() : options.front());
        if (stored != current) set_state(id, current);

        if (std::strcmp(select->get_attr("data-value", ""), current.c_str()) != 0) {
            select->set_attr("data-value", current.c_str());
        }
        if (std::strcmp(select->get_attr("aria-valuetext", ""), current.c_str()) != 0) {
            select->set_attr("aria-valuetext", current.c_str());
        }
        for (const auto& option : select->select_all("option")) {
            const char* value = option->get_attr("value", "");
            const bool is_selected = value != nullptr && current == value;
            const char* serialized = is_selected ? "true" : "false";
            if (std::strcmp(option->get_attr("selected", ""), serialized) != 0) {
                option->set_attr("selected", serialized);
            }
            if (std::strcmp(option->get_attr("aria-selected", ""), serialized) != 0) {
                option->set_attr("aria-selected", serialized);
            }
        }
    }

    if (options_changed) {
        doc_->refresh_styles();
        doc_->rebuild_render_tree();
        MarkDocumentLayoutDirty("app");
    }
}

void HtmlDocument::SyncRanges() {
    if (!doc_ || !doc_->root()) return;

    for (const auto& range : range_inputs_) {
        const std::string id = range->get_attr("id", "");
        if (id.empty()) continue;

        const double minimum = ParseRangeNumber(range->get_attr("min", "0"), 0.0);
        const double maximum = ParseRangeNumber(range->get_attr("max", "100"), 100.0);
        const double low = std::min(minimum, maximum);
        const double high = std::max(minimum, maximum);
        const double initial = ParseRangeNumber(range->get_attr("value", ""), low);
        const std::string stored = state_value(id, "");
        const double current = QuantizeRangeValue(
            stored.empty() ? initial : ParseRangeNumber(stored.c_str(), initial), low, high, RangeStep(range));
        const std::string serialized = SerializeRangeValue(current);

        if (!has_value(id)) set_state(id, serialized);
        if (std::strcmp(range->get_attr("data-range-value", ""), serialized.c_str()) != 0) {
            range->set_attr("data-range-value", serialized.c_str());
        }
        const std::string minimum_text = SerializeRangeValue(low);
        if (std::strcmp(range->get_attr("aria-valuemin", ""), minimum_text.c_str()) != 0) {
            range->set_attr("aria-valuemin", minimum_text.c_str());
        }
        const std::string maximum_text = SerializeRangeValue(high);
        if (std::strcmp(range->get_attr("aria-valuemax", ""), maximum_text.c_str()) != 0) {
            range->set_attr("aria-valuemax", maximum_text.c_str());
        }
        if (std::strcmp(range->get_attr("aria-valuenow", ""), serialized.c_str()) != 0) {
            range->set_attr("aria-valuenow", serialized.c_str());
        }
        set_text("#" + id + "-value", FormatRangeValue(current));
    }
}

void HtmlDocument::UpdateSelectFromMouse(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root() || !doc_->root_render()) return;

    const ImVec2 mouse_pos = ImGui::GetMousePos();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 mouse(mouse_pos.x + window_pos.x, mouse_pos.y + window_pos.y);
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    if (!active_select_id_.empty()) {
        const auto select = doc_->root()->select_one("#" + active_select_id_);
        const std::vector<std::string> options = SelectOptionValues(select);
        SelectPopupGeometry popup;
        if (!select || options.empty() || !GetSelectPopupGeometry(select, document_origin, options.size(), popup)) {
            active_select_id_.clear();
            return;
        }

        const float maximum_start = static_cast<float>(options.size() - popup.visible_count);
        float& scroll = select_scroll_offsets_[active_select_id_];
        scroll = ImClamp(scroll, 0.0f, maximum_start);
        const bool in_popup = mouse.x >= popup.min.x && mouse.x < popup.max.x &&
                              mouse.y >= popup.min.y && mouse.y < popup.max.y;
        SelectScrollbarGeometry scrollbar;
        const bool has_scrollbar = GetSelectScrollbarGeometry(
            select, popup, options.size(), scroll, scrollbar);
        if (!has_scrollbar) select_scroll_dragging_ = false;
        const auto point_in = [](const ImVec2& point, const ImVec2& min, const ImVec2& max) {
            return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
        };

        if (in_popup && ImGui::GetIO().MouseWheel != 0.0f) {
            scroll = ImClamp(scroll - ImGui::GetIO().MouseWheel * 3.0f, 0.0f, maximum_start);
        }

        if (select_scroll_dragging_) {
            if (mouse_down || mouse_released) {
                const float track_height = scrollbar.track_max.y - scrollbar.track_min.y;
                const float thumb_height = scrollbar.thumb_max.y - scrollbar.thumb_min.y;
                const float travel = std::max(1.0f, track_height - thumb_height);
                scroll = ImClamp((mouse.y - scrollbar.track_min.y - select_scroll_drag_offset_) /
                                     travel * maximum_start,
                                 0.0f, maximum_start);
            }
            if (mouse_released) select_scroll_dragging_ = false;
            return;
        }

        const auto commit = [&](const std::string& value) {
            set_state(active_select_id_, value);
            select->set_attr("data-value", value.c_str());
            select->set_attr("aria-valuetext", value.c_str());
            for (const auto& option : select->select_all("option")) {
                const bool selected = std::string(option->get_attr("value", "")) == value;
                option->set_attr("selected", selected ? "true" : "false");
                option->set_attr("aria-selected", selected ? "true" : "false");
            }
            dispatch_event(active_select_id_, "change");
        };

        const std::string current = select->get_attr("data-value", "");
        const auto current_it = std::find(options.begin(), options.end(), current);
        const std::size_t current_index = current_it == options.end()
                                               ? 0
                                               : static_cast<std::size_t>(std::distance(options.begin(), current_it));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            active_select_id_.clear();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            commit(options[current_index == 0 ? options.size() - 1 : current_index - 1]);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            commit(options[(current_index + 1) % options.size()]);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space)) {
            active_select_id_.clear();
            return;
        }

        if (clicked) {
            if (in_popup) {
                if (has_scrollbar && point_in(mouse, scrollbar.track_min, scrollbar.track_max)) {
                    if (point_in(mouse, scrollbar.thumb_min, scrollbar.thumb_max)) {
                        select_scroll_dragging_ = true;
                        select_scroll_drag_offset_ = mouse.y - scrollbar.thumb_min.y;
                    } else {
                        const float track_height = scrollbar.track_max.y - scrollbar.track_min.y;
                        const float thumb_height = scrollbar.thumb_max.y - scrollbar.thumb_min.y;
                        const float travel = std::max(1.0f, track_height - thumb_height);
                        scroll = ImClamp((mouse.y - scrollbar.track_min.y - thumb_height * 0.5f) /
                                             travel * maximum_start,
                                         0.0f, maximum_start);
                    }
                    return;
                }
                const float row = mouse.y - popup.min.y - popup.padding;
                const std::size_t index = static_cast<std::size_t>(scroll) +
                                          static_cast<std::size_t>(std::floor(row / popup.row_height));
                if (row >= 0.0f && index < options.size()) {
                    commit(options[index]);
                }
                active_select_id_.clear();
                select_scroll_dragging_ = false;
                return;
            }

            litehtml::position box;
            const bool in_control = ElementBox(select, box) &&
                                     mouse.x >= document_origin.x + static_cast<float>(box.x) &&
                                     mouse.x < document_origin.x + static_cast<float>(box.x + box.width) &&
                                     mouse.y >= document_origin.y + static_cast<float>(box.y) &&
                                     mouse.y < document_origin.y + static_cast<float>(box.y + box.height);
            if (!in_control) {
                active_select_id_.clear();
                select_scroll_dragging_ = false;
            }
            return;
        }
        return;
    }

    if (!clicked) return;

    const float x = mouse.x - document_origin.x;
    const float y = mouse.y - document_origin.y;
    const auto hit = doc_->root_render()->get_element_by_point(
        static_cast<litehtml::pixel_t>(x), static_cast<litehtml::pixel_t>(y),
        static_cast<litehtml::pixel_t>(x), static_cast<litehtml::pixel_t>(y), nullptr);

    std::shared_ptr<litehtml::element> select;
    for (auto current = hit; current; current = current->parent()) {
        const char* tag = current->get_tagName();
        if (tag != nullptr && std::strcmp(tag, "select") == 0) {
            select = current;
            break;
        }
        if (tag != nullptr && std::strcmp(tag, "label") == 0) {
            const char* target = current->get_attr("for", "");
            if (target != nullptr && target[0] != '\0') {
                select = doc_->root()->select_one("#" + std::string(target));
            }
            break;
        }
    }

    if (!select) {
        active_select_id_.clear();
        return;
    }
    const std::string id = select->get_attr("id", "");
    if (id.empty() || SelectOptionValues(select).empty()) {
        active_select_id_.clear();
        return;
    }

    active_select_id_ = id;
    select_scroll_offsets_[id] = 0.0f;
    select_scroll_dragging_ = false;
}

void HtmlDocument::DrawSelects(const ImVec2& document_origin, const bool popup_only) {
    if (!doc_ || !doc_->root()) return;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;
    const ImRect visible_rect(draw_list->GetClipRectMin(), draw_list->GetClipRectMax());

    if (!popup_only) {
        for (const auto& select : select_elements_) {
            litehtml::position box;
            if (!ElementBox(select, box)) continue;
            const auto render = select->get_render_item();
            if (!render) continue;

            const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                             document_origin.y + static_cast<float>(box.y));
            const ImVec2 max(min.x + static_cast<float>(box.width), min.y + static_cast<float>(box.height));
            if (!visible_rect.Overlaps(ImRect(min, max))) continue;
            const litehtml::web_color text_color = select->css().get_color();
            const float font_size = static_cast<float>(select->css().get_font_metrics().font_size);
            const ImU32 text_u32 = IM_COL32(text_color.red, text_color.green, text_color.blue, text_color.alpha);
            const auto& padding = render->get_paddings();
            const auto& border = render->get_borders();
            const std::string value = select->get_attr("data-value", "");
            const ImVec2 text_min(min.x + static_cast<float>(border.left + padding.left),
                                  min.y + (max.y - min.y - font_size) * 0.5f);
            NativeControlClipScope scroll_clip(scroll_states_, select, document_origin, draw_list);
            draw_list->AddText(ImGui::GetFont(), font_size, text_min, text_u32, value.c_str());

            const float arrow_size = std::max(2.0f, font_size * 0.2f);
            const ImVec2 arrow(max.x - static_cast<float>(border.right + padding.right) - arrow_size,
                               (min.y + max.y) * 0.5f);
            draw_list->AddLine(ImVec2(arrow.x - arrow_size, arrow.y - arrow_size * 0.35f),
                               ImVec2(arrow.x, arrow.y + arrow_size * 0.35f), text_u32, 1.2f);
            draw_list->AddLine(ImVec2(arrow.x, arrow.y + arrow_size * 0.35f),
                               ImVec2(arrow.x + arrow_size, arrow.y - arrow_size * 0.35f), text_u32, 1.2f);
        }
    }

    if (active_select_id_.empty()) return;
    const auto select = doc_->root()->select_one("#" + active_select_id_);
    if (!select) {
        active_select_id_.clear();
        return;
    }
    litehtml::position box;
    if (!ElementBox(select, box)) {
        active_select_id_.clear();
        return;
    }
    const std::vector<std::string> options = SelectOptionValues(select);
    if (options.empty()) {
        active_select_id_.clear();
        return;
    }

    SelectPopupGeometry popup;
    if (!GetSelectPopupGeometry(select, document_origin, options.size(), popup)) {
        active_select_id_.clear();
        return;
    }

    const float font_size = static_cast<float>(select->css().get_font_metrics().font_size);
    const auto& select_borders = select->css().get_borders();
    const auto popup_radii = select_borders.radius.calc_percents(box.width, box.height);
    const float rounding = std::min({static_cast<float>(popup_radii.top_left_x),
                                     static_cast<float>(popup_radii.top_right_x),
                                     static_cast<float>(popup_radii.bottom_right_x),
                                     static_cast<float>(popup_radii.bottom_left_x)});
    float& scroll = select_scroll_offsets_[active_select_id_];
    const float maximum_start = static_cast<float>(options.size() - popup.visible_count);
    scroll = ImClamp(scroll, 0.0f, maximum_start);
    const std::size_t first = static_cast<std::size_t>(scroll);
    const std::string current = select->get_attr("data-value", "");
    const ImVec2 mouse_pos = ImGui::GetMousePos();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 mouse(mouse_pos.x + window_pos.x, mouse_pos.y + window_pos.y);
    SelectScrollbarGeometry scrollbar;
    const bool has_scrollbar = GetSelectScrollbarGeometry(select, popup, options.size(), scroll, scrollbar);
    const bool mouse_in_popup = mouse.x >= popup.min.x && mouse.x < popup.max.x &&
                                mouse.y >= popup.min.y && mouse.y < popup.max.y;
    const bool mouse_in_option_area = mouse_in_popup &&
                                      (!has_scrollbar || mouse.x < scrollbar.track_min.x);
    const std::size_t hovered = mouse_in_option_area && mouse.y >= popup.min.y + popup.padding
                                    ? first + static_cast<std::size_t>(
                                          std::floor((mouse.y - popup.min.y - popup.padding) / popup.row_height))
                                    : options.size();

    const auto option_list = select->select_all("option");
    std::vector<std::shared_ptr<litehtml::element>> option_elements(option_list.begin(), option_list.end());
    bool hover_state_changed = false;
    for (std::size_t index = 0; index < option_elements.size(); ++index) {
        const bool is_hovered = index == hovered;
        const char* desired = is_hovered ? "true" : "false";
        const char* current_hovered = option_elements[index]->get_attr("data-hovered", nullptr);
        if (current_hovered == nullptr || std::strcmp(current_hovered, desired) != 0) {
            option_elements[index]->set_attr("data-hovered", desired);
            hover_state_changed = true;
        }
    }
    if (hover_state_changed) {
        doc_->refresh_styles();
    }
    const auto popup_background = CssBackgroundColor(select);
    const auto popup_border = select_borders.top.color;
    const auto& select_scrollbar = select->css().get_scrollbar_colors();
    const auto scrollbar_track = select_scrollbar.auto_value ? CssBackgroundColor(select) : select_scrollbar.track;
    const auto scrollbar_thumb = select_scrollbar.auto_value ? CssBorderColor(select) : select_scrollbar.thumb;
    const auto scrollbar_thumb_hover = scrollbar_thumb;
    const ImU32 option_selected_color = ToImColor(CssAccentColor(select));

    draw_list->AddRectFilled(popup.min, popup.max, ToImColor(popup_background), rounding);
    const float popup_border_width = select->get_render_item()
                                         ? static_cast<float>(select->get_render_item()->get_borders().top)
                                         : 0.0f;
    if (popup_border_width > 0.0f) {
        draw_list->AddRect(popup.min, popup.max, ToImColor(popup_border), rounding, 0,
                           popup_border_width);
    }
    draw_list->PushClipRect(popup.min, popup.max, true);
    for (std::size_t offset = 0; offset < popup.visible_count && first + offset < options.size(); ++offset) {
        const std::size_t index = first + offset;
        const float top = popup.min.y + popup.padding + static_cast<float>(offset) * popup.row_height;
        const ImVec2 row_min(popup.min.x + 1.0f, top);
        const ImVec2 row_max(popup.max.x - 1.0f, top + popup.row_height);
        const auto option = index < option_elements.size() ? option_elements[index] : select;
        const bool is_selected = options[index] == current;
        if (is_selected) {
            draw_list->AddRectFilled(row_min, row_max, option_selected_color, rounding);
        } else if (index == hovered) {
            draw_list->AddRectFilled(row_min, row_max, ToImColor(CssBackgroundColor(option)), rounding);
        }
        draw_list->AddText(ImGui::GetFont(), font_size,
                           ImVec2(popup.min.x + popup.padding, top + (popup.row_height - font_size) * 0.5f),
                           ToImColor(option->css().get_color()), options[index].c_str());
    }
    if (has_scrollbar) {
        const float radius = (scrollbar.track_max.x - scrollbar.track_min.x) * 0.5f;
        draw_list->AddRectFilled(scrollbar.track_min, scrollbar.track_max, ToImColor(scrollbar_track), radius);
        const ImU32 thumb_color = mouse_in_popup &&
                                          mouse.x >= scrollbar.thumb_min.x && mouse.x < scrollbar.thumb_max.x &&
                                          mouse.y >= scrollbar.thumb_min.y && mouse.y < scrollbar.thumb_max.y
                                      ? ToImColor(scrollbar_thumb_hover)
                                      : ToImColor(scrollbar_thumb);
        draw_list->AddRectFilled(scrollbar.thumb_min, scrollbar.thumb_max, thumb_color, radius);
    }
    draw_list->PopClipRect();
}

void HtmlDocument::UpdateRangeFromMouse(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root() || !doc_->root_render()) return;

    const ImVec2 mouse_pos = ImGui::GetMousePos();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 mouse(mouse_pos.x + window_pos.x, mouse_pos.y + window_pos.y);
    const float x = mouse.x - document_origin.x;
    const float y = mouse.y - document_origin.y;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        active_range_id_.clear();
        const auto hit = doc_->root_render()->get_element_by_point(
            static_cast<litehtml::pixel_t>(x), static_cast<litehtml::pixel_t>(y),
            static_cast<litehtml::pixel_t>(x), static_cast<litehtml::pixel_t>(y), nullptr);
        if (const auto range = RangeAncestor(hit)) {
            active_range_id_ = range->get_attr("id", "");
        }
    }

    if (active_range_id_.empty()) return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        return;
    }

    const auto range = doc_->root()->select_one("#" + active_range_id_);
    litehtml::position box;
    if (!IsRangeInput(range) || !RangeBox(range, box)) {
        active_range_id_.clear();
        return;
    }

    const double minimum = ParseRangeNumber(range->get_attr("min", "0"), 0.0);
    const double maximum = ParseRangeNumber(range->get_attr("max", "100"), 100.0);
    const double low = std::min(minimum, maximum);
    const double high = std::max(minimum, maximum);
    const float height = static_cast<float>(box.height);
    const float knob_width = RangeThumbWidth(height);
    const float travel = std::max(static_cast<float>(box.width) - knob_width, 1.0f);
    const float normalized = ImClamp(
        (x - static_cast<float>(box.x) - knob_width * 0.5f) / travel, 0.0f, 1.0f);
    const double value = QuantizeRangeValue(low + (high - low) * normalized, low, high, RangeStep(range));
    const std::string serialized = SerializeRangeValue(value);

    set_state(active_range_id_, serialized);
    range->set_attr("data-range-value", serialized.c_str());
    range->set_attr("aria-valuenow", serialized.c_str());
    set_text("#" + active_range_id_ + "-value", FormatRangeValue(value));
    dispatch_event(active_range_id_, "input");

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dispatch_event(active_range_id_, "change");
        active_range_id_.clear();
    }
}

void HtmlDocument::DrawRanges(const ImVec2& document_origin) const {
    if (!doc_ || !doc_->root()) return;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;
    const ImRect visible_rect(draw_list->GetClipRectMin(), draw_list->GetClipRectMax());

    for (const auto& range : range_inputs_) {
        litehtml::position box;
        if (!RangeBox(range, box)) continue;

        const double minimum = ParseRangeNumber(range->get_attr("min", "0"), 0.0);
        const double maximum = ParseRangeNumber(range->get_attr("max", "100"), 100.0);
        const double low = std::min(minimum, maximum);
        const double high = std::max(minimum, maximum);
        const double current = QuantizeRangeValue(
            ParseRangeNumber(range->get_attr("data-range-value", ""), low), low, high, RangeStep(range));

        const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                         document_origin.y + static_cast<float>(box.y));
        const ImVec2 max(min.x + static_cast<float>(box.width), min.y + static_cast<float>(box.height));
        if (!visible_rect.Overlaps(ImRect(min, max))) continue;
        const float height = static_cast<float>(box.height);
        const float knob_width = RangeThumbWidth(height);
        const float travel_left = min.x + knob_width * 0.5f;
        const float travel = std::max(max.x - min.x - knob_width, 1.0f);
        const float normalized = high > low
                                    ? ImClamp(static_cast<float>((current - low) / (high - low)), 0.0f, 1.0f)
                                    : 0.0f;
        const float knob_x = travel_left + travel * normalized;
        const float track_height = std::max(2.0f, height * 0.22f);
        const float track_radius = track_height * 0.5f;
        const float center_y = (min.y + max.y) * 0.5f;
        const auto background = CssBackgroundColor(range);
        const auto track = background.alpha == 0 ? range->css().get_color().darken(0.7) : background;
        const ImU32 track_color = ToImColor(track);
        const litehtml::web_color accent = range->css().get_accent_color().auto_value
                                               ? range->css().get_color()
                                               : range->css().get_accent_color().color;
        const ImU32 fill_color = ToImColor(accent);
        const ImU32 thumb_color = ToImColor(range->css().get_color());

        NativeControlClipScope scroll_clip(scroll_states_, range, document_origin, draw_list);
        draw_list->AddRectFilled(ImVec2(travel_left, center_y - track_radius),
                                 ImVec2(travel_left + travel, center_y + track_radius), track_color, track_radius);
        if (knob_x > travel_left) {
            draw_list->AddRectFilled(ImVec2(travel_left, center_y - track_radius),
                                     ImVec2(knob_x, center_y + track_radius),
                                     fill_color, track_radius);
        }
        draw_list->AddRectFilled(ImVec2(knob_x - knob_width * 0.5f, center_y - height * 0.5f),
                                 ImVec2(knob_x + knob_width * 0.5f, center_y + height * 0.5f),
                                 thumb_color, height * 0.5f);
    }
}

void HtmlDocument::MountLazyPanels() {
    if (!doc_ || !doc_->root()) return;

    bool mounted_panel = true;
    while (mounted_panel) {
        mounted_panel = false;
        const auto switchers = doc_->root()->select_all("[data-switcher]");
        const auto panels = doc_->root()->select_all("[role=tabpanel]");

        for (const auto& switcher : switchers) {
            const std::string group = switcher->get_attr("data-switcher", "");
            if (group.empty() || group == "main") continue;

            struct Candidate {
                std::shared_ptr<litehtml::element> element;
                std::string key;
            };
            std::vector<Candidate> owned_panels;
            for (const auto& panel : panels) {
                if (NearestSwitcher(panel) != switcher) continue;
                const std::string key = SwitchPanelKey(panel);
                if (!key.empty()) {
                    owned_panels.push_back({panel, key});
                }
            }
            if (owned_panels.empty()) continue;

            const std::string fallback = owned_panels.front().key;
        const std::string requested = state_value(group + "-selection", fallback);
            const auto selected_panel = std::ranges::find_if(
                owned_panels, [&](const Candidate& panel) { return SwitchPanelMatches(panel.element, requested); });
            const bool valid = selected_panel != owned_panels.end();
            const std::string selected = valid ? selected_panel->key : fallback;

            for (const Candidate& panel : owned_panels) {
                if (panel.key != selected || mounted_lazy_panels_.contains(panel.element.get())) {
                    continue;
                }
                const std::string fragment_id = panel.element->get_attr("id", "");
                const auto found = fragments_.find(fragment_id);
                if (fragment_id.empty() || found == fragments_.end() || !found->second) continue;

                const auto render_template = [this](const std::map<std::string, std::string>& attributes,
                                                    std::string_view children, const HtmlElementContext& context) {
                    return RenderListTemplate(attributes, children, context);
                };
                const std::string fragment_html = ExpandCustomElements(found->second(), render_template);
                doc_->append_children_from_string(*panel.element, fragment_html.c_str(), false);
                mounted_lazy_panels_.insert(panel.element.get());
                mounted_panel = true;
            }
        }
    }
}

std::shared_ptr<litehtml::element> HtmlDocument::NearestSwitcher(
    const std::shared_ptr<litehtml::element>& element) const {
    for (auto current = element; current; current = current->parent()) {
        if (std::string(current->get_attr("data-switcher", "")).empty()) continue;
        return current;
    }
    return {};
}

bool HtmlDocument::IsElementMounted(const std::shared_ptr<litehtml::element>& element) const {
    for (auto current = element; current; current = current->parent()) {
        if (std::string(current->get_attr("role", "")) != "tabpanel") continue;
        const auto owner = NearestSwitcher(current);
        if (!owner || std::string(owner->get_attr("data-switcher", "")) == "main") return true;
        return mounted_lazy_panels_.contains(current.get());
    }
    return true;
}

std::string HtmlDocument::SwitchControlTarget(const std::shared_ptr<litehtml::element>& element) const {
    const std::string href = element->get_attr("href", "");
    return href.starts_with("#") ? href.substr(1) : std::string();
}

std::string HtmlDocument::SwitchPanelKey(const std::shared_ptr<litehtml::element>& element) const {
    return element->get_attr("id", "");
}

bool HtmlDocument::SwitchPanelMatches(const std::shared_ptr<litehtml::element>& element,
                                     const std::string& requested) const {
    if (requested.empty()) return false;
    const std::string key = SwitchPanelKey(element);
    const std::string id = element->get_attr("id", "");
    if (key == requested || id == requested) return true;

    if (id.size() > requested.size()) {
        const std::string suffix = "-" + requested;
        return id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return false;
}

void HtmlDocument::ApplySwitchers() const {
    if (!doc_ || !doc_->root()) return;

    struct SwitchPanel {
        std::shared_ptr<litehtml::element> element;
        std::string key;
    };

    const auto switchers = doc_->root()->select_all("[data-switcher]");
    const auto panels = doc_->root()->select_all("[role=tabpanel]");
    const auto controls = doc_->root()->select_all("[href]");

    for (const auto& switcher : switchers) {
        const std::string group = switcher->get_attr("data-switcher", "");
        if (group.empty()) continue;

        std::vector<SwitchPanel> owned_panels;
        for (const auto& panel : panels) {
            if (NearestSwitcher(panel) != switcher) continue;
            const std::string key = SwitchPanelKey(panel);
            if (!key.empty()) {
                owned_panels.push_back({panel, key});
            }
        }
        if (owned_panels.empty()) {
            if (group != "main") continue;
            const std::string selected = current_fragment_;
            for (const auto& control : controls) {
                if (NearestSwitcher(control) != switcher) continue;
                const bool active = SwitchControlTarget(control) == selected;
                control->set_class("active", active);
                control->set_attr("aria-pressed", active ? "true" : "false");
            }
            continue;
        }

        const std::string fallback = owned_panels.front().key;
        const std::string requested = state_value(group + "-selection", fallback);
        const auto selected_panel = std::ranges::find_if(
            owned_panels, [&](const SwitchPanel& panel) { return SwitchPanelMatches(panel.element, requested); });
        const bool valid = selected_panel != owned_panels.end();
        const std::string selected = valid ? selected_panel->key : fallback;

        for (const SwitchPanel& panel : owned_panels) {
            const bool hidden = panel.key != selected;
            panel.element->set_class("is-hidden", hidden);
            panel.element->set_attr("aria-hidden", hidden ? "true" : "false");
        }
        const auto active_panel = std::ranges::find_if(
            owned_panels, [&](const SwitchPanel& panel) { return panel.key == selected; });
        for (const auto& control : controls) {
            if (NearestSwitcher(control) != switcher) continue;
            const std::string target = SwitchControlTarget(control);
            const bool active = target == selected ||
                                (active_panel != owned_panels.end() && target == active_panel->element->get_attr("id", ""));
            control->set_class("active", active);
            control->set_attr("aria-pressed", active ? "true" : "false");
            bool is_tab = false;
            for (auto ancestor = control->parent(); ancestor; ancestor = ancestor->parent()) {
                if (std::string(ancestor->get_attr("role", "")) == "tablist") {
                    is_tab = true;
                    break;
                }
            }
            if (is_tab) control->set_attr("aria-selected", active ? "true" : "false");
        }
    }

    // Switcher state changes classes and therefore invalidate cached selector
    // results, including selectors held by event dispatch registrations.
    element_cache_.clear();
}

bool HtmlDocument::SelectPanelById(const std::string& id) {
    if (id.empty()) return false;
    if (doc_ && doc_->root()) {
        if (const auto panel = doc_->root()->select_one("#" + id)) {
            const auto owner = NearestSwitcher(panel);
            if (owner) {
                const std::string group = owner->get_attr("data-switcher", "");
                if (!group.empty()) {
                    select(group, id);
                    return true;
                }
            }
        }
    }
    if (HasFragment(id)) {
        select("main", id);
        return true;
    }
    return false;
}

bool HtmlDocument::ToggleCheckbox(const std::string& id) {
    if (id.empty() || !doc_ || !doc_->root()) return false;
    const auto checkbox = doc_->root()->select_one("#" + id);
    if (!checkbox || checkbox->get_tagName() == nullptr || std::strcmp(checkbox->get_tagName(), "input") != 0 ||
        std::string(checkbox->get_attr("type", "")) != "checkbox") {
        return false;
    }

    const bool checked = std::string(checkbox->get_attr("data-checked", "")) == "true";
    const bool next = !checked;
    const std::string state = next ? "true" : "false";
    set_attribute("#" + id, "data-checked", state, ElementUpdateMode::Layout);
    set_attribute("#" + id, "aria-checked", state);
    set_state(id, next ? "true" : "false");
    dispatch_event(id, "click");
    dispatch_event(id, "change");
    return true;
}

void HtmlDocument::set_style(const std::string& selector, const std::string& css) {
    if (!doc_) {
        return;
    }
    const auto previous = inline_styles_.find(selector);
    if (previous != inline_styles_.end() && previous->second == css) {
        return;
    }
    if (auto target = FindElement(selector)) {
        const litehtml::style_display previous_display = target->css().get_display();
        target->set_attr("style", css.c_str());
        // Inline style changes can affect inherited values. Flush pending
        // selector updates, then recompute only this subtree.
        doc_->flush_updates();
        target->refresh_styles();
        target->compute_styles(true);
        inline_styles_[selector] = css;
        if (target->css().get_display() != previous_display) {
            // The render tree is display-dependent; a layout pass alone cannot
            // create or remove the corresponding render item.
            doc_->rebuild_render_tree();
        }
        MarkElementLayoutDirty(target);
    }
}

std::shared_ptr<litehtml::element> HtmlDocument::FindElement(const std::string& selector) {
    if (!doc_ || !doc_->root()) return {};
    const bool simple_id = selector.size() > 1 && selector.front() == '#' &&
                           selector.find_first_of(" .#[>+~:") == std::string::npos;
    auto& cache = simple_id ? id_cache_ : element_cache_;
    if (const auto found = cache.find(selector); found != cache.end()) {
        if (auto target = found->second.lock()) return target;
    }
    auto target = doc_->root()->select_one(selector);
    if (target) cache[selector] = target;
    return target;
}

void HtmlDocument::set_flex_pixels(const std::string& selector, long pixels) {
    const std::string css = "flex: 0 0 " + std::to_string(pixels) + "px;";
    set_style(selector, css);
}

void HtmlDocument::set_text(const std::string& selector, const std::string& text) {
    if (!doc_) return;
    const auto previous = element_texts_.find(selector);
    if (previous != element_texts_.end() && previous->second == text) return;

    if (auto target = FindElement(selector)) {
        if (SetTextNode(target, text)) {
            MarkElementLayoutDirty(target);
            element_texts_[selector] = text;
        }
    }
}

namespace {

std::string ElementRole(const std::shared_ptr<litehtml::element>& element) {
    return element ? std::string(element->get_attr("role", "")) : std::string();
}

std::shared_ptr<litehtml::element> NearestRole(std::shared_ptr<litehtml::element> element,
                                              const std::string& role) {
    for (; element; element = element->parent()) {
        if (ElementRole(element) == role) return element;
    }
    return nullptr;
}

void CollectOptions(const std::shared_ptr<litehtml::element>& element,
                    std::vector<std::shared_ptr<litehtml::element>>& out) {
    if (!element) return;
    for (const auto& child : element->children()) {
        if (!child) continue;
        if (ElementRole(child) == "option") {
            out.push_back(child);
            continue;
        }
        CollectOptions(child, out);
    }
}

}  // namespace

bool HtmlDocument::apply_selection(const std::string& id, const bool ctrl_held,
                                      const bool shift_held) {
    if (!doc_ || id.empty()) return false;

    const auto clicked = FindElement("#" + id);
    if (clicked) {
        const char* tag = clicked->get_tagName();
        const std::string name = tag != nullptr ? tag : "";
        if (name == "input" || name == "select" || name == "textarea") return false;
    }
    const auto option = NearestRole(clicked, "option");
    if (!option) return false;
    if (std::string(option->get_attr("data-selectable", "true")) == "false") return false;
    const auto listbox = NearestRole(option->parent(), "listbox");
    if (!listbox) return false;

    std::vector<std::shared_ptr<litehtml::element>> options;
    CollectOptions(listbox, options);
    if (options.empty()) return false;

    const auto clicked_it = std::ranges::find(options, option);
    if (clicked_it == options.end()) return false;
    const auto clicked_index = static_cast<size_t>(std::distance(options.begin(), clicked_it));

    const bool multi = std::string(listbox->get_attr("aria-multiselectable", "false")) == "true";
    const std::string listbox_id = listbox->get_attr("id", "");

    std::vector<bool> selected(options.size(), false);
    for (size_t i = 0; i < options.size(); ++i) {
        selected[i] = std::string(options[i]->get_attr("aria-selected", "false")) == "true";
    }

    if (!multi || (!ctrl_held && !shift_held)) {
        std::ranges::fill(selected, false);
        selected[clicked_index] = true;
        aria_selection_anchors_[listbox_id] = clicked_index;
    } else if (ctrl_held) {
        selected[clicked_index] = !selected[clicked_index];
        aria_selection_anchors_[listbox_id] = clicked_index;
    } else {
        const auto found = aria_selection_anchors_.find(listbox_id);
        const size_t anchor = found != aria_selection_anchors_.end() &&
                                      found->second < options.size()
                                  ? found->second
                                  : clicked_index;
        const size_t low = std::min(anchor, clicked_index);
        const size_t high = std::max(anchor, clicked_index);
        std::ranges::fill(selected, false);
        for (size_t i = low; i <= high; ++i) selected[i] = true;
        if (found == aria_selection_anchors_.end()) aria_selection_anchors_[listbox_id] = anchor;
    }

    auto& store = aria_selections_[listbox_id];
    const bool replaces_selection = !(multi && ctrl_held);
    if (replaces_selection) store.clear();
    bool changed = false;
    for (size_t i = 0; i < options.size(); ++i) {
        const std::string option_id = options[i]->get_attr("id", "");
        if (!option_id.empty()) {
            if (selected[i]) {
                store.insert(option_id);
            } else if (!replaces_selection) {
                store.erase(option_id);
            }
        }

        const char* current = options[i]->get_attr("aria-selected", "false");
        const bool was = current != nullptr && std::string(current) == "true";
        if (was == selected[i]) continue;
        options[i]->set_attr("aria-selected", selected[i] ? "true" : "false");
        options[i]->set_class("selected", selected[i]);
        changed = true;
    }
    if (changed) {
        element_cache_.clear();
        doc_->refresh_styles();
    }
    return true;
}

void HtmlDocument::ApplyStoredAriaSelection() const {
    if (!doc_ || !doc_->root() || aria_selections_.empty()) return;
    bool changed = false;
    for (const auto& [listbox_id, ids] : aria_selections_) {
        const auto listbox = doc_->root()->select_one("#" + listbox_id);
        if (!listbox) continue;
        std::vector<std::shared_ptr<litehtml::element>> options;
        CollectOptions(listbox, options);
        for (const auto& option : options) {
            const std::string option_id = option->get_attr("id", "");
            const bool selected = !option_id.empty() && ids.contains(option_id);
            const char* current = option->get_attr("aria-selected", "false");
            const bool was = current != nullptr && std::string(current) == "true";
            if (was == selected) continue;
            option->set_attr("aria-selected", selected ? "true" : "false");
            option->set_class("selected", selected);
            changed = true;
        }
    }
    if (changed) {
        element_cache_.clear();
        doc_->refresh_styles();
    }
}

std::vector<std::string> HtmlDocument::selected_ids(const std::string& listbox_id) {
    std::vector<std::string> ids;
    const auto found = aria_selections_.find(listbox_id);
    if (found == aria_selections_.end()) return ids;
    ids.assign(found->second.begin(), found->second.end());
    return ids;
}

void HtmlDocument::set_attribute(const std::string& selector, const std::string& name,
                                    const std::string& value, ElementUpdateMode mode) {
    if (!doc_ || name.empty()) return;
    if (auto target = FindElement(selector)) {
        const char* current = target->get_attr(name.c_str(), nullptr);
        if (current != nullptr && value == current) return;
        target->set_attr(name.c_str(), value.c_str());
        const bool selector_sensitive = name == "class" || name == "id" || name == "style" ||
                                        doc_->attribute_affects_styles(name.c_str());
        if (selector_sensitive) {
            element_cache_.clear();
            if (name == "id") id_cache_.clear();
            doc_->refresh_styles();
        }
        if (mode == ElementUpdateMode::Layout) {
            MarkElementLayoutDirty(target);
        }
    }
}

void HtmlDocument::frame() {
    ++frame_count_;
    const auto frame_start = std::chrono::steady_clock::now();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("HTML document", nullptr, flags);

    if (!pending_fragment_.empty()) {
        current_fragment_ = std::move(pending_fragment_);
        set_state("main-selection", current_fragment_);
    }

    ApplyPendingListUpdates();
    const auto after_list_updates = std::chrono::steady_clock::now();

    if (needs_rebuild_) {
        needs_rebuild_ = false;
        RebuildDocument();
    }

    // Synchronization and user callbacks commonly issue several attribute
    // mutations. Defer their stylesheet walk until immediately before layout.
    doc_->begin_update();

    if (before_render) {
        before_render();
    }

    if (application_) {
        for (const auto& control : html_controls_) {
            if (control) control->before_render(*this, *application_);
        }
    }

    if (!control_cache_valid_) RefreshControlCaches();
    SyncTextInputs();
    SyncSelects();
    SyncRanges();
    doc_->end_update();
    const auto after_sync = std::chrono::steady_clock::now();
    const ImVec2 document_origin = ImGui::GetCursorScreenPos();
    std::string clicked_url;
    const bool clicked = RenderDocument("app", 0.0f, &clicked_url, &scroll_states_);
    if (clicked && !clicked_url.empty()) {
        handle_interaction(clicked_url);
        // A click handler may append or replace controls. Keep the overlay
        // cache and scroll snapshot coherent for this same frame.
        if (!control_cache_valid_) RefreshControlCaches();
        if (doc_ && doc_->root()) {
            scroll_states_.clear();
            CollectScrollStates(doc_, scroll_states_);
        }
    }
    const auto after_document = std::chrono::steady_clock::now();
    // RenderDocument has already applied scrolling and has the authoritative
    // post-scroll geometry. Share one snapshot with all native overlays;
    // otherwise every input/select/range would walk the complete render tree
    // independently just to reconstruct the same ancestor clips.
    DrawTextInputs(document_origin);
    UpdateSelectFromMouse(document_origin);
    UpdateRangeFromMouse(document_origin);
    DrawSelects(document_origin);
    DrawRanges(document_origin);
    if (after_render) {
        after_render();
    }
    DrawSelects(document_origin, true);

    UpdateListWindows();
    const auto after_overlays = std::chrono::steady_clock::now();

    if (telemetry_enabled() && frame_count_ % 120 == 0) {
        std::fprintf(stderr,
                     "[ImHTML telemetry] frames=%llu rebuilds=%llu lists=%llu inactive-lists=%llu "
                     "paint-only-lists=%llu in-place-lists=%llu\n",
                     static_cast<unsigned long long>(frame_count_),
                     static_cast<unsigned long long>(rebuild_count_),
                     static_cast<unsigned long long>(list_update_count_),
                     static_cast<unsigned long long>(inactive_list_update_count_),
                     static_cast<unsigned long long>(paint_only_list_update_count_),
                     static_cast<unsigned long long>(in_place_list_update_count_));
    }
    if (timing_enabled() && frame_count_ % 120 == 0) {
        const auto micros = [](const auto begin, const auto end) {
            return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
        };
        std::fprintf(stderr,
                     "[ImHTML timings] frame=%llu list=%lld sync=%lld document=%lld overlays=%lld total=%lld "
                     "controls(text=%zu checkbox=%zu select=%zu range=%zu) scroll=%zu\n",
                     static_cast<unsigned long long>(frame_count_),
                     static_cast<long long>(micros(frame_start, after_list_updates)),
                     static_cast<long long>(micros(after_list_updates, after_sync)),
                     static_cast<long long>(micros(after_sync, after_document)),
                     static_cast<long long>(micros(after_document, after_overlays)),
                     static_cast<long long>(micros(frame_start, after_overlays)),
                     text_inputs_.size(), checkbox_inputs_.size(), select_elements_.size(), range_inputs_.size(),
                     scroll_states_.size());
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void HtmlDocument::shutdown() {
    doc_.reset();
    content_slot_.reset();
}

}  // namespace ImHTML
