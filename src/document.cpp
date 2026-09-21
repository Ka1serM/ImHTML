#include "imgui_internal.h"

#include <imhtml/document.hpp>

#include "internal/core_internal.hpp"

#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>

#include <imhtml/platform.hpp>
#include <imhtml/theme.hpp>
#include "litehtml/document.h"
#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace ImHTML {

std::string Event::attribute(const std::string& name) const {
    if (!target_element_ || name.empty()) return {};
    return target_element_->get_attr(name.c_str(), "");
}

Element Event::closest(const std::string& selector) const {
    if (application_ == nullptr || selector.empty()) return {};
    const auto& matches = application_->CachedSelectorAll(selector);
    for (auto current = target_element_; current; current = current->parent()) {
        if (std::ranges::find(matches, current) != matches.end()) {
            return Element(current, application_);
        }
    }
    return {};
}


namespace {
bool InTemplateContents(const std::shared_ptr<litehtml::element>& element,
                        const std::shared_ptr<litehtml::element>& scope = {}) {
    for (auto parent = element ? element->parent() : nullptr; parent && parent != scope; parent = parent->parent())
        if (std::string_view(parent->get_tagName()) == "template") return true;
    return false;
}

bool telemetry_enabled() {
    static const bool enabled = std::getenv("IMHTML_TELEMETRY") != nullptr;
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
    if (Platform::OpenURL(browser_url)) {
        return true;
    }
    LogPrintf(LogLevel::Warning, "failed to open external URL: %s", browser_url.c_str());
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

bool IsLogarithmicRange(const std::shared_ptr<litehtml::element>& range) {
    return range && std::strcmp(range->get_attr("data-scale", ""), "log") == 0;
}

double RangeValueAt(const std::shared_ptr<litehtml::element>& range,
                    double normalized, double minimum, double maximum) {
    normalized = std::clamp(normalized, 0.0, 1.0);
    if (IsLogarithmicRange(range) && minimum > 0.0 && maximum > minimum) {
        return std::exp(std::log(minimum) + normalized * (std::log(maximum) - std::log(minimum)));
    }
    return minimum + (maximum - minimum) * normalized;
}

double RangePositionOf(const std::shared_ptr<litehtml::element>& range,
                       double value, double minimum, double maximum) {
    if (maximum <= minimum) return 0.0;
    value = std::clamp(value, minimum, maximum);
    if (IsLogarithmicRange(range) && minimum > 0.0) {
        return std::clamp((std::log(value) - std::log(minimum)) /
                              (std::log(maximum) - std::log(minimum)),
                          0.0, 1.0);
    }
    return std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
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

bool IsNumberInput(const std::shared_ptr<litehtml::element>& element) {
    return element && std::strcmp(element->get_tagName(), "input") == 0 &&
           std::strcmp(element->get_attr("type", ""), "number") == 0;
}

bool IsTextInput(const std::shared_ptr<litehtml::element>& element) {
    return element && std::strcmp(element->get_tagName(), "input") == 0 &&
           (std::strcmp(element->get_attr("type", ""), "text") == 0 || IsNumberInput(element));
}

double NumberStep(const std::shared_ptr<litehtml::element>& input) {
    const char* step = input ? input->get_attr("step", "1") : "1";
    if (step == nullptr || std::strcmp(step, "any") == 0) return 1.0;
    const double parsed = ParseRangeNumber(step, 1.0);
    return parsed > 0.0 ? parsed : 1.0;
}

double NumberBound(const std::shared_ptr<litehtml::element>& input, const char* attribute,
                  const double fallback) {
    return ParseRangeNumber(input ? input->get_attr(attribute, "") : "", fallback);
}

std::string SerializeNumberValue(const double value) {
    const double normalized = std::abs(value) < 0.0000005 ? 0.0 : value;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << normalized;
    return stream.str();
}

float NumberSpinnerWidth(const litehtml::position& box) {
    return std::clamp(static_cast<float>(box.height) * 0.50f, 14.0f, 18.0f);
}

bool IsColorInput(const std::shared_ptr<litehtml::element>& element) {
    return element && std::strcmp(element->get_tagName(), "input") == 0 &&
           std::strcmp(element->get_attr("type", ""), "color") == 0;
}

std::shared_ptr<litehtml::element> ColorAncestor(std::shared_ptr<litehtml::element> element) {
    for (; element; element = element->parent()) {
        if (IsColorInput(element)) return element;
    }
    return nullptr;
}

int HexDigit(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::array<float, 4> ParseHtmlColor(const std::string& value) {
    std::array<float, 4> result{0.0f, 0.0f, 0.0f, 1.0f};
    if (value.size() != 7 || value.front() != '#') return result;
    for (int channel = 0; channel < 3; ++channel) {
        const int high = HexDigit(value[1 + channel * 2]);
        const int low = HexDigit(value[2 + channel * 2]);
        if (high < 0 || low < 0) return result;
        result[channel] = static_cast<float>((high << 4) | low) / 255.0f;
    }
    return result;
}

std::string SerializeHtmlColor(const float color[3]) {
    char result[8]{};
    std::snprintf(result, sizeof(result), "#%02x%02x%02x",
                  static_cast<unsigned int>(std::lround(ImClamp(color[0], 0.0f, 1.0f) * 255.0f)),
                  static_cast<unsigned int>(std::lround(ImClamp(color[1], 0.0f, 1.0f) * 255.0f)),
                  static_cast<unsigned int>(std::lround(ImClamp(color[2], 0.0f, 1.0f) * 255.0f)));
    return result;
}

std::string ColorPopupId(const std::string& id) {
    return "html-color-picker-" + id;
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
    NativeControlClipScope(const std::shared_ptr<litehtml::document>& document,
                           const std::shared_ptr<litehtml::element>& element,
                           const ImVec2& document_origin, ImDrawList* draw_list)
        : draw_list_(draw_list) {
        if (!document || !element || !draw_list_) return;

        for (const auto& state : FrameScrollStates("app")) {
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
    const auto children = select->get_attr("data-editable-input", "")[0] != '\0'
                              ? select->select_all("option") : select->children();
    for (const auto& child : children) {
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

void HtmlDocument::set_stylesheet_provider(std::function<std::string()> provider) {
    stylesheet_provider_ = std::move(provider);
    document_recreate_required_ = true;
    RequestStructuralRebuild();
}

void HtmlDocument::register_shell(std::string html) {
    if (shell_html_ == html) return;
    shell_html_ = std::move(html);
    document_recreate_required_ = true;
    RequestStructuralRebuild();
}

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
    event_match_cache_.erase(listener_id);
    std::erase_if(event_listeners_, [listener_id](const EventListenerRegistration& registration) {
        return registration.id == listener_id;
    });
}

bool HtmlDocument::initialize(HtmlWindow* application) {
    // Registrations made during initialization belong to this document's
    // context, not to whichever one happens to be current.
    const ContextScope scope(*context_);
    application_ = application;
    return !shell_html_.empty();
}

void HtmlDocument::InvalidateDomCaches() {
    ++dom_generation_;
    element_cache_.clear();
    InvalidateSelectorCaches();
    switcher_owner_cache_.clear();
    switcher_index_generation_ = 0;
    switcher_index_.clear();
}

void HtmlDocument::InvalidateSelectorCaches() {
    selector_cache_.clear();
    event_match_cache_.clear();
    ++selector_generation_;
}

void HtmlDocument::Invalidate(const DirtyState state) {
    dirty_state_ = static_cast<DirtyState>(static_cast<std::uint8_t>(dirty_state_) |
                                            static_cast<std::uint8_t>(state));
}

const std::vector<std::shared_ptr<litehtml::element>>& HtmlDocument::CachedSelectorAll(
    const std::string& selector) const {
    static const std::vector<std::shared_ptr<litehtml::element>> empty;
    if (!doc_ || !doc_->root() || selector.empty()) return empty;

    const auto found = selector_cache_.find(selector);
    if (found != selector_cache_.end()) return found->second;

    ++performance_stats_.selector_scans;
    const auto elements = doc_->root()->select_all(selector.c_str());
    auto [inserted, _] = selector_cache_.emplace(selector,
                                                  std::vector<std::shared_ptr<litehtml::element>>{});
    inserted->second.assign(elements.begin(), elements.end());
    std::erase_if(inserted->second, [](const auto& element) { return InTemplateContents(element); });
    return inserted->second;
}

const std::vector<HtmlDocument::SwitcherIndexEntry>& HtmlDocument::CachedSwitcherIndex() const {
    if (!doc_ || !doc_->root()) {
        switcher_index_.clear();
        switcher_index_generation_ = dom_generation_;
        return switcher_index_;
    }
    if (switcher_index_generation_ == dom_generation_) return switcher_index_;

    switcher_index_.clear();
    const auto switchers = doc_->root()->select_all("[data-switcher]");
    const auto panels = doc_->root()->select_all("[role=tabpanel]");
    const auto controls = doc_->root()->select_all("[href]");

    std::unordered_map<const litehtml::element*, std::size_t> entries;
    entries.reserve(switchers.size());
    for (const auto& switcher : switchers) {
        const std::string group = switcher->get_attr("data-switcher", "");
        if (group.empty()) continue;
        entries.emplace(switcher.get(), switcher_index_.size());
        switcher_index_.push_back({switcher, group, {}, {}});
    }

    for (const auto& panel : panels) {
        const auto owner = NearestSwitcher(panel);
        const auto found = entries.find(owner.get());
        if (found != entries.end()) switcher_index_[found->second].panels.push_back(panel);
    }
    for (const auto& control : controls) {
        const auto owner = NearestSwitcher(control);
        const auto found = entries.find(owner.get());
        if (found == entries.end()) continue;
        // Nested switchers only own explicit tab controls. Arbitrary links
        // inside a mounted panel must not participate in tab reconciliation.
        const bool is_tab = std::string(control->get_attr("role", "")) == "tab";
        if (switcher_index_[found->second].group == "main" || is_tab) {
            switcher_index_[found->second].controls.push_back(control);
        }
    }
    switcher_index_generation_ = dom_generation_;
    return switcher_index_;
}

void HtmlDocument::RefreshStylesIfNeeded() {
    if (!styles_dirty_ || !doc_) return;
    doc_->refresh_styles();
    styles_dirty_ = false;
    ++performance_stats_.style_refreshes;
}

void HtmlDocument::RefreshElementStyles(
    std::span<const std::shared_ptr<litehtml::element>> elements) {
    if (styles_dirty_) return;
    std::unordered_set<const litehtml::element*> refreshed;
    for (const auto& element : elements) {
        if (!element || !refreshed.insert(element.get()).second) continue;
        doc_->refresh_element_styles(*element);
    }
}

Element::Element() = default;
struct Element::Node {
    HtmlDocument* owner = nullptr;
    std::string tag;
    std::map<std::string, std::string> attributes;
    std::string text;
    std::vector<Element> children;
    std::string raw_html;
    std::shared_ptr<litehtml::element> materialized;
    bool fragment = false;
};

Element::Element(std::shared_ptr<litehtml::element> element, HtmlDocument* owner)
    : element_(std::move(element)), owner_(owner) {}
Element::Element(std::shared_ptr<Node> node) : node_(std::move(node)) {}
Element::Element(const Element& other) = default;
Element::Element(Element&& other) noexcept = default;
Element& Element::operator=(const Element& other) = default;
Element& Element::operator=(Element&& other) noexcept = default;
Element::~Element() = default;

const std::shared_ptr<litehtml::element>& RawElement(const Element& element) { return element.element_; }

namespace {

bool IsVoidHtmlTag(const std::string_view tag) {
    return tag == "area" || tag == "base" || tag == "br" || tag == "col" || tag == "embed" ||
           tag == "hr" || tag == "img" || tag == "input" || tag == "link" || tag == "meta" ||
           tag == "param" || tag == "source" || tag == "track" || tag == "wbr";
}

std::string SerializeElement(const std::shared_ptr<litehtml::element>& element);

std::string SerializeChildren(const std::shared_ptr<litehtml::element>& element) {
    if (!element) return {};
    std::string result;
    for (const auto& child : element->children()) result += SerializeElement(child);
    return result;
}

std::string SerializeElement(const std::shared_ptr<litehtml::element>& element) {
    if (!element) return {};
    if (element->is_text()) {
        std::string text;
        element->get_text(text);
        return EscapeHtml(text);
    }

    const char* tag_name = element->get_tagName();
    if (tag_name == nullptr || *tag_name == '\0') return SerializeChildren(element);

    std::string result = "<" + std::string(tag_name);
    for (const auto& [name, value] : element->attributes())
        result += " " + name + "=\"" + EscapeHtml(value) + "\"";
    result += ">";
    if (!IsVoidHtmlTag(tag_name)) result += SerializeChildren(element) + "</" + std::string(tag_name) + ">";
    return result;
}

}  // namespace

std::string Element::tag() const {
    if (node_) return node_->tag;
    if (!element_) return {};
    const char* name = element_->get_tagName();
    return name == nullptr ? std::string() : std::string(name);
}

std::string Element::attribute(const std::string& name, const std::string& fallback) const {
    if (node_) {
        const auto found = node_->attributes.find(name);
        return found == node_->attributes.end() ? fallback : found->second;
    }
    if (!element_ || name.empty()) return fallback;
    const char* value = element_->get_attr(name.c_str(), nullptr);
    return value == nullptr ? fallback : std::string(value);
}

Element Element::query_selector(const std::string& selector) const {
    const auto matches = query_selector_all(selector);
    return matches.empty() ? Element{} : matches.front();
}

std::vector<Element> Element::query_selector_all(const std::string& selector) const {
    std::vector<Element> matches;
    if (selector.empty()) return matches;
    if (node_) {
        if (!node_->materialized && node_->owner) node_->materialized = node_->owner->MaterializeFragment(node_->raw_html);
        if (!node_->materialized) return matches;
        for (const auto& match : node_->materialized->select_all(selector))
            if (!InTemplateContents(match, node_->materialized)) matches.emplace_back(Element(match, node_->owner));
        return matches;
    }
    if (!element_ || tag() == "template") return matches;
    for (const auto& match : element_->select_all(selector))
        if (!InTemplateContents(match, element_)) matches.emplace_back(Element(match, owner_));
    return matches;
}

Element Element::parent_element() const {
    if (!element_) return {};
    auto parent = element_->parent();
    return parent ? Element(std::move(parent), owner_) : Element{};
}

Rect Element::get_bounding_client_rect() const {
    if (!element_ || owner_ == nullptr || !element_->get_render_item()) return Rect();
    // Unlike ElementBox, a collapsed (zero-sized) box is still a valid rect.
    std::optional<litehtml::position> first;
    element_->get_render_item()->get_rendering_boxes([&](const litehtml::position& box) {
        if (!first) first = box;
    });
    if (!first) return Rect();
    const ImVec2 origin = owner_->document_origin_;
    const ImVec2 min(origin.x + static_cast<float>(first->x), origin.y + static_cast<float>(first->y));
    return Rect(min, ImVec2(min.x + static_cast<float>(first->width), min.y + static_cast<float>(first->height)));
}

Element Element::content() const {
    if (!element_ || tag() != "template" || owner_ == nullptr) return {};
    auto node = std::make_shared<Node>();
    node->owner = owner_;
    node->fragment = true;
    node->materialized = element_;
    return Element(std::move(node));
}

Element Element::clone_node(const bool deep) const {
    if (node_) {
        auto clone = std::make_shared<Node>(*node_);
        if (node_->materialized) clone->raw_html = SerializeChildren(node_->materialized);
        clone->materialized.reset();
        if (!deep) { clone->raw_html.clear(); clone->children.clear(); clone->text.clear(); }
        return Element(std::move(clone));
    }
    if (!element_ || owner_ == nullptr) return {};
    const auto root = owner_->MaterializeFragment(SerializeElement(element_));
    if (!root || root->children().empty()) return {};
    auto clone = root->children().front();
    root->removeChild(clone);
    Element result(clone, owner_);
    if (!deep) result.replace_children();
    return result;
}

std::string Element::text_content() const {
    if (node_) {
        if (node_->materialized) {
            std::string result;
            node_->materialized->get_text(result);
            return result;
        }
        return node_->text;
    }
    if (!element_) return {};
    std::string result;
    element_->get_text(result);
    return result;
}

Element& Element::set_attribute(std::string name, std::string value) {
    if (name.empty()) return *this;
    if (node_) {
        if (node_->materialized) node_->materialized->set_attr(name.c_str(), value.c_str());
        else node_->attributes[std::move(name)] = std::move(value);
    } else if (element_) {
        if (attribute(name) == value) return *this;
        element_->set_attr(name.c_str(), value.c_str());
        element_->parse_attributes();
        if (owner_ && owner_->IsElementMounted(element_)) {
            owner_->InvalidateDomCaches();
            owner_->styles_dirty_ = true;
            owner_->Invalidate(HtmlDocument::DirtyState::Layout);
            MarkDocumentLayoutDirty("app");
        }
    }
    return *this;
}

Element& Element::set_text_content(std::string text) {
    if (node_) {
        if (node_->materialized) {
            node_->owner->MutateElement(node_->materialized, EscapeHtml(text), true);
        } else {
            node_->text = std::move(text);
            node_->raw_html.clear();
            node_->children.clear();
        }
    } else if (element_ && owner_) {
        owner_->MutateElement(element_, EscapeHtml(text), true);
    }
    return *this;
}

std::string Element::html() const {
    if (element_) return SerializeElement(element_);
    if (node_ && node_->materialized) return SerializeChildren(node_->materialized);
    if (node_ && !node_->raw_html.empty()) return node_->raw_html;
    if (!node_ || node_->tag.empty()) return {};
    std::string result = "<" + node_->tag;
    for (const auto& [name, value] : node_->attributes)
        result += " " + name + "=\"" + EscapeHtml(value) + "\"";
    result += ">" + EscapeHtml(node_->text);
    for (const Element& child : node_->children) result += child.html();
    result += "</" + node_->tag + ">";
    return result;
}

Element& Element::append(Element child) {
    auto* owner = node_ ? node_->owner : owner_;
    if (!owner || !child) return *this;
    if (node_ && !node_->materialized) node_->materialized = owner->MaterializeFragment(node_->raw_html);
    auto target = node_ ? node_->materialized : element_;
    if (!target) return *this;
    std::vector<std::shared_ptr<litehtml::element>> children;
    if (child.node_) {
        if (!child.node_->materialized)
            child.node_->materialized = child.node_->owner->MaterializeFragment(child.node_->raw_html);
        if (child.node_->materialized)
            children.assign(child.node_->materialized->children().begin(), child.node_->materialized->children().end());
    } else children.push_back(child.element_);
    for (const auto& element : children) {
        // Insertion moves nodes, never reparses or duplicates them.
        for (auto ancestor = target; ancestor; ancestor = ancestor->parent())
            if (ancestor == element) throw std::invalid_argument("Cannot insert an ancestor into its descendant");
        if (auto parent = element->parent()) {
            parent->removeChild(element);
            owner->DomChanged(parent);
        }
        target->appendChild(element);
    }
    owner->DomChanged(target);
    return *this;
}

Element& Element::append_child(Element child) { return append(std::move(child)); }

Element& Element::replace_children() {
    auto* owner = node_ ? node_->owner : owner_;
    if (!owner) return *this;
    if (node_ && !node_->materialized) node_->materialized = owner->MaterializeFragment(node_->raw_html);
    auto target = node_ ? node_->materialized : element_;
    if (!target) return *this;
    while (!target->children().empty()) target->removeChild(target->children().front());
    owner->DomChanged(target);
    return *this;
}

Element& Element::replace_children(Element child) {
    replace_children();
    return append_child(std::move(child));
}

void Element::remove() {
    if (!element_ || !owner_) return;
    if (auto parent = element_->parent()) {
        parent->removeChild(element_);
        owner_->DomChanged(parent);
    }
}

std::vector<Element> HtmlDocument::query_selector_all(const std::string& selector) const {
    const auto& elements = CachedSelectorAll(selector);
    std::vector<Element> handles;
    handles.reserve(elements.size());
    for (const auto& element : elements) handles.emplace_back(Element(element, const_cast<HtmlDocument*>(this)));
    return handles;
}

Element HtmlDocument::query_selector(const std::string& selector) const {
    return Element(SelectOne(selector), const_cast<HtmlDocument*>(this));
}

Element HtmlDocument::create_element(std::string tag) {
    if (!doc_) return {};
    return Element(doc_->create_element(tag.c_str(), {}), this);
}

Element HtmlDocument::create_document_fragment() {
    auto node = std::make_shared<Element::Node>();
    node->owner = this;
    node->fragment = true;
    return Element(std::move(node));
}

std::shared_ptr<litehtml::element> HtmlDocument::SelectOne(const std::string& selector) const {
    const auto& elements = CachedSelectorAll(selector);
    return elements.empty() ? std::shared_ptr<litehtml::element>{} : elements.front();
}

std::shared_ptr<litehtml::element> HtmlDocument::MaterializeFragment(const std::string_view html) const {
    if (!doc_) return {};
    static const litehtml::string_map empty_attributes;
    auto root = doc_->create_element("div", empty_attributes);
    if (!root) return {};
    doc_->append_children_from_string(*root, std::string(html).c_str(), true);
    return root;
}

void HtmlDocument::MutateElement(const std::shared_ptr<litehtml::element>& target,
                                 const std::string_view html, const bool replace_existing) {
    if (!doc_ || !target) return;
    doc_->append_children_from_string(*target, std::string(html).c_str(), replace_existing);
    DomChanged(target);
}

void HtmlDocument::DomChanged(const std::shared_ptr<litehtml::element>& target) {
    if (!IsElementMounted(target)) return;
    dom_render_tree_dirty_ = true;
    InvalidateDomCaches();
    styles_dirty_ = true;
    Invalidate(DirtyState::Structure);
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
        if (IsTextInput(element)) {
            auto& state = text_states_[key];
            // A host commonly synchronizes model values in before_render().
            // Do not replace an in-progress lexical edit (notably "-" or
            // "1.") with the model's normalized value on the next frame.
            // The supplied value remains stored and is reconciled after blur.
            if (active_text_id_ == key && state.value != value) return;
            if (state.value != value) {
                state.value = value;
                state.cursor = state.anchor = value.size();
                state.scroll_x = 0.0f;
            }
        }
        if (std::strcmp(element->get_attr("data-value", ""), value.c_str()) != 0) {
            element->set_attr("data-value", value.c_str());
        }
    }
}

void HtmlDocument::set_checked(const std::string& id, const bool checked_value) {
    if (id.empty()) return;
    const std::string key = id[0] == '#' ? id.substr(1) : id;
    values_[key] = checked_value ? "true" : "false";
    if (const auto element = FindElement("#" + key)) {
        const char* state = checked_value ? "true" : "false";
        const bool style_state_changed = std::strcmp(element->get_attr("data-checked", ""), state) != 0 ||
                                         std::strcmp(element->get_attr("aria-checked", ""), state) != 0;
        element->set_attr("data-checked", state);
        element->set_attr("aria-checked", state);
        const bool pseudo_state_changed =
            element->set_pseudo_class(litehtml::_id("checked"), checked_value) |
            element->set_pseudo_class(litehtml::_id("unchecked"), !checked_value);
        if (style_state_changed || pseudo_state_changed) {
            InvalidateSelectorCaches();
            // Dynamic pseudo-classes participate in normal selector matching,
            // so refresh this element before the next layout/paint.
            doc_->refresh_element_styles(*element);
            Invalidate(DirtyState::Layout);
            MarkDocumentLayoutDirty("app");
        }
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
    const auto element = SelectOne(selector);
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
    const auto element = SelectOne(selector);
    if (element) {
        const char* state = element->get_attr("data-checked", nullptr);
        if (state == nullptr) state = element->get_attr("checked", nullptr);
        if (state != nullptr) return std::string(state) != "false";
    }
    return value(selector) == "true";
}

std::string HtmlDocument::state_value(const std::string& key, const std::string& fallback) const {
    if (!has_value(key)) return fallback;
    return value(key);
}

void HtmlDocument::set_state(const std::string& key, const std::string& value) {
    set_value(key, value);
}

bool HtmlDocument::dispatch_event(const std::string& id, const std::string& event,
                                     const std::string& key) {
    if (id.empty() || event.empty() || !doc_ || !doc_->root()) return false;
    const auto target = FindElement("#" + id);
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
            auto& match_cache = event_match_cache_[registration.id];
            if (match_cache.generation != selector_generation_) {
                match_cache.elements.clear();
                for (const auto& element : CachedSelectorAll(registration.selector)) {
                    match_cache.elements.insert(element.get());
                }
                match_cache.generation = selector_generation_;
            }
            if (!match_cache.elements.contains(current.get())) continue;
            dom_event.current_target_id_ = current->get_attr("id", "");
            registration.listener(dom_event);
            dispatched = true;
            if (dom_event.propagation_stopped()) break;
        }
    }
    return dispatched;
}

void HtmlDocument::DispatchPointerEvents(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const ImVec2 movement = io.MouseDelta;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pointer_capture_selector_.empty()) {
        for (const auto& registration : event_listeners_) {
            if (registration.event != "mousedown" || !registration.listener) continue;
            bool matched = false;
            for (const auto& element : CachedSelectorAll(registration.selector)) {
                litehtml::position box;
                if (!ElementBox(element, box)) continue;
                const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                                 document_origin.y + static_cast<float>(box.y));
                const ImVec2 max(min.x + static_cast<float>(box.width),
                                 min.y + static_cast<float>(box.height));
                if (mouse.x < min.x || mouse.x >= max.x || mouse.y < min.y || mouse.y >= max.y) continue;

                pointer_capture_selector_ = registration.selector;
                pointer_capture_id_ = element->get_attr("id", "");
                Event dom_event;
                dom_event.application_ = this;
                dom_event.target_element_ = element;
                dom_event.type_ = "mousedown";
                dom_event.target_id_ = pointer_capture_id_;
                dom_event.current_target_id_ = dom_event.target_id_;
                dom_event.client_x_ = mouse.x;
                dom_event.client_y_ = mouse.y;
                registration.listener(dom_event);
                matched = true;
                break;
            }
            if (matched) break;
        }
    }

    if (pointer_capture_selector_.empty()) return;

    const bool moved = movement.x != 0.0f || movement.y != 0.0f;
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (!moved && !released) return;

    const auto listeners = event_listeners_;
    for (const auto& registration : listeners) {
        if (registration.selector != pointer_capture_selector_ || !registration.listener) continue;
        if (registration.event != "mousemove" && registration.event != "mouseup") continue;
        if (registration.event == "mousemove" && !moved) continue;
        if (registration.event == "mouseup" && !released) continue;

        Event dom_event;
        dom_event.application_ = this;
        dom_event.type_ = registration.event;
        dom_event.target_id_ = pointer_capture_id_;
        dom_event.current_target_id_ = pointer_capture_id_;
        dom_event.client_x_ = mouse.x;
        dom_event.client_y_ = mouse.y;
        dom_event.movement_x_ = movement.x;
        dom_event.movement_y_ = movement.y;
        registration.listener(dom_event);
    }

    if (released) {
        pointer_capture_selector_.clear();
        pointer_capture_id_.clear();
    }
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

void HtmlDocument::RequestStructuralRebuild() {
    needs_rebuild_ = true;
    Invalidate(DirtyState::Structure);
}

void HtmlDocument::RebuildRenderTreeForTabSwitch(const bool structure_changed) {
    if (!doc_) return;

    // Persistent DOM does not imply persistent render geometry. Replacing the
    // tree on every nested transition prevents a previously loaded panel from
    // retaining flex placement or hit-test data after it becomes hidden.
    std::vector<ScrollState> scroll_states;
    CollectScrollStates(doc_, scroll_states);
    for (const ScrollState& state : scroll_states) {
        if (!state.target) continue;
        pending_scroll_offsets_.push_back({state.target, state.left, state.top});
    }

    // append_children_from_string can attach simple block children
    // incrementally, but a lazy fragment can also introduce anonymous inline,
    // flex, table and pseudo-element render nodes. Rebuild once on first mount
    // so the next paint cannot observe a partially updated render topology.
    // Already-mounted tab switches keep the persistent fast path below.
    if (structure_changed) {
        doc_->rebuild_render_tree();
    }
    MarkDocumentLayoutDirty("app");
}

void HtmlDocument::mark_dirty() { RequestStructuralRebuild(); }


void HtmlDocument::RestorePendingScrollOffsets() {
    if (pending_scroll_offsets_.empty()) return;
    for (const PendingScrollOffset& offset : pending_scroll_offsets_) {
        if (const auto render = offset.target ? offset.target->get_render_item() : nullptr) {
            render->h_scroll(offset.left);
            render->v_scroll(offset.top);
        }
    }
    pending_scroll_offsets_.clear();
}


std::string HtmlDocument::attribute(const std::string& element_id,
                                       const std::string& name) const {
    if (element_id.empty() || !doc_ || !doc_->root()) return {};
    const auto element = SelectOne("#" + element_id);
    if (!element) return {};
    const char* value = element->get_attr(name.c_str(), "");
    return value != nullptr ? std::string(value) : std::string();
}


HtmlDocument::ScrollMetrics HtmlDocument::scroll_metrics(const std::string& id) {
    ScrollMetrics metrics;
    if (id.empty() || !doc_ || !doc_->root()) return metrics;
    const auto target = FindElement("#" + id);
    if (!target) return metrics;

    for (const ScrollState& state : FrameScrollStates("app")) {
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


void HtmlDocument::select(const std::string& group, const std::string& key) {
    if (group.empty() || key.empty()) return;

    static const bool trace_interactions = std::getenv("IMHTML_TRACE_INTERACTIONS") != nullptr;
    if (trace_interactions) {
        LogPrintf(LogLevel::Trace, "[ImHTML interaction] select group=%s key=%s\n", group.c_str(), key.c_str());
    }

    if (group == "main") {
        if (!HasFragment(key)) return;
        set_state(group + "-selection", key);
        if (current_fragment_ == key && pending_fragment_.empty()) return;
        pending_fragment_ = key;
        RequestStructuralRebuild();
        return;
    }

    const std::string selection_state = group + "-selection";
    set_state(selection_state, key);
    // Nested panels stay mounted after their first build. Switching them is a
    // style/layout change, not a DOM replacement: reparsing the panel and
    // rebuilding every list below it was the dominant cost of renderer,
    // environment, and settings tab changes.
    if (!doc_ || !doc_->root()) {
        RequestStructuralRebuild();
        return;
    }
    const std::size_t mounted_before = mounted_lazy_panels_.size();
    MountLazyPanels();
    if (mounted_lazy_panels_.size() != mounted_before) {
        // The initial control reconciliation runs before lazy panels are mounted.
        // Apply the already-stored values to controls that were just appended so
        // settings and other tab-local controls do not render with HTML defaults.
        SyncCheckboxes();
        SyncTextInputs();
        SyncSelects();
        SyncRanges();
        SyncColors();
    }
    ApplySwitchers();
    ApplyStoredAriaSelection();
    RefreshStylesIfNeeded();
    RebuildRenderTreeForTabSwitch(mounted_lazy_panels_.size() != mounted_before);
}

bool HtmlDocument::HasFragment(const std::string& name) const {
    return fragments_.find(name) != fragments_.end();
}

bool HtmlDocument::handle_interaction(const std::string& url) {
    const std::string normalized_url = TrimURL(url);
    if (normalized_url.empty()) {
        return false;
    }

    static const bool trace_interactions = std::getenv("IMHTML_TRACE_INTERACTIONS") != nullptr;
    if (trace_interactions) {
        LogPrintf(LogLevel::Trace, "[ImHTML interaction] url=%s\n", normalized_url.c_str());
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
    for (const auto& control : CachedSelectorAll("[href]")) {
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
    ++performance_stats_.document_rebuilds;

    if (shell_html_.empty()) {
        doc_.reset();
        content_slot_.reset();
        return;
    }


    const bool recreated = document_recreate_required_ || !doc_;
    if (recreated) {
        inline_styles_.clear();
        element_texts_.clear();
        mounted_lazy_panels_.clear();
        InvalidateDomCaches();
        doc_.reset();
        content_slot_.reset();
        ResetDocument("app");

        const int width = static_cast<int>(ImGui::GetContentRegionAvail().x);
        std::string expanded_shell = ExpandCustomElements(shell_html_);
        InsertStylesheet(expanded_shell, stylesheet_provider_ ? stylesheet_provider_() : std::string());
        doc_ = ParseDocument("app", expanded_shell.c_str(), static_cast<float>(width));
        if (!doc_) {
            content_slot_.reset();
            return;
        }
        content_slot_ = SelectOne("#content-slot");
        if (const auto sidebar = fragments_.find("sidebar"); sidebar != fragments_.end() && sidebar->second) {
            if (const auto target = SelectOne("#sidebar-nav")) {
                const std::string sidebar_html = ExpandCustomElements(sidebar->second());
                doc_->append_children_from_string(*target, sidebar_html.c_str(), false);
            }
        }
        SelectInitialFragment();
        document_recreate_required_ = false;
    }

    static const bool trace_rebuild = std::getenv("IMHTML_TRACE_REBUILD") != nullptr;
    auto phase_clock = std::chrono::steady_clock::now();
    const auto phase = [&](const char* name) {
        if (!trace_rebuild) return;
        const auto now = std::chrono::steady_clock::now();
        LogPrintf(LogLevel::Trace, "  %-22s %8.2f ms\n", name,
                     std::chrono::duration<double, std::milli>(now - phase_clock).count());
        phase_clock = now;
    };
    if (trace_rebuild) LogPrintf(LogLevel::Trace, "[rebuild] fragment=%s recreated=%d\n", current_fragment_.c_str(), (int)recreated);
    phase("shell");

    if (content_slot_) {
        const std::string fragment_html = BuildFragmentHtml(current_fragment_);
        if (!fragment_html.empty()) {
            // Replacing the main fragment destroys every nested lazy-panel
            // element below the slot. Do not retain raw addresses from the
            // previous DOM tree.
            mounted_lazy_panels_.clear();
            const std::string expanded_fragment_html = ExpandCustomElements(fragment_html);
            doc_->append_children_from_string(*content_slot_, expanded_fragment_html.c_str(), true);
            ++performance_stats_.fragment_updates;
        }
    }

    phase("main fragment");

    if (doc_->root()) {
        for (const auto& [fragment_id, provider] : fragments_) {
            if (fragment_id == "sidebar" || !provider) continue;
            if (const auto target = SelectOne("#" + fragment_id)) {
                const auto owner = NearestSwitcher(target);
                if (owner && std::string(owner->get_attr("data-switcher", "")) != "main") continue;
                const std::string fragment_html = ExpandCustomElements(provider());
                doc_->append_children_from_string(*target, fragment_html.c_str(), true);
            }
        }
    }

    phase("other fragments");

    InvalidateDomCaches();
    styles_dirty_ = true;
    MountLazyPanels();
    phase("mount lazy panels");
    // MountLazyPanels() appends DOM nodes. Any selector results collected while
    // discovering the panels are stale after that mutation.
    InvalidateDomCaches();

    SyncCheckboxes();
    SyncTextInputs();
    SyncSelects();
    SyncRanges();
    SyncColors();
    phase("sync controls");
    ApplySwitchers();
    ApplyStoredAriaSelection();
    RefreshStylesIfNeeded();
    phase("switchers");
    if (trace_rebuild) {
        for (const auto& panel : CachedSelectorAll("[role=tabpanel]")) {
            LogPrintf(LogLevel::Trace, "    panel %-30s class='%s' display=%d\n", panel->get_attr("id", "-"),
                         panel->get_attr("class", "-"), (int)panel->css().get_display());
        }
    }
    doc_->rebuild_render_tree();
    phase("rebuild render tree");
    MarkDocumentDirty("app");
}

void HtmlDocument::SyncCheckboxes() {
    if (!doc_ || !doc_->root()) return;

    // Updating dynamic pseudo-classes invalidates selector membership. Own the
    // traversal snapshot so cache invalidation cannot invalidate this loop.
    const std::vector<std::shared_ptr<litehtml::element>> checkboxes =
        CachedSelectorAll("input[type=checkbox]");
    for (const auto& checkbox : checkboxes) {
        const std::string id = checkbox->get_attr("id", "");
        if (id.empty()) continue;
        const bool initial = checkbox->get_attr("checked", nullptr) != nullptr;
        const std::string stored = state_value(id, "");
        const bool checked = stored.empty() ? initial : stored == "true";
        if (!has_value(id)) set_state(id, checked ? "true" : "false");
        const char* state = checked ? "true" : "false";
        const bool style_state_changed = std::strcmp(checkbox->get_attr("data-checked", ""), state) != 0 ||
                                         std::strcmp(checkbox->get_attr("aria-checked", ""), state) != 0;
        checkbox->set_attr("data-checked", state);
        checkbox->set_attr("aria-checked", state);
        const bool disabled = checkbox->get_attr("disabled", nullptr) != nullptr;
        const bool pseudo_state_changed =
            checkbox->set_pseudo_class(litehtml::_id("checked"), checked) |
            checkbox->set_pseudo_class(litehtml::_id("unchecked"), !checked) |
            checkbox->set_pseudo_class(litehtml::_id("disabled"), disabled) |
            checkbox->set_pseudo_class(litehtml::_id("enabled"), !disabled);
        if ((style_state_changed || pseudo_state_changed) && !styles_dirty_) {
            doc_->refresh_element_styles(*checkbox);
        }
        if (style_state_changed || pseudo_state_changed) InvalidateSelectorCaches();
    }
}

void HtmlDocument::SyncTextInputs() {
    if (!doc_ || !doc_->root()) return;

    for (const auto& input : CachedSelectorAll("input")) {
        if (!IsTextInput(input)) continue;
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
        input->set_attr("data-value", state.value.c_str());
    }
}

void HtmlDocument::DrawTextInputs(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root()) return;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;

    // Event handlers below may mutate the DOM and invalidate selector-cache
    // storage. Keep an owning snapshot while dispatching callbacks.
    std::vector<std::shared_ptr<litehtml::element>> inputs;
    for (const auto& input : CachedSelectorAll("input")) {
        if (IsTextInput(input)) inputs.push_back(input);
    }
    if (!active_text_id_.empty()) {
        const auto active = FindElement("#" + active_text_id_);
        if (!IsElementVisible(active)) {
            active_text_id_.clear();
            text_mouse_selecting_ = false;
        }
    }
    // ImGui mouse positions are already expressed in screen coordinates.
    // Adding the window origin a second time made every native HTML control
    // miss its hit target when the canvas was embedded in a child window.
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    std::shared_ptr<litehtml::element> clicked_input;
    std::shared_ptr<litehtml::element> clicked_number_spinner;
    litehtml::position clicked_box;
    int clicked_number_direction = 0;
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
        if (!IsElementVisible(input)) continue;
        litehtml::position box;
        if (!ElementBox(input, box)) continue;
        const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                         document_origin.y + static_cast<float>(box.y));
        const ImVec2 max(min.x + static_cast<float>(box.width), min.y + static_cast<float>(box.height));
        if (mouse_clicked && mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y) {
            if (IsNumberInput(input) && mouse.x >= max.x - NumberSpinnerWidth(box)) {
                clicked_number_spinner = input;
                clicked_box = box;
                clicked_number_direction = mouse.y < (min.y + max.y) * 0.5f ? 1 : -1;
            } else {
                clicked_input = input;
                clicked_box = box;
            }
            break;
        }
    }

    const auto set_number_value = [&](const std::shared_ptr<litehtml::element>& input,
                                      double value) {
        if (!input) return;
        const std::string id = input->get_attr("id", "");
        if (id.empty()) return;
        double minimum = NumberBound(input, "min", -DBL_MAX);
        double maximum = NumberBound(input, "max", DBL_MAX);
        if (minimum > maximum) std::swap(minimum, maximum);
        value = std::clamp(value, minimum, maximum);
        const std::string serialized = SerializeNumberValue(value);
        auto& state = text_states_[id];
        state.value = serialized;
        state.cursor = state.anchor = serialized.size();
        state.scroll_x = 0.0f;
        values_[id] = serialized;
        input->set_attr("data-value", serialized.c_str());
        dispatch_event(id, "input");
    };

    if (mouse_clicked && active_text_id_ != "" && (!clicked_input || clicked_input->get_attr("id", "") != active_text_id_)) {
        if (FindElement("#" + active_text_id_)) {
            dispatch_event(active_text_id_, "change");
        }
        active_text_id_.clear();
        text_mouse_selecting_ = false;
    }
    if (clicked_number_spinner) {
        active_number_drag_id_ = clicked_number_spinner->get_attr("id", "");
        number_drag_start_value_ = ParseRangeNumber(
            text_states_[active_number_drag_id_].value.c_str(),
            ParseRangeNumber(clicked_number_spinner->get_attr("value", ""), 0.0));
        number_drag_start_y_ = mouse.y;
        number_drag_click_direction_ = clicked_number_direction;
        active_text_id_.clear();
        text_mouse_selecting_ = false;
        set_number_value(clicked_number_spinner,
                         number_drag_start_value_ + number_drag_click_direction_ * NumberStep(clicked_number_spinner));
    } else if (clicked_input) {
        active_text_id_ = clicked_input->get_attr("id", "");
        NotifyHtmlControlTextFocus(active_text_id_);
        auto& state = text_states_[active_text_id_];
        state.cursor = state.anchor = cursor_at_mouse(clicked_input, clicked_box, state);
        text_mouse_selecting_ = true;
    }

    if (!active_number_drag_id_.empty() && !mouse_clicked && (mouse_down || mouse_released)) {
        const auto input = FindElement("#" + active_number_drag_id_);
        if (IsElementVisible(input) && IsNumberInput(input)) {
            const double steps = std::floor((number_drag_start_y_ - mouse.y) / 4.0f);
            set_number_value(input, number_drag_start_value_ +
                (number_drag_click_direction_ + steps) * NumberStep(input));
            if (mouse_released) {
                dispatch_event(active_number_drag_id_, "change");
                active_number_drag_id_.clear();
            }
        } else {
            active_number_drag_id_.clear();
        }
    }

    if (text_mouse_selecting_ && !active_text_id_.empty() && (mouse_down || mouse_released)) {
        for (const auto& input : inputs) {
            if (!IsElementVisible(input)) continue;
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

    Platform::SetTextInputActive(!active_text_id_.empty());

    bool any_active = false;
    for (const auto& input : inputs) {
        if (!IsElementVisible(input)) continue;
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
        const auto& font_metrics = input->css().get_font_metrics();
        const float font_size = static_cast<float>(font_metrics.font_size);
        const float line_height = static_cast<float>(font_metrics.height);
        const litehtml::web_color text_color = input->css().get_color();
        const ImU32 text_u32 = IM_COL32(text_color.red, text_color.green, text_color.blue, text_color.alpha);
        const ImU32 selection_u32 = ToImColor(CssBorderColor(input));
        const ImU32 placeholder_u32 = ToImColor(text_color);
        const auto& padding = render->get_paddings();
        const auto& border = render->get_borders();
        const float number_spinner_width = IsNumberInput(input) ? NumberSpinnerWidth(box) : 0.0f;
        const float content_left = min.x + static_cast<float>(border.left + padding.left);
        const float content_right = max.x - static_cast<float>(border.right + padding.right) -
                                    number_spinner_width;
        const float content_width = std::max(1.0f, content_right - content_left);
        const float text_y = min.y + (max.y - min.y - line_height) * 0.5f;
        const bool active = active_text_id_ == id;
        NativeControlClipScope scroll_clip(doc_, input, document_origin, draw_list);
        std::string value_before = state.value;
        if (active) {
            active_text_id_ = id;
            any_active = true;
        }

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
                // DOM listeners observe a value change; they do not own the
                // input's value. Commit first so a listener cannot cause the
                // edit to snap back as soon as focus leaves the field.
                // This is the control's live value property. Assign directly:
                // public set_value() deliberately defers conflicting external
                // writes while this field owns focus.
                values_[id] = state.value;
                dispatch_event(id, "input");
            }
        }

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

        if (IsNumberInput(input)) {
            const float spinner_left = max.x - number_spinner_width;
            const float middle = (min.y + max.y) * 0.5f;
            const bool spinner_hovered = mouse.x >= spinner_left && mouse.x < max.x &&
                                         mouse.y >= min.y && mouse.y < max.y;
            const bool upper_hovered = spinner_hovered && mouse.y < middle;
            const bool lower_hovered = spinner_hovered && mouse.y >= middle;
            const ImU32 normal_color = IM_COL32(37, 38, 42, 120);
            const ImU32 hovered_color = IM_COL32(59, 156, 82, 150);
            const ImU32 icon_color = IM_COL32(243, 244, 246, 210);
            if (upper_hovered)
                draw_list->AddRectFilled(ImVec2(spinner_left, min.y), ImVec2(max.x, middle), hovered_color);
            if (lower_hovered)
                draw_list->AddRectFilled(ImVec2(spinner_left, middle), ImVec2(max.x, max.y), hovered_color);
            draw_list->AddRectFilled(ImVec2(spinner_left, min.y), ImVec2(max.x, max.y),
                                     spinner_hovered ? IM_COL32(0, 0, 0, 0) : normal_color);
            draw_list->AddLine(ImVec2(spinner_left, middle), ImVec2(max.x, middle),
                               IM_COL32(65, 67, 75, 150), 1.0f);
            const float centre_x = spinner_left + number_spinner_width * 0.5f;
            const float arrow_width = std::max(2.5f, number_spinner_width * 0.16f);
            const float arrow_height = std::max(1.5f, (max.y - min.y) * 0.09f);
            draw_list->AddTriangleFilled(
                ImVec2(centre_x, min.y + (middle - min.y) * 0.34f - arrow_height),
                ImVec2(centre_x - arrow_width, min.y + (middle - min.y) * 0.34f + arrow_height),
                ImVec2(centre_x + arrow_width, min.y + (middle - min.y) * 0.34f + arrow_height),
                icon_color);
            draw_list->AddTriangleFilled(
                ImVec2(centre_x, middle + (max.y - middle) * 0.66f + arrow_height),
                ImVec2(centre_x - arrow_width, middle + (max.y - middle) * 0.66f - arrow_height),
                ImVec2(centre_x + arrow_width, middle + (max.y - middle) * 0.66f - arrow_height),
                icon_color);
        }
    }

    if (!any_active) active_text_id_.clear();
}

void HtmlDocument::SyncSelects() {
    if (!doc_ || !doc_->root()) return;

    bool options_changed = false;
    for (const auto& select : CachedSelectorAll("select")) {
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

        select->set_attr("data-value", current.c_str());
        select->set_attr("aria-valuetext", current.c_str());
        for (const auto& option : select->select_all("option")) {
            const char* value = option->get_attr("value", "");
            const bool is_selected = value != nullptr && current == value;
            option->set_attr("selected", is_selected ? "true" : "false");
            option->set_attr("aria-selected", is_selected ? "true" : "false");
        }
    }

    if (options_changed) {
        styles_dirty_ = true;
        RefreshStylesIfNeeded();
        doc_->rebuild_render_tree();
        MarkDocumentLayoutDirty("app");
    }
}

void HtmlDocument::SyncRanges() {
    if (!doc_ || !doc_->root()) return;

    for (const auto& range : CachedSelectorAll("input[type=range]")) {
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
        range->set_attr("data-range-value", serialized.c_str());
        range->set_attr("aria-valuemin", SerializeRangeValue(low).c_str());
        range->set_attr("aria-valuemax", SerializeRangeValue(high).c_str());
        range->set_attr("aria-valuenow", serialized.c_str());
        set_text("#" + id + "-value", FormatRangeValue(current));
    }
}

void HtmlDocument::SyncColors() {
    if (!doc_ || !doc_->root()) return;

    for (const auto& color : CachedSelectorAll("input[type=color]")) {
        const std::string id = color->get_attr("id", "");
        if (id.empty()) continue;

        const std::string initial = color->get_attr("value", "#000000");
        const std::string stored = state_value(id, "");
        const std::string current = stored.empty() ? initial : stored;
        const std::array<float, 4> parsed = ParseHtmlColor(current);
        const std::string normalized = SerializeHtmlColor(parsed.data());
        if (!has_value(id) || stored != normalized) set_state(id, normalized);
        color->set_attr("data-color-value", normalized.c_str());
        color->set_attr("data-value", normalized.c_str());
        color->set_attr("aria-valuetext", normalized.c_str());
    }
}

// The popup is drawn over the document but is not part of it, so a press that
// starts inside it must be hidden from litehtml for its whole press-release
// cycle, or the elements under the popup would be clicked too.
void HtmlDocument::BlockPointerBehindSelectPopup(const ImVec2& document_origin) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        select_popup_pointer_blocked_ = false;
        if (!active_select_id_.empty()) {
            const auto select = FindElement("#" + active_select_id_);
            const std::vector<std::string> options = SelectOptionValues(select);
            SelectPopupGeometry popup;
            const ImVec2 mouse = ImGui::GetMousePos();
            select_popup_pointer_blocked_ = select && !options.empty() &&
                GetSelectPopupGeometry(select, document_origin, options.size(), popup) &&
                mouse.x >= popup.min.x && mouse.x < popup.max.x &&
                mouse.y >= popup.min.y && mouse.y < popup.max.y;
        }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        select_popup_pointer_blocked_ = false;
    }
    SetDocumentPointerBlocked("app", select_popup_pointer_blocked_);
}

void HtmlDocument::UpdateSelectFromMouse(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root() || !doc_->root_render()) return;

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    if (!active_select_id_.empty()) {
        const auto select = FindElement("#" + active_select_id_);
        if (!IsElementVisible(select)) {
            active_select_id_.clear();
            select_scroll_dragging_ = false;
            return;
        }
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
            const std::string input_id = select->get_attr("data-editable-input", "");
            if (!input_id.empty()) {
                active_text_id_.clear();
                set_value(input_id, value);
                dispatch_event(input_id, "change");
            }
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
            } else if (select->get_attr("data-editable-input", "")[0] != '\0') {
                active_select_id_.clear();
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
        if (tag != nullptr && (std::strcmp(tag, "select") == 0 ||
                              current->get_attr("data-editable-input", "")[0] != '\0')) {
            select = current;
            break;
        }
        if (tag != nullptr && std::strcmp(tag, "label") == 0) {
            const char* target = current->get_attr("for", "");
            if (target != nullptr && target[0] != '\0') {
                select = FindElement("#" + std::string(target));
            }
            break;
        }
    }

    if (!select) {
        active_select_id_.clear();
        return;
    }
    if (select->get_attr("data-editable-input", "")[0] != '\0') {
        litehtml::position box;
        if (!ElementBox(select, box) ||
            x < static_cast<float>(box.x + box.width) -
                2.0f * static_cast<float>(select->css().get_font_metrics().font_size)) return;
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

    if (!popup_only) {
        // litehtml's DOM selector parser accepts one selector, not a CSS
        // selector list. Keep ordinary selects on their original query.
        const auto& select_elements = CachedSelectorAll("select");
        for (const auto& select : select_elements) {
            if (!IsElementVisible(select)) continue;
            litehtml::position box;
            if (!ElementBox(select, box)) continue;
            const auto render = select->get_render_item();
            if (!render) continue;

            const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                             document_origin.y + static_cast<float>(box.y));
            const ImVec2 max(min.x + static_cast<float>(box.width), min.y + static_cast<float>(box.height));
            const litehtml::web_color text_color = select->css().get_color();
            const float font_size = static_cast<float>(select->css().get_font_metrics().font_size);
            const ImU32 text_u32 = IM_COL32(text_color.red, text_color.green, text_color.blue, text_color.alpha);
            const auto& padding = render->get_paddings();
            const auto& border = render->get_borders();
            const std::string value = select->get_attr("data-value", "");
            const ImVec2 text_min(min.x + static_cast<float>(border.left + padding.left),
                                  min.y + (max.y - min.y - font_size) * 0.5f);
            NativeControlClipScope scroll_clip(doc_, select, document_origin, draw_list);
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

    if (!popup_only) {
        for (const auto& editable : CachedSelectorAll("[data-editable-input]")) {
            if (!IsElementVisible(editable)) continue;
            litehtml::position box;
            if (!ElementBox(editable, box)) continue;
            const auto render = editable->get_render_item();
            if (!render) continue;
            const float font_size = static_cast<float>(editable->css().get_font_metrics().font_size);
            const float arrow_size = std::max(2.0f, font_size * 0.2f);
            const auto& padding = render->get_paddings();
            const auto& border = render->get_borders();
            const ImVec2 arrow(document_origin.x + static_cast<float>(box.x + box.width) -
                                   static_cast<float>(border.right + padding.right) - arrow_size,
                               document_origin.y + static_cast<float>(box.y) +
                                   static_cast<float>(box.height) * 0.5f);
            const ImU32 color = ToImColor(editable->css().get_color());
            NativeControlClipScope scroll_clip(doc_, editable, document_origin, draw_list);
            draw_list->AddLine(ImVec2(arrow.x - arrow_size, arrow.y - arrow_size * 0.35f),
                              ImVec2(arrow.x, arrow.y + arrow_size * 0.35f), color, 1.2f);
            draw_list->AddLine(ImVec2(arrow.x, arrow.y + arrow_size * 0.35f),
                              ImVec2(arrow.x + arrow_size, arrow.y - arrow_size * 0.35f), color, 1.2f);
        }
    }

    if (active_select_id_.empty()) return;
    const auto select = FindElement("#" + active_select_id_);
    if (!IsElementVisible(select)) {
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
    const ImVec2 mouse = ImGui::GetMousePos();
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
        styles_dirty_ = true;
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

    const ImVec2 mouse = ImGui::GetMousePos();
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

    const auto range = FindElement("#" + active_range_id_);
    if (!IsElementVisible(range)) {
        active_range_id_.clear();
        return;
    }
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
    const double value = QuantizeRangeValue(
        RangeValueAt(range, normalized, low, high), low, high, RangeStep(range));
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

    for (const auto& range : CachedSelectorAll("input[type=range]")) {
        if (!IsElementVisible(range)) continue;
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
        const float height = static_cast<float>(box.height);
        const float knob_width = RangeThumbWidth(height);
        const float travel_left = min.x + knob_width * 0.5f;
        const float travel = std::max(max.x - min.x - knob_width, 1.0f);
        const float normalized = static_cast<float>(RangePositionOf(range, current, low, high));
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

        NativeControlClipScope scroll_clip(doc_, range, document_origin, draw_list);
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

void HtmlDocument::UpdateColorFromMouse(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root() || !doc_->root_render() ||
        !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

    const ImVec2 mouse = ImGui::GetMousePos();
    const float x = mouse.x - document_origin.x;
    const float y = mouse.y - document_origin.y;
    const auto hit = doc_->root_render()->get_element_by_point(
        static_cast<litehtml::pixel_t>(x), static_cast<litehtml::pixel_t>(y),
        static_cast<litehtml::pixel_t>(x), static_cast<litehtml::pixel_t>(y), nullptr);

    std::shared_ptr<litehtml::element> color = ColorAncestor(hit);
    if (!color) {
        for (auto current = hit; current; current = current->parent()) {
            if (std::strcmp(current->get_tagName(), "label") != 0) continue;
            const char* target = current->get_attr("for", "");
            if (target != nullptr && target[0] != '\0')
                color = ColorAncestor(FindElement("#" + std::string(target)));
            break;
        }
    }
    if (!color) return;

    const std::string id = color->get_attr("id", "");
    if (!id.empty()) ImGui::OpenPopup(ColorPopupId(id).c_str());
}

void HtmlDocument::DrawColors(const ImVec2& document_origin) {
    if (!doc_ || !doc_->root()) return;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list) return;

    for (const auto& color : CachedSelectorAll("input[type=color]")) {
        if (!IsElementVisible(color)) continue;
        litehtml::position box;
        if (!ElementBox(color, box)) continue;

        const ImVec2 min(document_origin.x + static_cast<float>(box.x),
                         document_origin.y + static_cast<float>(box.y));
        const ImVec2 max(min.x + static_cast<float>(box.width), min.y + static_cast<float>(box.height));
        const float width = max.x - min.x;
        const float height = max.y - min.y;
        const float inset = std::max(2.0f, std::min(width, height) * 0.14f);
        const auto parsed = ParseHtmlColor(color->get_attr("data-color-value", "#000000"));
        const ImU32 swatch = IM_COL32(
            static_cast<int>(std::lround(parsed[0] * 255.0f)),
            static_cast<int>(std::lround(parsed[1] * 255.0f)),
            static_cast<int>(std::lround(parsed[2] * 255.0f)), 255);
        const auto border = CssBorderColor(color);
        {
            NativeControlClipScope scroll_clip(doc_, color, document_origin, draw_list);
            draw_list->AddRectFilled(ImVec2(min.x + inset, min.y + inset),
                                     ImVec2(max.x - inset, max.y - inset), swatch,
                                     std::max(1.0f, height * 0.18f));
            draw_list->AddRect(ImVec2(min.x + inset, min.y + inset),
                               ImVec2(max.x - inset, max.y - inset), ToImColor(border),
                               std::max(1.0f, height * 0.18f));
        }

        const std::string popup_id = ColorPopupId(color->get_attr("id", ""));
        constexpr float popup_width = 320.0f;
        constexpr float popup_height = 390.0f;
        const ImVec2 display_size = ImGui::GetIO().DisplaySize;
        ImVec2 popup_pos(max.x + 10.0f, min.y);
        if (popup_pos.x + popup_width > display_size.x - 8.0f)
            popup_pos.x = min.x - popup_width - 10.0f;
        if (popup_pos.y + popup_height > display_size.y - 8.0f)
            popup_pos.y = display_size.y - popup_height - 8.0f;
        popup_pos.x = std::max(8.0f, popup_pos.x);
        popup_pos.y = std::max(8.0f, popup_pos.y);
        ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(popup_width, popup_height), ImGuiCond_Appearing);

        const Theme::AccentColor& accent = Theme::ui_accent();
        const ImVec4 accent_color(accent.r / 255.0f, accent.g / 255.0f,
                                  accent.b / 255.0f, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 14.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.105f, 0.110f, 0.130f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.255f, 0.270f, 0.310f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.145f, 0.155f, 0.185f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.190f, 0.205f, 0.240f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.225f, 0.245f, 0.275f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.145f, 0.155f, 0.185f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.190f, 0.205f, 0.240f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent_color);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, accent_color);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, accent_color);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, accent_color);
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.255f, 0.270f, 0.310f, 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.925f, 0.930f, 0.950f, 1.0f));

        const bool popup_visible = ImGui::BeginPopup(popup_id.c_str());
        if (!popup_visible) {
            ImGui::PopStyleColor(13);
            ImGui::PopStyleVar(10);
            continue;
        }

        float rgba[4] = {parsed[0], parsed[1], parsed[2], 1.0f};
        const bool changed = ImGui::ColorPicker4(
            "##html-color-picker", rgba,
            ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_DisplayHex |
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview);
        if (changed) {
            const std::string value = SerializeHtmlColor(rgba);
            set_state(color->get_attr("id", ""), value);
            color->set_attr("data-color-value", value.c_str());
            color->set_attr("data-value", value.c_str());
            color->set_attr("aria-valuetext", value.c_str());
            dispatch_event(color->get_attr("id", ""), "input");
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                dispatch_event(color->get_attr("id", ""), "change");
        }
        ImGui::EndPopup();
        ImGui::PopStyleColor(13);
        ImGui::PopStyleVar(10);
    }
}

void HtmlDocument::MountLazyPanels() {
    if (!doc_ || !doc_->root()) return;

    scratch_arena_.release();
    bool mounted_panel = true;
    bool dom_changed = false;
    while (mounted_panel) {
        mounted_panel = false;
        // This loop mutates the DOM by appending fragment content. Query the
        // current tree on every pass so nested lazy switchers are discovered.
        const auto switchers = doc_->root()->select_all("[data-switcher]");
        const auto panels = doc_->root()->select_all("[role=tabpanel]");

        for (const auto& switcher : switchers) {
            const std::string group = switcher->get_attr("data-switcher", "");
            if (group.empty() || group == "main") continue;

            struct Candidate {
                std::shared_ptr<litehtml::element> element;
                std::string key;
            };
            std::pmr::vector<Candidate> owned_panels(&scratch_arena_);
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
                // Mount the selected panel on first visit, but retain it after
                // switching away so returning to it does not reparse the
                // fragment or recreate its list/control DOM.
                if (panel.key != selected || mounted_lazy_panels_.contains(panel.element.get())) {
                    continue;
                }
                const std::string fragment_id = panel.element->get_attr("id", "");
                const auto found = fragments_.find(fragment_id);
                if (fragment_id.empty() || found == fragments_.end() || !found->second) continue;

                const std::string fragment_html = ExpandCustomElements(found->second());
                doc_->append_children_from_string(*panel.element, fragment_html.c_str(), false);
                mounted_lazy_panels_.insert(panel.element.get());
                dom_changed = true;
                mounted_panel = true;
            }
        }
    }
    if (dom_changed) {
        InvalidateDomCaches();
        styles_dirty_ = true;
    }
}

std::shared_ptr<litehtml::element> HtmlDocument::NearestSwitcher(
    const std::shared_ptr<litehtml::element>& element) const {
    if (!element) return {};
    if (const auto found = switcher_owner_cache_.find(element.get());
        found != switcher_owner_cache_.end()) {
        return found->second.lock();
    }

    std::shared_ptr<litehtml::element> owner;
    for (auto current = element; current; current = current->parent()) {
        if (std::string(current->get_attr("data-switcher", "")).empty()) continue;
        owner = current;
        break;
    }
    switcher_owner_cache_[element.get()] = owner;
    return owner;
}

bool HtmlDocument::IsElementMounted(const std::shared_ptr<litehtml::element>& element) const {
    if (!element || !doc_ || !doc_->root()) return false;
    bool belongs_to_document = false;
    for (auto current = element; current; current = current->parent()) {
        if (current.get() == doc_->root().get()) {
            belongs_to_document = true;
            break;
        }
    }
    if (!belongs_to_document) return false;

    for (auto current = element; current; current = current->parent()) {
        if (std::string(current->get_attr("role", "")) != "tabpanel") continue;
        const auto owner = NearestSwitcher(current);
        if (!owner || std::string(owner->get_attr("data-switcher", "")) == "main") return true;
        return mounted_lazy_panels_.contains(current.get());
    }
    return true;
}

bool HtmlDocument::IsElementVisible(const std::shared_ptr<litehtml::element>& element) const {
    for (auto current = element; current; current = current->parent()) {
        if (std::string(current->get_attr("role", "")) != "tabpanel") continue;
        const auto owner = NearestSwitcher(current);
        if (!owner || std::string(owner->get_attr("data-switcher", "")) == "main") return true;
        return mounted_lazy_panels_.contains(current.get()) && current->is_visible() &&
               current->css().get_display() != litehtml::display_none;
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

void HtmlDocument::ApplySwitchers() {
    if (!doc_ || !doc_->root()) return;

    // Switching a panel changes classes and ARIA attributes that the stylesheet
    // selects on. Whatever ran earlier in the rebuild may already have consumed
    // styles_dirty_, so re-arm it here or the new state never reaches layout.
    scratch_arena_.release();
    bool state_changed = false;
    bool style_changed = false;
    std::pmr::vector<std::shared_ptr<litehtml::element>> style_targets(&scratch_arena_);
    const auto set_class_if_changed = [&](const std::shared_ptr<litehtml::element>& element, const char* name,
                                          const bool add) {
        if (element->set_class(name, add)) {
            state_changed = true;
            style_changed = true;
            style_targets.push_back(element);
        }
    };
    const auto set_attr_if_changed = [&](const std::shared_ptr<litehtml::element>& element, const char* name,
                                         const char* value) {
        if (std::strcmp(element->get_attr(name, ""), value) == 0) return;
        element->set_attr(name, value);
        state_changed = true;
    };
    const auto set_active_style_if_changed = [&](const std::shared_ptr<litehtml::element>& element,
                                                 const bool active) {
        // Keep the visual state on the control itself. The embedded litehtml
        // stylesheet can lag behind a runtime class mutation, while inline
        // style is applied during the same reconciliation pass as selection.
        const char* desired = active ? "background: #3b9c52; color: #fff;" : "";
        if (std::strcmp(element->get_attr("style", ""), desired) == 0) return;
        element->set_attr("style", desired);
        state_changed = true;
        style_changed = true;
        style_targets.push_back(element);
    };

    struct SwitchPanel {
        std::shared_ptr<litehtml::element> element;
        std::string key;
    };

    for (const auto& indexed : CachedSwitcherIndex()) {
        const std::string& group = indexed.group;

        std::pmr::vector<SwitchPanel> owned_panels(&scratch_arena_);
        owned_panels.reserve(indexed.panels.size());
        for (const auto& panel : indexed.panels) {
            const std::string key = SwitchPanelKey(panel);
            if (!key.empty()) {
                owned_panels.push_back({panel, key});
            }
        }
        if (owned_panels.empty()) {
            if (group != "main") continue;
            const std::string selected = current_fragment_;
            for (const auto& control : indexed.controls) {
                const bool active = SwitchControlTarget(control) == selected;
                set_class_if_changed(control, "active", active);
                set_active_style_if_changed(control, active);
                set_attr_if_changed(control, "aria-pressed", active ? "true" : "false");
            }
            continue;
        }

        const std::string fallback = owned_panels.front().key;
        const std::string requested = state_value(group + "-selection", fallback);
        const auto selected_panel = std::ranges::find_if(
            owned_panels, [&](const SwitchPanel& panel) { return SwitchPanelMatches(panel.element, requested); });
        const bool valid = selected_panel != owned_panels.end();
        const std::string selected = valid ? selected_panel->key : fallback;
        // Materialize the fallback so startup and later document rebuilds use
        // the same deterministic tab selection instead of recomputing it.
        const std::string state_key = group + "-selection";
        if (!has_value(state_key)) set_state(state_key, selected);

        for (const SwitchPanel& panel : owned_panels) {
            const bool hidden = panel.key != selected;
            // Persistent panels stay in the DOM for fast switching, but only
            // the selected panel may enter litehtml's render tree.
            // Visibility is owned by the host-managed render item. Persistent
            // panels stay in the DOM, but hidden panels must be excluded from
            // layout, paint, and hit testing.
            panel.element->set_visible(!hidden);
            set_class_if_changed(panel.element, "is-hidden", false);
            set_attr_if_changed(panel.element, "aria-hidden", hidden ? "true" : "false");
        }
        const auto active_panel = std::ranges::find_if(
            owned_panels, [&](const SwitchPanel& panel) { return panel.key == selected; });
        for (const auto& control : indexed.controls) {
            const std::string target = SwitchControlTarget(control);
            const bool active = target == selected ||
                                (active_panel != owned_panels.end() && target == active_panel->element->get_attr("id", ""));
            set_class_if_changed(control, "active", active);
            set_active_style_if_changed(control, active);
            set_attr_if_changed(control, "aria-pressed", active ? "true" : "false");
            bool is_tab = false;
            for (auto ancestor = control->parent(); ancestor; ancestor = ancestor->parent()) {
                if (std::string(ancestor->get_attr("role", "")) == "tablist") {
                    is_tab = true;
                    break;
                }
            }
            if (is_tab) set_attr_if_changed(control, "aria-selected", active ? "true" : "false");
        }
    }

    if (style_changed) {
        // A document rebuild may already have marked the stylesheet dirty
        // because lazy panel content was appended. Refresh the full stylesheet
        // immediately in that case; a targeted element refresh cannot discover
        // selectors that were not active when the element was parsed.
        if (styles_dirty_) {
            RefreshStylesIfNeeded();
        } else {
            RefreshElementStyles(style_targets);
        }
    } else if (state_changed) {
        // ARIA state does not affect layout or CSS matching.
    }
}

bool HtmlDocument::SelectPanelById(const std::string& id) {
    if (id.empty()) return false;
    if (doc_ && doc_->root()) {
        if (const auto panel = FindElement("#" + id)) {
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
    const auto checkbox = FindElement("#" + id);
    if (!checkbox || checkbox->get_tagName() == nullptr || std::strcmp(checkbox->get_tagName(), "input") != 0 ||
        std::string(checkbox->get_attr("type", "")) != "checkbox") {
        return false;
    }
    // Disabled form controls do not run activation behavior.
    if (checkbox->get_attr("disabled", nullptr) != nullptr) return true;

    const bool checked = std::string(checkbox->get_attr("data-checked", "")) == "true";
    const bool next = !checked;
    set_checked(id, next);
    dispatch_event(id, "click");
    dispatch_event(id, "change");
    return true;
}

void HtmlDocument::set_style(const std::string& selector, const std::string& css) {
    if (!doc_) {
        return;
    }
    auto target = FindElement(selector);
    if (!target) return;
    const auto previous = inline_styles_.find(selector);
    const char* live_style = target->get_attr("style", "");
    if (previous != inline_styles_.end() && previous->second == css &&
        live_style != nullptr && css == live_style) {
        return;
    }
    target->set_attr("style", css.c_str());
    target->compute_styles(false);
    inline_styles_[selector] = css;
    // Inline style changes cannot alter selector membership. Avoid a full
    // stylesheet refresh and selector-cache invalidation for high-frequency
    // geometry updates such as splitter drags.
    Invalidate(DirtyState::Layout);
    MarkDocumentLayoutDirty("app");
}

std::shared_ptr<litehtml::element> HtmlDocument::FindElement(const std::string& selector) {
    if (!doc_ || !doc_->root()) return {};
    if (const auto found = element_cache_.find(selector); found != element_cache_.end()) {
        if (auto target = found->second.lock()) return target;
    }
    auto target = SelectOne(selector);
    if (target) element_cache_[selector] = target;
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
            Invalidate(DirtyState::Layout);
            MarkDocumentLayoutDirty("app");
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
    active_listbox_id_ = multi ? listbox_id : std::string();

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
        InvalidateSelectorCaches();
        styles_dirty_ = true;
    }
    return true;
}

void HtmlDocument::HandleListboxSelectAll() {
    if (active_listbox_id_.empty() || !active_text_id_.empty()) return;
    if (!ImGui::GetIO().KeyCtrl || !ImGui::IsKeyPressed(ImGuiKey_A, false)) return;
    const auto listbox = FindElement("#" + active_listbox_id_);
    if (!listbox) {
        active_listbox_id_.clear();
        return;
    }

    std::vector<std::shared_ptr<litehtml::element>> options;
    CollectOptions(listbox, options);
    auto& store = aria_selections_[active_listbox_id_];
    store.clear();
    for (const auto& option : options) {
        const bool selectable = std::string(option->get_attr("data-selectable", "true")) != "false";
        const std::string option_id = option->get_attr("id", "");
        if (selectable && !option_id.empty()) store.insert(option_id);
    }
    ApplyStoredAriaSelection();
    dispatch_event(active_listbox_id_, "change");
}

void HtmlDocument::ApplyStoredAriaSelection() {
    if (!doc_ || !doc_->root() || aria_selections_.empty()) return;
    bool changed = false;
    for (const auto& [listbox_id, ids] : aria_selections_) {
        const auto listbox = FindElement("#" + listbox_id);
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
        InvalidateSelectorCaches();
        styles_dirty_ = true;
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
        // Any attribute can participate in a CSS selector. Keep the DOM
        // pointer cache, but invalidate selector membership conservatively.
        InvalidateSelectorCaches();
        styles_dirty_ = true;
        Invalidate(mode == ElementUpdateMode::Layout ? DirtyState::Layout : DirtyState::Paint);
        if (mode == ElementUpdateMode::Layout) {
            MarkDocumentLayoutDirty("app");
        }
    }
}

void HtmlDocument::frame() {
    const ContextScope scope(*context_);
    ++frame_count_;
    frame_started_at_ = std::chrono::steady_clock::now();
    performance_stats_.frames = frame_count_;
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


    if (needs_rebuild_) {
        needs_rebuild_ = false;
        RebuildDocument();
    }

    if (before_render) {
        before_render();
    }

    if (application_) {
        for (const auto& control : html_controls_) {
            if (control) control->before_render(*this, *application_);
        }
    }

    SyncTextInputs();
    SyncSelects();
    SyncRanges();
    RefreshStylesIfNeeded();
    const ImVec2 document_origin = ImGui::GetCursorScreenPos();
    document_origin_ = document_origin;
    if (dom_render_tree_dirty_) {
        doc_->rebuild_render_tree();
        dom_render_tree_dirty_ = false;
    }
    BlockPointerBehindSelectPopup(document_origin);
    std::string clicked_url;
    const auto render_started_at = std::chrono::steady_clock::now();
    const bool clicked = RenderDocument("app", 0.0f, &clicked_url) && !clicked_url.empty();
    RestorePendingScrollOffsets();
    const auto render_finished_at = std::chrono::steady_clock::now();
    const DocumentRenderStats render_stats = GetDocumentRenderStats("app");
    performance_stats_.layout_count = render_stats.layouts;
    performance_stats_.paint_count = render_stats.paints;
    performance_stats_.last_layout_ms = render_stats.last_layout_ms;
    performance_stats_.last_paint_ms = render_stats.last_paint_ms;
    if (render_stats.last_frame_relayout && performance_stats_.last_paint_ms <= 0.0) {
        performance_stats_.last_paint_ms =
            std::chrono::duration<double, std::milli>(render_finished_at - render_started_at).count();
    }
    static const bool trace_layout = std::getenv("IMHTML_TRACE_LAYOUT") != nullptr;
    if (trace_layout && render_stats.last_frame_relayout) {
        LogPrintf(LogLevel::Trace, "[ImHTML layout] fragment=%s frame=%llu layout=%.3f paint=%.3f\n",
                     current_fragment_.c_str(),
                     static_cast<unsigned long long>(frame_count_),
                     render_stats.last_layout_ms, render_stats.last_paint_ms);
    }
    DrawTextInputs(document_origin);
    HandleListboxSelectAll();
    UpdateSelectFromMouse(document_origin);
    UpdateRangeFromMouse(document_origin);
    DrawSelects(document_origin);
    DrawRanges(document_origin);
    UpdateColorFromMouse(document_origin);
    DrawColors(document_origin);
    DispatchPointerEvents(document_origin);
    if (after_render) {
        after_render();
    }
    DrawSelects(document_origin, true);


    // RenderDocument discovers link clicks after painting the current render
    // tree. Apply the resulting selection only after all native overlays have
    // been drawn, otherwise one frame can contain the old litehtml panel and
    // controls from the newly selected panel.
    if (clicked) {
        handle_interaction(clicked_url);
    }

    if (telemetry_enabled() && frame_count_ % 120 == 0) {
        const auto& render = performance_stats_;
        LogPrintf(LogLevel::Trace, "[ImHTML telemetry] frame-ms=%.3f layout-ms=%.3f paint-ms=%.3f selectors=%llu styles=%llu\n",
                     render.last_frame_ms, render.last_layout_ms, render.last_paint_ms,
                     static_cast<unsigned long long>(render.selector_scans),
                     static_cast<unsigned long long>(render.style_refreshes));
    }

    performance_stats_.last_frame_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_started_at_).count();

    ImGui::End();
    dirty_state_ = DirtyState::Clean;
    ImGui::PopStyleVar();
}

void HtmlDocument::shutdown() {
    // Keep CanvasState alive while litehtml destroys its document: the
    // document destructor calls back into its BrowserContainer to delete
    // fonts. Erase the registry only after the document has been released.
    doc_.reset();
    content_slot_.reset();
    InvalidateDomCaches();
    mounted_lazy_panels_.clear();
    ResetDocument("app");
}

}  // namespace ImHTML
