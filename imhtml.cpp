
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include "imgui_internal.h"
#include <inja/inja.hpp>
#include "imhtml.hpp"
#include "litehtml.h"
#include "litehtml/render_item.h"
#include "litehtml/types.h"

namespace ImHTML {

/**
 * Custom element
 */
class CustomElement : public litehtml::html_tag {
 private:
  std::string tag = "";
  std::map<std::string, std::string> attributes = {};

 public:
  CustomElement(const std::shared_ptr<litehtml::document>& doc, const std::string& tag,
                std::map<std::string, std::string> attributes)
      : litehtml::html_tag(doc), tag(tag), attributes(attributes) {
    // Register the tag name so that css selectors can modify it
    set_tagName(tag.c_str());
  }

  /**
   * @brief Force display:block after normal CSS computation.
   *
   * Custom elements are replaced/leaf elements that must always occupy a full
   * line. Overriding compute_styles lets us guarantee block display regardless
   * of what the master CSS or author stylesheet resolves to.
   *
   * @param recursive Whether to recurse into children (passed through to base)
   */
  void compute_styles(bool recursive = true) override {
    litehtml::html_tag::compute_styles(recursive);
    m_css.set_display(litehtml::display_block);
  }

  void draw_background(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y, const litehtml::position* clip,
                       const std::shared_ptr<litehtml::render_item>& ri) override;
};

std::string DefaultFileLoader(const char* url, const char* baseurl) {
  if (url == nullptr || strlen(url) == 0) {
    return "";
  }

  std::ifstream file(url);
  if (!file.is_open()) {
    IMHTML_PRINTF("[ImHTML] Failed to open file: %s\n", url);
    return "";
  }

  std::string content((std::istreambuf_iterator<char>(file)), (std::istreambuf_iterator<char>()));
  return content;
}

namespace {

Config config = Config{
    .BaseFontSize = 16,
    .LoadCSS = DefaultFileLoader,
};

std::vector<Config> configStack;
std::unordered_map<std::string, CustomElementDrawFunction> customElements;
std::unordered_map<std::string, CustomElementHtmlFunction> customElementHtmls;

const Config* getCurrentConfigPtr() {
  return configStack.empty() ? &config : &configStack.back();
}

static ImFont* getFontFromFamily(const FontFamily& family, FontStyle style) {
  switch (style) {
    case FontStyle::Regular:
      return family.Regular;
    case FontStyle::Bold:
      return family.Bold ? family.Bold : family.Regular;
    case FontStyle::Italic:
      return family.Italic ? family.Italic : family.Regular;
    case FontStyle::BoldItalic:
      if (family.BoldItalic) return family.BoldItalic;
      if (family.Bold) return family.Bold;
      if (family.Italic) return family.Italic;
      return family.Regular;
    default:
      return family.Regular;
  }
}

static ImFont* resolveFont(const Config& cfg, const std::string& family_name, FontStyle style) {
  if (!family_name.empty()) {
    auto it = cfg.FontFamilies.find(family_name);
    if (it != cfg.FontFamilies.end()) {
      if (ImFont* f = getFontFromFamily(it->second, style)) {
        return f;
      }
    }
  }

  if (ImFont* f = getFontFromFamily(cfg.DefaultFont, style)) {
    return f;
  }

  return ImGui::GetFont();
}
}  // namespace

void CustomElement::draw_background(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
                                    const litehtml::position* clip, const std::shared_ptr<litehtml::render_item>& ri) {
  // Let the base class draw background color/image and borders first.
  litehtml::html_tag::draw_background(hdc, x, y, clip, ri);

  // ri->pos() is the element's own content box relative to its parent.
  litehtml::position content = ri->pos();
  content.x += x;
  content.y += y;
  litehtml::position padding_box = content;
  padding_box += ri->get_paddings();
  litehtml::position border_box = padding_box;
  border_box += ri->get_borders();

  if (const auto found = customElements.find(this->tag); found != customElements.end()) {
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    const auto to_screen_rect = [&cursor](const litehtml::position& position) {
      return ImRect(cursor + ImVec2(position.x, position.y),
                    cursor + ImVec2(position.x + position.width, position.y + position.height));
    };
    found->second(to_screen_rect(border_box), this->attributes);
    ImGui::SetCursorScreenPos(cursor);
  }
}

namespace {
struct ScrollbarGeometry {
  ImVec2 track_min;
  ImVec2 track_max;
  ImVec2 thumb_min;
  ImVec2 thumb_max;
};

litehtml::web_color CssBackgroundColor(const litehtml::element* element) {
  return element != nullptr ? element->css().get_bg().m_color : litehtml::web_color::transparent;
}

litehtml::web_color CssBorderColor(const litehtml::element* element) {
  if (element == nullptr) return litehtml::web_color::transparent;
  const auto& borders = element->css().get_borders();
  if (borders.top.color.alpha != 0) return borders.top.color;
  if (borders.right.color.alpha != 0) return borders.right.color;
  if (borders.bottom.color.alpha != 0) return borders.bottom.color;
  if (borders.left.color.alpha != 0) return borders.left.color;
  return element->css().get_color();
}

struct ScrollbarColors {
  litehtml::web_color thumb;
  litehtml::web_color track;
};

ScrollbarColors CssScrollbarColors(const litehtml::element* element) {
  if (element == nullptr) return {};
  const auto& colors = element->css().get_scrollbar_colors();
  if (!colors.auto_value) return {colors.thumb, colors.track};
  return {CssBorderColor(element), CssBackgroundColor(element)};
}

struct ScrollbarVisualState {
  const litehtml::element* dom_target = nullptr;
  const litehtml::render_item* target = nullptr;
  float opacity = 0.0f;
  double visible_until = 0.0;
};

void PushSafeClipRect(ImDrawList* draw_list, ImVec2 clip_min, ImVec2 clip_max, bool intersect = true) {
  ImVec2 current_min(0.0f, 0.0f);
  ImVec2 current_max(0.0f, 0.0f);
  if (!draw_list->_ClipRectStack.empty()) {
    current_min = draw_list->GetClipRectMin();
    current_max = draw_list->GetClipRectMax();
  }

  const bool current_is_valid = std::isfinite(current_min.x) && std::isfinite(current_min.y) &&
                                std::isfinite(current_max.x) && std::isfinite(current_max.y) &&
                                current_min.x <= current_max.x && current_min.y <= current_max.y;
  if (!current_is_valid) {
    current_min = ImVec2(0.0f, 0.0f);
    current_max = current_min;
  }

  if (!std::isfinite(clip_min.x) || !std::isfinite(clip_min.y) || !std::isfinite(clip_max.x) ||
      !std::isfinite(clip_max.y)) {
    clip_min = current_min;
    clip_max = current_max;
  } else {
    clip_max.x = std::max(clip_max.x, clip_min.x);
    clip_max.y = std::max(clip_max.y, clip_min.y);
  }

  if (intersect) {
    clip_min.x = std::max(clip_min.x, current_min.x);
    clip_min.y = std::max(clip_min.y, current_min.y);
    clip_max.x = std::min(clip_max.x, current_max.x);
    clip_max.y = std::min(clip_max.y, current_max.y);
  }

  clip_max.x = std::max(clip_max.x, clip_min.x);
  clip_max.y = std::max(clip_max.y, clip_min.y);
  draw_list->PushClipRect(clip_min, clip_max, false);
}

bool GetScrollbarGeometry(const ScrollState& state, const ImVec2& origin, bool vertical,
                          ScrollbarGeometry& geometry) {
  const litehtml::position& lane = vertical ? state.vertical_scrollbar_box : state.horizontal_scrollbar_box;
  const float width = static_cast<float>(lane.width);
  const float height = static_cast<float>(lane.height);
  if (width <= 0.0f || height <= 0.0f) return false;

  const float thickness = vertical ? width : height;
  const float end_inset = 0.0f;
  const float minimum_thumb = 0.0f;

  const ImVec2 box_min = origin + ImVec2(static_cast<float>(lane.x), static_cast<float>(lane.y));
  const ImVec2 box_max = box_min + ImVec2(width, height);
  const float viewport_width = std::max(1.0f, static_cast<float>(state.viewport_size.width));
  const float viewport_height = std::max(1.0f, static_cast<float>(state.viewport_size.height));
  if (vertical) {
    const float track_top = box_min.y + end_inset;
    const float track_bottom = box_max.y - end_inset;
    const float track_height = track_bottom - track_top;
    const float content_height = std::max(1.0f, static_cast<float>(state.content_size.height));
    if (track_height <= 0.0f) return false;
    const float thumb_height = std::min(track_height,
                                        std::max(minimum_thumb, track_height * viewport_height / content_height));
    const float travel = track_height - thumb_height;
    const float scroll_ratio = static_cast<float>(state.top) / std::max(1.0f, static_cast<float>(state.max_top));
    const float thumb_top = track_top + travel * ImClamp(scroll_ratio, 0.0f, 1.0f);
    const float track_left = box_min.x + std::max(0.0f, (width - thickness) * 0.5f);
    geometry.track_min = ImVec2(track_left, track_top);
    geometry.track_max = ImVec2(track_left + std::min(thickness, width), track_bottom);
    geometry.thumb_min = ImVec2(geometry.track_min.x, thumb_top);
    geometry.thumb_max = ImVec2(geometry.track_max.x, thumb_top + thumb_height);
  } else {
    const float track_left = box_min.x + end_inset;
    const float track_right = box_max.x - end_inset;
    const float track_width = track_right - track_left;
    const float content_width = std::max(1.0f, static_cast<float>(state.content_size.width));
    if (track_width <= 0.0f) return false;
    const float thumb_width = std::min(track_width,
                                       std::max(minimum_thumb, track_width * viewport_width / content_width));
    const float travel = track_width - thumb_width;
    const float scroll_ratio = static_cast<float>(state.left) / std::max(1.0f, static_cast<float>(state.max_left));
    const float thumb_left = track_left + travel * ImClamp(scroll_ratio, 0.0f, 1.0f);
    const float track_top = box_min.y + std::max(0.0f, (height - thickness) * 0.5f);
    geometry.track_min = ImVec2(track_left, track_top);
    geometry.track_max = ImVec2(track_right, track_top + std::min(thickness, height));
    geometry.thumb_min = ImVec2(thumb_left, geometry.track_min.y);
    geometry.thumb_max = ImVec2(thumb_left + thumb_width, geometry.track_max.y);
  }
  return true;
}
}  // namespace

class BrowserContainer : public litehtml::document_container {
 private:
  ImVec2 bottomRight = ImVec2(0, 0);
  std::string title = "Browser";
  std::string tooltip = "";
  std::string loadUrl = "";
  std::string currentUrl = "";
  std::vector<std::string> history = {};
  std::deque<std::string> pending_events;
  std::string hovered_id;
  float width;
  const Config* config = nullptr;
  ImGuiMouseCursor requested_cursor = ImGuiMouseCursor_Arrow;

  static std::string element_id(const litehtml::element::ptr& element) {
    for (auto current = element; current; current = current->parent()) {
      const char* id = current->get_attr("id", "");
      if (id != nullptr && id[0] != '\0') return id;
    }
    return {};
  }

  void queue_element_event(const char* event, const std::string& id) {
    if (event != nullptr && !id.empty()) pending_events.push_back("event:" + std::string(event) + ":" + id);
  }

  static ImU32 scrollbar_color(ImU32 color, float opacity) {
    const ImU32 alpha = static_cast<ImU32>(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFu) *
                                               ImClamp(opacity, 0.0f, 1.0f) +
                                           0.5f);
    return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
  }

  static void draw_scrollbars(const std::vector<ScrollState>& states,
                              const std::vector<ScrollbarVisualState>& visuals, const ImVec2& origin) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (draw_list == nullptr) return;

    for (std::size_t index = 0; index < states.size(); ++index) {
      const float opacity = index < visuals.size() ? visuals[index].opacity : 0.0f;
      if (opacity <= 0.001f) continue;
      const ScrollState& state = states[index];
      const float left = static_cast<float>(state.scroll_box.x);
      const float top = static_cast<float>(state.scroll_box.y);
      const float width = static_cast<float>(state.scroll_box.width);
      const float height = static_cast<float>(state.scroll_box.height);
      ScrollbarGeometry vertical;
      ScrollbarGeometry horizontal;
      const bool has_vertical = GetScrollbarGeometry(state, origin, true, vertical);
      const bool has_horizontal = GetScrollbarGeometry(state, origin, false, horizontal);
      if ((!has_vertical && !has_horizontal) || width <= 0.0f || height <= 0.0f) continue;

      const ImVec2 box_min = origin + ImVec2(left, top);
      const ImVec2 box_max = box_min + ImVec2(width, height);
      bool has_state_clip = false;
      if (state.has_clip) {
        const ImVec2 clip_min = origin + ImVec2(static_cast<float>(state.clip_box.x),
                                                static_cast<float>(state.clip_box.y));
        const ImVec2 clip_max = clip_min + ImVec2(static_cast<float>(state.clip_box.width),
                                                  static_cast<float>(state.clip_box.height));
        PushSafeClipRect(draw_list, clip_min, clip_max);
        has_state_clip = true;
      }
      PushSafeClipRect(draw_list, box_min, box_max);

      if (has_vertical) {
        const float radius = (vertical.track_max.x - vertical.track_min.x) * 0.5f;
        const auto scrollbar_colors = CssScrollbarColors(state.target.get());
        draw_list->AddRectFilled(vertical.track_min, vertical.track_max,
                                 scrollbar_color(IM_COL32(scrollbar_colors.track.red, scrollbar_colors.track.green,
                                                          scrollbar_colors.track.blue, scrollbar_colors.track.alpha),
                                                 opacity), radius);
        draw_list->AddRectFilled(vertical.thumb_min, vertical.thumb_max,
                                 scrollbar_color(IM_COL32(scrollbar_colors.thumb.red, scrollbar_colors.thumb.green,
                                                          scrollbar_colors.thumb.blue, scrollbar_colors.thumb.alpha),
                                                 opacity), radius);
      }

      if (has_horizontal) {
        const float radius = (horizontal.track_max.y - horizontal.track_min.y) * 0.5f;
        const auto scrollbar_colors = CssScrollbarColors(state.target.get());
        draw_list->AddRectFilled(horizontal.track_min, horizontal.track_max,
                                 scrollbar_color(IM_COL32(scrollbar_colors.track.red, scrollbar_colors.track.green,
                                                          scrollbar_colors.track.blue, scrollbar_colors.track.alpha),
                                                 opacity), radius);
        draw_list->AddRectFilled(horizontal.thumb_min, horizontal.thumb_max,
                                 scrollbar_color(IM_COL32(scrollbar_colors.thumb.red, scrollbar_colors.thumb.green,
                                                          scrollbar_colors.thumb.blue, scrollbar_colors.thumb.alpha),
                                                 opacity), radius);
      }

      draw_list->PopClipRect();
      if (has_state_clip) {
        draw_list->PopClipRect();
      }
    }
  }

 public:
  BrowserContainer(float width) : width(width) {}

  void reset() { bottomRight = ImVec2(0, 0); }
  ImVec2 get_bottom_right() { return bottomRight; }
  void push_bottom_right(ImVec2 point) {
    bottomRight.x = std::max(bottomRight.x, point.x);
    bottomRight.y = std::max(bottomRight.y, point.y);
  }
  std::string get_tooltip() { return tooltip; }
  std::string get_title() { return title; }
  std::string pop_load_url() {
    if (!loadUrl.empty()) {
      auto url = loadUrl;
      loadUrl = "";
      return url;
    }
    if (!pending_events.empty()) {
      auto event = std::move(pending_events.front());
      pending_events.pop_front();
      return event;
    }
    return "";
  }
  void go_back() {
    if (!history.empty()) {
      loadUrl = history.back();
      history.pop_back();
    }
  }
  bool can_go_back() { return !history.empty(); }
  void set_current_url(std::string url) { currentUrl = url; }
  std::string get_current_url() { return currentUrl; }
  void refresh() { loadUrl = currentUrl; }
  void set_config(const Config* value) { config = value; }
  void apply_requested_cursor() const {
    if (requested_cursor != ImGuiMouseCursor_Arrow && ImGui::IsWindowHovered()) {
      ImGui::SetMouseCursor(requested_cursor);
    }
  }
  void paint_scrollbars(const std::vector<ScrollState>& states,
                        const std::vector<ScrollbarVisualState>& visuals, const ImVec2& origin) {
    draw_scrollbars(states, visuals, origin);
  }

  //
  // Font functions
  //

  struct ResolvedFont {
    ImFont* Font = nullptr;
    FontStyle Style = FontStyle::Regular;
    std::string Family;
    float Size = 16.0f;
    litehtml::font_metrics Metrics{};
    std::unordered_map<std::string, litehtml::pixel_t> WidthCache;
  };

  std::unordered_map<litehtml::uint_ptr, std::unique_ptr<ResolvedFont>> fonts_;

  static ResolvedFont* from_handle(litehtml::uint_ptr hFont) { return reinterpret_cast<ResolvedFont*>(hFont); }

  virtual litehtml::uint_ptr create_font(const litehtml::font_description& descr, const litehtml::document* doc,
                                         litehtml::font_metrics* fm) override {
    bool bold = descr.weight > 400;
    bool italic = descr.style == litehtml::font_style_italic;

    FontStyle font_style = FontStyle::Regular;
    if (bold && italic) {
      font_style = FontStyle::BoldItalic;
    } else if (bold) {
      font_style = FontStyle::Bold;
    } else if (italic) {
      font_style = FontStyle::Italic;
    }

    ImFont* font = resolveFont(*config, descr.family, font_style);
    if (font != nullptr) {
      IMHTML_PRINTF("[ImHTML] Resolved font for weight=%i style=%i\n",
                    static_cast<int>(descr.weight),
                    static_cast<int>(descr.style));
    } else {
      IMHTML_PRINTF("[ImHTML] Failed to resolve font\n");
    }

    auto rf = std::make_unique<ResolvedFont>();
    rf->Font = font;
    rf->Style = font_style;
    rf->Family = descr.family;
    rf->Size = descr.size;

    const float base_size = font ? font->GetFontBaked(descr.size)->Size : ImGui::GetFontSize();
    const float scale = base_size > 0.0f ? (static_cast<float>(descr.size) / base_size) : 1.0f;

    rf->Metrics.font_size = (int)descr.size;
    rf->Metrics.height = (int)(base_size * scale);
    rf->Metrics.ascent = font ? (int)(font->GetFontBaked(descr.size)->Ascent * scale) : (int)(base_size * 0.8f);
    rf->Metrics.descent = font ? (int)(-font->GetFontBaked(descr.size)->Descent * scale) : (int)(base_size * 0.2f);
    rf->Metrics.x_height = rf->Metrics.ascent / 2;

    if (fm) {
      *fm = rf->Metrics;
    }

    ResolvedFont* raw = rf.get();
    const litehtml::uint_ptr handle = reinterpret_cast<litehtml::uint_ptr>(raw);
    fonts_.emplace(handle, std::move(rf));
    return handle;
  }

  virtual void delete_font(litehtml::uint_ptr hFont) override {
    fonts_.erase(hFont);
  }

  virtual litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override {
    auto* rf = from_handle(hFont);
    if (!rf || !rf->Font || !text) {
      return 0;
    }

    const auto cached = rf->WidthCache.find(text);
    if (cached != rf->WidthCache.end()) {
      return cached->second;
    }

    const char* end = text + strlen(text);
    ImVec2 size = rf->Font->CalcTextSizeA(rf->Size, FLT_MAX, 0.0f, text, end, nullptr);
    const litehtml::pixel_t width = static_cast<litehtml::pixel_t>(size.x);
    rf->WidthCache.emplace(text, width);
    return width;
  }

  virtual void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont, litehtml::web_color color,
                         const litehtml::position& pos) override {
    auto* rf = from_handle(hFont);
    if (!rf || !rf->Font || !text) {
      return;
    }

    ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(pos.x, pos.y);
    ImU32 col = IM_COL32(color.red, color.green, color.blue, color.alpha);

    ImGui::GetWindowDrawList()->AddText(rf->Font, rf->Size, p, col, text);

    // text_width() owns the per-font cache used during layout. Reuse it for
    // the paint-side bounds bookkeeping instead of measuring every visible
    // text run a second time on every frame.
    const litehtml::pixel_t measured_width = text_width(text, hFont);
    push_bottom_right(ImVec2(pos.x + static_cast<float>(measured_width),
                             pos.y + static_cast<float>(rf->Metrics.height)));
  }

  //
  // Measurement and defaults
  //

  virtual litehtml::pixel_t pt_to_px(float pt) const override { return pt; }
  virtual litehtml::pixel_t get_default_font_size() const override { return config->BaseFontSize; }
  virtual const char* get_default_font_name() const override { return "Default"; }

  //
  // Drawing functions
  //

  struct LayerGeometry {
    ImVec2 border_min;
    ImVec2 border_max;
    ImVec2 clip_min;
    ImVec2 clip_max;
    float tl, tr, br, bl;
  };

  LayerGeometry get_layer_geometry(const litehtml::background_layer& layer) const {
    ImVec2 screen_pos = ImGui::GetCursorScreenPos();

    LayerGeometry g;
    g.border_min = screen_pos + ImVec2((float)layer.border_box.x, (float)layer.border_box.y);
    g.border_max = screen_pos + ImVec2((float)(layer.border_box.x + layer.border_box.width),
                                       (float)(layer.border_box.y + layer.border_box.height));
    g.clip_min = screen_pos + ImVec2((float)layer.clip_box.x, (float)layer.clip_box.y);
    g.clip_max = screen_pos + ImVec2((float)(layer.clip_box.x + layer.clip_box.width),
                                     (float)(layer.clip_box.y + layer.clip_box.height));

    g.tl = (float)layer.border_radius.top_left_x;
    g.tr = (float)layer.border_radius.top_right_x;
    g.br = (float)layer.border_radius.bottom_right_x;
    g.bl = (float)layer.border_radius.bottom_left_x;
    return g;
  }

  virtual void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 center = ImGui::GetCursorScreenPos() +
                    ImVec2(marker.pos.x + marker.pos.width / 2.0f, marker.pos.y + marker.pos.height / 2.0f);
    float radius = marker.pos.width / 2.0f;
    ImU32 color = IM_COL32(marker.color.red, marker.color.green, marker.color.blue, marker.color.alpha);

    switch (marker.marker_type) {
      case litehtml::list_style_type_circle:
        draw_list->AddCircle(center, radius, color, 0, 1.5f);
        break;
      case litehtml::list_style_type_disc:
        draw_list->AddCircleFilled(center, radius, color);
        break;
      case litehtml::list_style_type_square: {
        ImVec2 p_min = ImGui::GetCursorScreenPos() + ImVec2(marker.pos.x, marker.pos.y);
        ImVec2 p_max = p_min + ImVec2(marker.pos.width, marker.pos.height);
        draw_list->AddRectFilled(p_min, p_max, color);
        break;
      }
      default:
        draw_list->AddCircleFilled(center, radius, color);
        break;
    }

    push_bottom_right(ImVec2(marker.pos.x + marker.pos.width, marker.pos.y + marker.pos.height));
  }

  virtual void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override {
    if (!config->LoadImage) {
      return;
    }

    config->LoadImage(src, baseurl);
  }

  virtual void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override {
    if (!config->GetImageMeta) {
      return;
    }

    auto image_meta = config->GetImageMeta(src, baseurl);
    sz.width = image_meta.Width;
    sz.height = image_meta.Height;
  }

  virtual void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer, const std::string& url,
                          const std::string& base_url) override {
    if (!config->GetImageTexture) {
      return;
    }

    LayerGeometry lgm = this->get_layer_geometry(layer);
    const int display_width = std::max(1, static_cast<int>(std::lround(lgm.border_max.x - lgm.border_min.x)));
    const int display_height = std::max(1, static_cast<int>(std::lround(lgm.border_max.y - lgm.border_min.y)));
    ImTextureID texture = config->GetImageTexture(url.c_str(), base_url.c_str(), display_width, display_height);
    if (!texture) {
      return;
    }

    ImVec2 p_min = lgm.border_min;
    ImVec2 p_max = lgm.border_max;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float radius = std::min({lgm.tl, lgm.tr, lgm.br, lgm.bl});

    PushSafeClipRect(draw_list, lgm.clip_min, lgm.clip_max);

    if (radius > 0.0f) {
      draw_list->AddImageRounded(texture, p_min, p_max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, lgm.tl);
    } else {
      draw_list->AddImage(texture, p_min, p_max);
    }

    draw_list->PopClipRect();

    push_bottom_right(ImVec2(lgm.border_max.x, lgm.border_max.y));
  }

  virtual void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                               const litehtml::web_color& color) override {
    if (color.alpha == 0) {
      return;
    }

    const litehtml::position& bg_box = layer.border_box;
    const litehtml::position& clip_box = layer.clip_box;

    if (bg_box.width <= litehtml::pixel_t(0) || bg_box.height <= litehtml::pixel_t(0) ||
        clip_box.width <= litehtml::pixel_t(0) || clip_box.height <= litehtml::pixel_t(0)) {
      return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    LayerGeometry lgm = this->get_layer_geometry(layer);

    ImU32 col = IM_COL32(color.red, color.green, color.blue, color.alpha);

    PushSafeClipRect(draw_list, lgm.clip_min, lgm.clip_max);

    if (lgm.tl == lgm.tr && lgm.tr == lgm.br && lgm.br == lgm.bl) {
      draw_list->AddRectFilled(lgm.border_min, lgm.border_max, col, lgm.tl);
    } else {
      draw_list->PathClear();

      if (lgm.tl > 0.0f)
        draw_list->PathArcTo(ImVec2(lgm.border_min.x + lgm.tl, lgm.border_min.y + lgm.tl), lgm.tl, IM_PI, IM_PI * 1.5f);
      else
        draw_list->PathLineTo(ImVec2(lgm.border_min.x, lgm.border_min.y));

      if (lgm.tr > 0.0f)
        draw_list->PathArcTo(
            ImVec2(lgm.border_max.x - lgm.tr, lgm.border_min.y + lgm.tr), lgm.tr, IM_PI * 1.5f, IM_PI * 2.0f);
      else
        draw_list->PathLineTo(ImVec2(lgm.border_max.x, lgm.border_min.y));

      if (lgm.br > 0.0f)
        draw_list->PathArcTo(ImVec2(lgm.border_max.x - lgm.br, lgm.border_max.y - lgm.br), lgm.br, 0.0f, IM_PI * 0.5f);
      else
        draw_list->PathLineTo(ImVec2(lgm.border_max.x, lgm.border_max.y));

      if (lgm.bl > 0.0f)
        draw_list->PathArcTo(ImVec2(lgm.border_min.x + lgm.bl, lgm.border_max.y - lgm.bl), lgm.bl, IM_PI * 0.5f, IM_PI);
      else
        draw_list->PathLineTo(ImVec2(lgm.border_min.x, lgm.border_max.y));

      draw_list->PathFillConvex(col);
    }

    draw_list->PopClipRect();

    push_bottom_right(ImVec2((float)(bg_box.x + bg_box.width), (float)(bg_box.y + bg_box.height)));
  }

  static constexpr float kEpsilon = 1e-6f;

  static ImU32 to_im_col32(const litehtml::web_color& c) { return IM_COL32(c.red, c.green, c.blue, c.alpha); }

  static litehtml::web_color sample_gradient_color(const std::vector<litehtml::background_layer::color_point>& points,
                                                   float t) {
    litehtml::web_color out{0, 0, 0, 0};

    if (points.empty()) {
      return out;
    }

    if (t <= points.front().offset) {
      return points.front().color;
    }

    if (t >= points.back().offset) {
      return points.back().color;
    }

    for (size_t i = 1; i < points.size(); ++i) {
      const auto& a = points[i - 1];
      const auto& b = points[i];

      if (t >= a.offset && t <= b.offset) {
        const float span = b.offset - a.offset;
        const float u = (span > 0.0f) ? ((t - a.offset) / span) : 0.0f;

        auto lerp_u8 = [u](unsigned char x, unsigned char y) -> unsigned char {
          return (unsigned char)(x + (y - x) * u);
        };

        out.red = lerp_u8(a.color.red, b.color.red);
        out.green = lerp_u8(a.color.green, b.color.green);
        out.blue = lerp_u8(a.color.blue, b.color.blue);
        out.alpha = lerp_u8(a.color.alpha, b.color.alpha);
        return out;
      }
    }

    return points.back().color;
  }

  static float cross2(const ImVec2& a, const ImVec2& b) { return a.x * b.y - a.y * b.x; }

  static ImVec2 line_intersection(const ImVec2& p1, const ImVec2& p2, const ImVec2& q1, const ImVec2& q2) {
    const ImVec2 r = p2 - p1;
    const ImVec2 s = q2 - q1;
    const float rxs = cross2(r, s);

    if (fabsf(rxs) < kEpsilon) {
      return p1;  // parallel fallback
    }

    const float t = cross2(q1 - p1, s) / rxs;
    return p1 + r * t;
  }

  static bool is_inside_edge(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    // For counter-clockwise clip polygon, inside is on the left side of edge AB.
    return cross2(b - a, p - a) >= 0.0f;
  }

  static std::vector<ImVec2> clip_polygon_convex(const std::vector<ImVec2>& subject_polygon,
                                                 const std::vector<ImVec2>& clip_polygon) {
    std::vector<ImVec2> result = subject_polygon;

    for (size_t edge_index = 0; edge_index < clip_polygon.size(); ++edge_index) {
      const ImVec2& clip_edge_start = clip_polygon[edge_index];
      const ImVec2& clip_edge_end = clip_polygon[(edge_index + 1) % clip_polygon.size()];

      if (result.empty()) {
        break;
      }

      std::vector<ImVec2> input = std::move(result);
      result.clear();

      ImVec2 previous_point = input.back();
      bool previous_inside = is_inside_edge(previous_point, clip_edge_start, clip_edge_end);

      for (const ImVec2& current_point : input) {
        const bool current_inside = is_inside_edge(current_point, clip_edge_start, clip_edge_end);

        if (current_inside) {
          if (!previous_inside) {
            result.push_back(line_intersection(previous_point, current_point, clip_edge_start, clip_edge_end));
          }

          result.push_back(current_point);

        } else if (previous_inside) {
          result.push_back(line_intersection(previous_point, current_point, clip_edge_start, clip_edge_end));
        }

        previous_point = current_point;
        previous_inside = current_inside;
      }
    }

    return result;
  }

  template <typename ColorFunc>
  static void draw_convex_shaded_polygon(ImDrawList* draw_list, const std::vector<ImVec2>& poly,
                                         ColorFunc&& color_for_point) {
    if (poly.size() < 3) {
      return;
    }

    const ImVec2 uv = draw_list->_Data->TexUvWhitePixel;
    const ImDrawIdx base = draw_list->_VtxCurrentIdx;
    const int vtx_count = (int)poly.size();
    const int idx_count = (vtx_count - 2) * 3;

    draw_list->PrimReserve(idx_count, vtx_count);

    for (int i = 1; i < vtx_count - 1; ++i) {
      draw_list->PrimWriteIdx(base + 0);
      draw_list->PrimWriteIdx(base + i);
      draw_list->PrimWriteIdx(base + i + 1);
    }

    for (const ImVec2& p : poly) {
      draw_list->PrimWriteVtx(p, uv, color_for_point(p));
    }
  }

  template <typename ColorFunc>
  static void draw_clipped_shaded_polygon(ImDrawList* draw_list, const std::vector<ImVec2>& poly,
                                          const std::vector<ImVec2>& clip_poly, ColorFunc&& color_for_point) {
    std::vector<ImVec2> clipped = clip_polygon_convex(poly, clip_poly);
    if (clipped.size() < 3) {
      return;
    }

    draw_convex_shaded_polygon(draw_list, clipped, std::forward<ColorFunc>(color_for_point));
  }

  static void append_point_if_distinct(std::vector<ImVec2>& pts, const ImVec2& p, float eps = 0.01f) {
    if (pts.empty()) {
      pts.push_back(p);
      return;
    }

    const ImVec2& last = pts.back();
    if (fabsf(last.x - p.x) > eps || fabsf(last.y - p.y) > eps) {
      pts.push_back(p);
    }
  }

  static void append_arc_points(std::vector<ImVec2>& pts, const ImVec2& center, float radius, float a_min, float a_max,
                                int segments, bool skip_first) {
    if (radius <= 0.0f || segments <= 0) {
      return;
    }

    for (int i = skip_first ? 1 : 0; i <= segments; ++i) {
      const float t = (float)i / (float)segments;
      const float a = a_min + (a_max - a_min) * t;
      append_point_if_distinct(pts, ImVec2(center.x + cosf(a) * radius, center.y + sinf(a) * radius));
    }
  }

  static std::vector<ImVec2> build_rect_polygon(const ImVec2& p_min, const ImVec2& p_max) {
    return {
        ImVec2(p_min.x, p_min.y),
        ImVec2(p_max.x, p_min.y),
        ImVec2(p_max.x, p_max.y),
        ImVec2(p_min.x, p_max.y),
    };
  }

  static std::vector<ImVec2> build_rounded_rect_polygon(const ImVec2& p_min, const ImVec2& p_max, float tl, float tr,
                                                        float br, float bl, int arc_segments = 8) {
    std::vector<ImVec2> pts;
    pts.reserve(4 * (arc_segments + 1));

    const float w = p_max.x - p_min.x;
    const float h = p_max.y - p_min.y;
    const float max_r = ImMin(w * 0.5f, h * 0.5f);

    tl = ImClamp(tl, 0.0f, max_r);
    tr = ImClamp(tr, 0.0f, max_r);
    br = ImClamp(br, 0.0f, max_r);
    bl = ImClamp(bl, 0.0f, max_r);

    append_point_if_distinct(pts, ImVec2(p_min.x + tl, p_min.y));
    append_point_if_distinct(pts, ImVec2(p_max.x - tr, p_min.y));

    if (tr > 0.0f) {
      append_arc_points(pts, ImVec2(p_max.x - tr, p_min.y + tr), tr, -IM_PI * 0.5f, 0.0f, arc_segments, true);
    }

    append_point_if_distinct(pts, ImVec2(p_max.x, p_max.y - br));

    if (br > 0.0f) {
      append_arc_points(pts, ImVec2(p_max.x - br, p_max.y - br), br, 0.0f, IM_PI * 0.5f, arc_segments, true);
    }

    append_point_if_distinct(pts, ImVec2(p_min.x + bl, p_max.y));

    if (bl > 0.0f) {
      append_arc_points(pts, ImVec2(p_min.x + bl, p_max.y - bl), bl, IM_PI * 0.5f, IM_PI, arc_segments, true);
    }

    append_point_if_distinct(pts, ImVec2(p_min.x, p_min.y + tl));

    if (tl > 0.0f) {
      append_arc_points(pts, ImVec2(p_min.x + tl, p_min.y + tl), tl, IM_PI, IM_PI * 1.5f, arc_segments, true);
    }

    return pts;
  }

  static bool has_rounded_corners(const LayerGeometry& lgm) {
    return lgm.tl > 0.0f || lgm.tr > 0.0f || lgm.br > 0.0f || lgm.bl > 0.0f;
  }

  static std::vector<ImVec2> build_layer_fill_polygon(const LayerGeometry& lgm, int arc_segments = 12) {
    if (has_rounded_corners(lgm)) {
      return build_rounded_rect_polygon(lgm.border_min, lgm.border_max, lgm.tl, lgm.tr, lgm.br, lgm.bl, arc_segments);
    }

    return build_rect_polygon(lgm.border_min, lgm.border_max);
  }

  static std::vector<ImVec2> build_ellipse_polygon(const ImVec2& center, float rx, float ry, float t, int segments) {
    std::vector<ImVec2> pts;
    pts.reserve(segments);

    const float ex = rx * t;
    const float ey = ry * t;

    for (int i = 0; i < segments; ++i) {
      const float a = ((float)i / (float)segments) * IM_PI * 2.0f;
      pts.push_back(ImVec2(center.x + cosf(a) * ex, center.y + sinf(a) * ey));
    }

    return pts;
  }

  static ImVec2 conic_point_on_circle(const ImVec2& center, float radius, float angle_deg) {
    const float a = angle_deg * IM_PI / 180.0f;

    // 0 degrees at top, clockwise positive
    const float x = sinf(a);
    const float y = -cosf(a);

    return ImVec2(center.x + x * radius, center.y + y * radius);
  }

  static std::vector<ImVec2> build_conic_wedge_polygon(const ImVec2& center, float radius, float angle0_deg,
                                                       float angle1_deg, int arc_segments) {
    std::vector<ImVec2> pts;
    pts.reserve(arc_segments + 3);

    pts.push_back(center);

    for (int i = 0; i <= arc_segments; ++i) {
      const float t = (float)i / (float)arc_segments;
      const float a = angle0_deg + (angle1_deg - angle0_deg) * t;
      pts.push_back(conic_point_on_circle(center, radius, a));
    }

    return pts;
  }

  void draw_linear_gradient_impl(const LayerGeometry& lgm,
                                 const litehtml::background_layer::linear_gradient& gradient) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    const ImVec2 start = screen_pos + ImVec2(gradient.start.x, gradient.start.y);
    const ImVec2 end = screen_pos + ImVec2(gradient.end.x, gradient.end.y);

    const ImVec2 axis = end - start;
    const float axis_len_sq = axis.x * axis.x + axis.y * axis.y;
    if (axis_len_sq <= 0.0001f) {
      return;
    }

    const float axis_len = sqrtf(axis_len_sq);
    const ImVec2 dir(axis.x / axis_len, axis.y / axis_len);
    const ImVec2 normal(-dir.y, dir.x);

    const float w = lgm.border_max.x - lgm.border_min.x;
    const float h = lgm.border_max.y - lgm.border_min.y;
    const float extent = sqrtf(w * w + h * h) + 2.0f;
    const float approx_span = ImMax(w, h);

    const int strips = (int)ImClamp(axis_len / 2.0f + approx_span / 4.0f, 16.0f, 128.0f);
    const std::vector<ImVec2> fill_poly = build_layer_fill_polygon(lgm, 8);

    auto color_for_point = [&](const ImVec2& p) -> ImU32 {
      float t = ((p.x - start.x) * axis.x + (p.y - start.y) * axis.y) / axis_len_sq;
      t = ImClamp(t, 0.0f, 1.0f);
      return to_im_col32(sample_gradient_color(gradient.color_points, t));
    };

    for (int i = 0; i < strips; ++i) {
      const float t0 = (float)i / (float)strips;
      const float t1 = (float)(i + 1) / (float)strips;

      const ImVec2 p0 = start + dir * (t0 * axis_len);
      const ImVec2 p1 = start + dir * (t1 * axis_len);

      const std::vector<ImVec2> strip_quad = {
          p0 - normal * extent,
          p0 + normal * extent,
          p1 + normal * extent,
          p1 - normal * extent,
      };

      draw_clipped_shaded_polygon(draw_list, strip_quad, fill_poly, color_for_point);
    }
  }

  void draw_radial_gradient_impl(const LayerGeometry& lgm,
                                 const litehtml::background_layer::radial_gradient& gradient) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    const ImVec2 center = screen_pos + ImVec2(gradient.position.x, gradient.position.y);

    const float rx = gradient.radius.x;
    const float ry = gradient.radius.y;

    if (rx <= 0.0001f || ry <= 0.0001f || gradient.color_points.empty()) {
      return;
    }

    const std::vector<ImVec2> fill_poly = build_layer_fill_polygon(lgm, 12);

    // Draw from outside to inside so smaller inner ellipses overwrite larger ones.
    const int ring_count = 64;
    const int ellipse_segments = 64;

    for (int i = ring_count; i >= 1; --i) {
      const float t = (float)i / (float)ring_count;
      const std::vector<ImVec2> ellipse = build_ellipse_polygon(center, rx, ry, t, ellipse_segments);

      std::vector<ImVec2> clipped = clip_polygon_convex(ellipse, fill_poly);
      if (clipped.size() < 3) {
        continue;
      }

      const ImU32 col = to_im_col32(sample_gradient_color(gradient.color_points, t));
      draw_convex_shaded_polygon(draw_list, clipped, [&](const ImVec2&) -> ImU32 { return col; });
    }

    // Fill the center with t=0 color.
    {
      const std::vector<ImVec2> center_poly =
          build_ellipse_polygon(center, rx, ry, 1.0f / (float)ring_count, ellipse_segments);

      std::vector<ImVec2> clipped = clip_polygon_convex(center_poly, fill_poly);
      if (clipped.size() >= 3) {
        const ImU32 col = to_im_col32(sample_gradient_color(gradient.color_points, 0.0f));
        draw_convex_shaded_polygon(draw_list, clipped, [&](const ImVec2&) -> ImU32 { return col; });
      }
    }
  }

  void draw_conic_gradient_impl(const LayerGeometry& lgm, const litehtml::background_layer::conic_gradient& gradient) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    const ImVec2 center = screen_pos + ImVec2(gradient.position.x, gradient.position.y);

    const float radius = gradient.radius;
    if (radius <= 0.0001f || gradient.color_points.empty()) {
      return;
    }

    const std::vector<ImVec2> fill_poly = build_layer_fill_polygon(lgm, 12);

    const int wedge_count = 128;
    const int arc_segments_per_wedge = 1;

    for (int i = 0; i < wedge_count; ++i) {
      const float t0 = (float)i / (float)wedge_count;
      const float t1 = (float)(i + 1) / (float)wedge_count;

      const float a0 = gradient.angle + t0 * 360.0f;
      const float a1 = gradient.angle + t1 * 360.0f;

      const std::vector<ImVec2> wedge = build_conic_wedge_polygon(center, radius, a0, a1, arc_segments_per_wedge);

      std::vector<ImVec2> clipped = clip_polygon_convex(wedge, fill_poly);
      if (clipped.size() < 3) {
        continue;
      }

      const ImU32 col = to_im_col32(sample_gradient_color(gradient.color_points, t0));

      draw_convex_shaded_polygon(draw_list, clipped, [&](const ImVec2&) -> ImU32 { return col; });
    }
  }

  template <typename Gradient, typename DrawFn>
  void draw_gradient_common(litehtml::uint_ptr hdc, const litehtml::background_layer& layer, const Gradient& gradient,
                            DrawFn&& draw_fn) {
    const litehtml::position& bg_box = layer.border_box;
    const litehtml::position& clip_box = layer.clip_box;

    if (bg_box.width <= litehtml::pixel_t(0) || bg_box.height <= litehtml::pixel_t(0) ||
        clip_box.width <= litehtml::pixel_t(0) || clip_box.height <= litehtml::pixel_t(0)) {
      return;
    }

    if (gradient.color_points.empty()) {
      return;
    }

    LayerGeometry lgm = this->get_layer_geometry(layer);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    PushSafeClipRect(draw_list, lgm.clip_min, lgm.clip_max);
    draw_fn(lgm, gradient);
    draw_list->PopClipRect();

    push_bottom_right(ImVec2((float)(bg_box.x + bg_box.width), (float)(bg_box.y + bg_box.height)));
  }

  virtual void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                                    const litehtml::background_layer::linear_gradient& gradient) override {
    const ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    const ImVec2 start = screen_pos + ImVec2(gradient.start.x, gradient.start.y);
    const ImVec2 end = screen_pos + ImVec2(gradient.end.x, gradient.end.y);
    const ImVec2 axis = end - start;
    const float axis_len_sq = axis.x * axis.x + axis.y * axis.y;

    if (axis_len_sq <= 0.0001f) {
      if (!gradient.color_points.empty()) {
        draw_solid_fill(hdc, layer, gradient.color_points.back().color);
      }
      return;
    }

    draw_gradient_common(
        hdc, layer, gradient, [&](const LayerGeometry& lgm, const auto& g) { draw_linear_gradient_impl(lgm, g); });
  }

  virtual void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                                    const litehtml::background_layer::radial_gradient& gradient) override {
    draw_gradient_common(
        hdc, layer, gradient, [&](const LayerGeometry& lgm, const auto& g) { draw_radial_gradient_impl(lgm, g); });
  }

  virtual void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                                   const litehtml::background_layer::conic_gradient& gradient) override {
    draw_gradient_common(
        hdc, layer, gradient, [&](const LayerGeometry& lgm, const auto& g) { draw_conic_gradient_impl(lgm, g); });
  }

  virtual void on_mouse_event(const litehtml::element::ptr& el, litehtml::mouse_event event) override {
    if (el != nullptr && ImGui::IsWindowHovered()) {
      const char* attr = el->get_attr("tooltip");
      if (event == litehtml::mouse_event_enter) {
        if (attr != nullptr) {
          tooltip = std::string(attr);
        } else {
          const char* tag = el->get_tagName();
          if (tag != nullptr) {
            if (config->AllowHrefTooltips && std::string(tag) == "a" && (attr = el->get_attr("href")) != nullptr) {
              tooltip = std::string(attr);
            } else if (config->AllowImgAltTooltips && std::string(tag) == "img" &&
                       (attr = el->get_attr("alt")) != nullptr) {
              tooltip = std::string(attr);
            }
          }
        }
      } else if (event == litehtml::mouse_event_leave) {
        tooltip = "";
      }
    } else {
      tooltip = "";
    }

    if (event == litehtml::mouse_event_enter) {
      const std::string id = element_id(el);
      if (id != hovered_id) {
        if (!hovered_id.empty()) queue_element_event("mouse-exit", hovered_id);
        hovered_id = id;
        if (!hovered_id.empty()) queue_element_event("mouse-enter", hovered_id);
      }
    } else if (event == litehtml::mouse_event_leave && !ImGui::IsWindowHovered()) {
      if (!hovered_id.empty()) queue_element_event("mouse-exit", hovered_id);
      hovered_id.clear();
    }

    if (event == litehtml::mouse_event_enter && el != nullptr) {
      const std::string cursor = el->css().get_cursor();
      if (cursor == "pointer") {
        requested_cursor = ImGuiMouseCursor_Hand;
      } else {
        requested_cursor = ImGuiMouseCursor_Arrow;
      }
      apply_requested_cursor();
    } else if (event == litehtml::mouse_event_leave) {
      requested_cursor = ImGuiMouseCursor_Arrow;
    }
  }

  virtual void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders,
                            const litehtml::position& draw_pos, bool root) override {
    ImVec2 base_pos = ImGui::GetCursorScreenPos();
    ImVec2 top_left = base_pos + ImVec2(draw_pos.x, draw_pos.y);
    ImVec2 top_right = base_pos + ImVec2(draw_pos.x + draw_pos.width, draw_pos.y);
    ImVec2 bottom_right = base_pos + ImVec2(draw_pos.x + draw_pos.width, draw_pos.y + draw_pos.height);
    ImVec2 bottom_left = base_pos + ImVec2(draw_pos.x, draw_pos.y + draw_pos.height);

    auto* draw_list = ImGui::GetWindowDrawList();

    // Check if all sides and colors are equal
    if (borders.top.width == borders.right.width && borders.top.width == borders.bottom.width &&
        borders.top.width == borders.left.width && borders.top.color == borders.right.color &&
        borders.top.color == borders.bottom.color && borders.top.color == borders.left.color) {
      float w = borders.top.width;
      if (w > 0) {
        // ImGui path strokes are centered. We must offset the path inward by half the width
        // to conform to the CSS Box Model (borders grow inwards from the bounding box).
        float half_w = w * 0.5f;
        ImVec2 p_min = top_left + ImVec2(half_w, half_w);
        ImVec2 p_max = bottom_right - ImVec2(half_w, half_w);

        // We also must reduce the border radius by half the width so the outer edge matches CSS.
        float tl = std::max(0.0f, (float)borders.radius.top_left_x - half_w);
        float tr = std::max(0.0f, (float)borders.radius.top_right_x - half_w);
        float br = std::max(0.0f, (float)borders.radius.bottom_right_x - half_w);
        float bl = std::max(0.0f, (float)borders.radius.bottom_left_x - half_w);

        ImU32 color =
            IM_COL32(borders.top.color.red, borders.top.color.green, borders.top.color.blue, borders.top.color.alpha);

        if (tl == tr && tr == br && br == bl) {
          draw_list->AddRect(p_min, p_max, color, tl, 0, w);
        } else {
          draw_list->PathClear();
          if (tl > 0.0f)
            draw_list->PathArcTo(ImVec2(p_min.x + tl, p_min.y + tl), tl, IM_PI, IM_PI * 1.5f);
          else
            draw_list->PathLineTo(ImVec2(p_min.x, p_min.y));

          if (tr > 0.0f)
            draw_list->PathArcTo(ImVec2(p_max.x - tr, p_min.y + tr), tr, IM_PI * 1.5f, IM_PI * 2.0f);
          else
            draw_list->PathLineTo(ImVec2(p_max.x, p_min.y));

          if (br > 0.0f)
            draw_list->PathArcTo(ImVec2(p_max.x - br, p_max.y - br), br, 0.0f, IM_PI * 0.5f);
          else
            draw_list->PathLineTo(ImVec2(p_max.x, p_max.y));

          if (bl > 0.0f)
            draw_list->PathArcTo(ImVec2(p_min.x + bl, p_max.y - bl), bl, IM_PI * 0.5f, IM_PI);
          else
            draw_list->PathLineTo(ImVec2(p_min.x, p_max.y));

          draw_list->PathStroke(color, ImDrawFlags_Closed, w);
        }
      }
    } else {
      // The Non-Uniform Path (Mitered Borders via Quads)
      auto color32 = [](const litehtml::web_color& c) { return IM_COL32(c.red, c.green, c.blue, c.alpha); };

      // Top border
      if (borders.top.width > litehtml::pixel_t(0)) {
        draw_list->AddQuadFilled(top_left,
                                 ImVec2(bottom_right.x, top_left.y),
                                 ImVec2(bottom_right.x - borders.right.width, top_left.y + borders.top.width),
                                 ImVec2(top_left.x + borders.left.width, top_left.y + borders.top.width),
                                 color32(borders.top.color));
      }

      // Bottom border
      if (borders.bottom.width > litehtml::pixel_t(0)) {
        draw_list->AddQuadFilled(ImVec2(top_left.x + borders.left.width, bottom_right.y - borders.bottom.width),
                                 ImVec2(bottom_right.x - borders.right.width, bottom_right.y - borders.bottom.width),
                                 bottom_right,
                                 ImVec2(top_left.x, bottom_right.y),
                                 color32(borders.bottom.color));
      }

      // Left border
      if (borders.left.width > litehtml::pixel_t(0)) {
        draw_list->AddQuadFilled(top_left,
                                 ImVec2(top_left.x + borders.left.width, top_left.y + borders.top.width),
                                 ImVec2(top_left.x + borders.left.width, bottom_right.y - borders.bottom.width),
                                 ImVec2(top_left.x, bottom_right.y),
                                 color32(borders.left.color));
      }

      // Right border
      if (borders.right.width > litehtml::pixel_t(0)) {
        draw_list->AddQuadFilled(ImVec2(bottom_right.x - borders.right.width, top_left.y + borders.top.width),
                                 ImVec2(bottom_right.x, top_left.y),
                                 bottom_right,
                                 ImVec2(bottom_right.x - borders.right.width, bottom_right.y - borders.bottom.width),
                                 color32(borders.right.color));
      }
    }

    push_bottom_right(ImVec2(draw_pos.x + draw_pos.width, draw_pos.y + draw_pos.height));
  }
  //
  // Document related functions
  //

  virtual void set_caption(const char* caption) override { title = caption; }
  virtual void set_base_url(const char* base_url) override {}
  virtual void link(const std::shared_ptr<litehtml::document>& doc, const litehtml::element::ptr& el) override {}
  virtual void on_anchor_click(const char* url, const litehtml::element::ptr& el) override {
    history.push_back(currentUrl);
    loadUrl = url;
  }
  virtual bool on_element_click(const litehtml::element::ptr& element) override {
    for (auto current = element; current; current = current->parent()) {
      const char* tag = current->get_tagName();
      if (tag != nullptr && std::strcmp(tag, "select") == 0) {
        return true;
      }
      if (tag != nullptr && std::strcmp(tag, "input") == 0 &&
          std::strcmp(current->get_attr("type", ""), "text") == 0) {
        return true;
      }
    }
    for (auto current = element; current; current = current->parent()) {
      const char* tag = current->get_tagName();
      if (tag != nullptr && std::strcmp(tag, "input") == 0 &&
          std::strcmp(current->get_attr("type", ""), "range") == 0) {
        return true;
      }
    }
    for (auto current = element; current; current = current->parent()) {
      const char* tag = current->get_tagName();
      if (tag == nullptr || std::strcmp(tag, "input") != 0 ||
          std::strcmp(current->get_attr("type", ""), "checkbox") != 0) {
        continue;
      }
      const char* id = current->get_attr("id", "");
      if (id[0] != '\0') {
        loadUrl = "event:toggle:" + std::string(id);
        return true;
      }
    }
    for (auto current = element; current; current = current->parent()) {
      const char* tag = current->get_tagName();
      if (tag == nullptr || std::strcmp(tag, "label") != 0) continue;
      const auto checkbox = current->select_one("input[type=checkbox]");
      if (!checkbox) continue;
      const char* id = checkbox->get_attr("id", "");
      if (id[0] != '\0') {
        loadUrl = "event:toggle:" + std::string(id);
        return true;
      }
    }

    for (auto current = element; current; current = current->parent()) {
      const char* tag = current->get_tagName();
      if (tag != nullptr && customElements.find(tag) != customElements.end()) {
        return true;
      }
    }

    for (auto current = element; current; current = current->parent()) {
      const char* id = current->get_attr("id", "");
      if (id == nullptr || id[0] == '\0') continue;
      loadUrl = "event:click:" + std::string(id);
      return true;
    }
    return false;
  }
  virtual void set_cursor(const char* cursor) override {
    requested_cursor = ImGuiMouseCursor_Arrow;
    if (cursor != nullptr && std::strcmp(cursor, "pointer") == 0) {
      requested_cursor = ImGuiMouseCursor_Hand;
    } else if (cursor != nullptr && std::strcmp(cursor, "move") == 0) {
      requested_cursor = ImGuiMouseCursor_ResizeAll;
    }
    apply_requested_cursor();
  }
  virtual void transform_text(std::string& text, litehtml::text_transform tt) override {}
  virtual void import_css(std::string& text, const std::string& url, std::string& baseurl) override {
    if (!config->LoadCSS) {
      return;
    }
    text = config->LoadCSS(url.c_str(), baseurl.c_str());
  }

  //
  // Clipping functions
  //

  virtual void set_clip(const litehtml::position& pos, const litehtml::border_radiuses&) override {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (draw_list == nullptr) return;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 clip_min = cursor + ImVec2(pos.x, pos.y);
    const ImVec2 clip_max = clip_min + ImVec2(pos.width, pos.height);
    PushSafeClipRect(draw_list, clip_min, clip_max);
  }
  virtual void del_clip() override {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (draw_list == nullptr) return;
    draw_list->PopClipRect();
  }

  //
  // Layout functions
  //

  virtual void get_viewport(litehtml::position& client) const override {
    client.x = 0;
    client.y = 0;
    client.width = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    client.height = ImGui::GetContentRegionAvail().y;
  }

  virtual litehtml::element::ptr create_element(const char* tag_name, const litehtml::string_map& attributes,
                                                const std::shared_ptr<litehtml::document>& doc) override {
    if (std::strcmp(tag_name, "input") == 0) {
      const auto type = attributes.find("type");
      if (type != attributes.end() &&
          (type->second == "checkbox" || type->second == "range" || type->second == "text")) {
        return nullptr;
      }
    }
    if (customElements.find(tag_name) != customElements.end()) {
      return std::make_shared<CustomElement>(doc, tag_name, attributes);
    }

    return nullptr;
  }

  virtual void get_media_features(litehtml::media_features& media) const override {
    media.color = 8;
    media.resolution = 96;
    media.width = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    media.height = ImGui::GetContentRegionAvail().y;
    media.device_width = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    media.device_height = ImGui::GetContentRegionAvail().y;
    media.type = litehtml::media_type_screen;
  }

  virtual void get_language(std::string& language, std::string& culture) const override {
    language = "en";
    culture = "US";
  }
};

Config* GetConfig() { return &config; }
void SetConfig(const Config& newConfig) { config = newConfig; }
void PushConfig(const Config& config) { configStack.push_back(config); }
void PopConfig() {
  assert(!configStack.empty());
  configStack.pop_back();
}

void RegisterCustomElement(const char* tagName, CustomElementDrawFunction draw) { customElements[tagName] = std::move(draw); }

void UnregisterCustomElement(const char* tagName) {
  customElements.erase(tagName);
}

void RegisterCustomElementHtml(const char* tagName, CustomElementHtmlFunction render_html) {
  customElementHtmls[tagName] = std::move(render_html);
}

void UnregisterCustomElementHtml(const char* tagName) { customElementHtmls.erase(tagName); }

namespace {

bool IsAttributeSpace(char value) { return std::isspace(static_cast<unsigned char>(value)) != 0; }

void ReplaceAll(std::string& value, std::string_view from, std::string_view to) {
  std::size_t offset = 0;
  while ((offset = value.find(from, offset)) != std::string::npos) {
    value.replace(offset, from.size(), to);
    offset += to.size();
  }
}

std::string DecodeAttributeEntities(std::string value) {
  ReplaceAll(value, "&amp;", "&");
  ReplaceAll(value, "&quot;", "\"");
  ReplaceAll(value, "&apos;", "'");
  ReplaceAll(value, "&lt;", "<");
  ReplaceAll(value, "&gt;", ">");
  return value;
}

std::map<std::string, std::string> ParseCustomAttributes(std::string_view source) {
  std::map<std::string, std::string> attributes;
  std::size_t offset = 0;
  while (offset < source.size()) {
    while (offset < source.size() && (IsAttributeSpace(source[offset]) || source[offset] == '/')) ++offset;
    if (offset >= source.size()) break;

    const std::size_t name_start = offset;
    while (offset < source.size() && !IsAttributeSpace(source[offset]) && source[offset] != '=' && source[offset] != '/') {
      ++offset;
    }
    const std::string name(source.substr(name_start, offset - name_start));
    while (offset < source.size() && IsAttributeSpace(source[offset])) ++offset;

    std::string value;
    bool has_value = false;
    if (offset < source.size() && source[offset] == '=') {
      has_value = true;
      ++offset;
      while (offset < source.size() && IsAttributeSpace(source[offset])) ++offset;
      if (offset < source.size() && (source[offset] == '\"' || source[offset] == '\'')) {
        const char quote = source[offset++];
        const std::size_t value_start = offset;
        while (offset < source.size() && source[offset] != quote) ++offset;
        value = std::string(source.substr(value_start, offset - value_start));
        if (offset < source.size()) ++offset;
      } else {
        const std::size_t value_start = offset;
        while (offset < source.size() && !IsAttributeSpace(source[offset])) ++offset;
        value = std::string(source.substr(value_start, offset - value_start));
      }
    }
    if (!has_value) value = "true";
    if (!name.empty()) attributes.emplace(name, DecodeAttributeEntities(std::move(value)));
  }
  return attributes;
}

std::size_t FindTagEnd(std::string_view source, std::size_t offset) {
  char quote = 0;
  for (; offset < source.size(); ++offset) {
    const char value = source[offset];
    if (quote != 0) {
      if (value == quote) quote = 0;
    } else if (value == '\"' || value == '\'') {
      quote = value;
    } else if (value == '>') {
      return offset;
    }
  }
  return std::string::npos;
}

struct ParsedTag {
  std::string name;
  std::map<std::string, std::string> attributes;
  std::size_t end = std::string::npos;
  bool closing = false;
  bool self_closing = false;
};

bool IsVoidTag(std::string_view tag) {
  for (const std::string_view name : {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
                                      "meta", "param", "source", "track", "wbr"}) {
    if (tag == name) return true;
  }
  return false;
}

bool ParseTag(std::string_view source, std::size_t start, ParsedTag& result) {
  if (start >= source.size() || source[start] != '<') return false;
  const std::size_t end = FindTagEnd(source, start + 1);
  if (end == std::string::npos) return false;

  std::size_t offset = start + 1;
  if (offset < end && source[offset] == '/') {
    result.closing = true;
    ++offset;
  }
  while (offset < end && IsAttributeSpace(source[offset])) ++offset;
  if (offset == end || source[offset] == '!' || source[offset] == '?') return false;

  const std::size_t name_start = offset;
  while (offset < end && !IsAttributeSpace(source[offset]) && source[offset] != '/' && source[offset] != '>') ++offset;
  if (offset == name_start) return false;
  result.name = source.substr(name_start, offset - name_start);
  result.end = end;
  if (result.closing) return true;

  std::size_t attributes_end = end;
  while (attributes_end > offset && IsAttributeSpace(source[attributes_end - 1])) --attributes_end;
  result.self_closing = attributes_end > offset && source[attributes_end - 1] == '/';
  if (result.self_closing) {
    --attributes_end;
    while (attributes_end > offset && IsAttributeSpace(source[attributes_end - 1])) --attributes_end;
  }
  result.attributes = ParseCustomAttributes(std::string_view(source).substr(offset, attributes_end - offset));
  return true;
}

std::size_t FindMatchingTag(std::string_view source, const ParsedTag& opening) {
  if (opening.self_closing || IsVoidTag(opening.name)) return std::string::npos;

  int depth = 1;
  std::size_t search = opening.end + 1;
  while ((search = source.find('<', search)) != std::string::npos) {
    ParsedTag candidate;
    if (!ParseTag(source, search, candidate)) {
      ++search;
      continue;
    }
    if (candidate.name == opening.name) {
      if (candidate.closing) {
        if (--depth == 0) return search;
      } else if (!candidate.self_closing && !IsVoidTag(candidate.name)) {
        ++depth;
      }
    }
    search = candidate.end + 1;
  }
  return std::string::npos;
}

std::string ExpandCustomElementRange(std::string_view source, std::string_view parent_tag,
                                     const std::map<std::string, std::string>* parent_attributes,
                                     const CustomElementHtmlFunction* template_renderer) {
  std::string result;
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    const std::size_t open = source.find('<', cursor);
    if (open == std::string_view::npos) {
      result.append(source.substr(cursor));
      break;
    }
    result.append(source.substr(cursor, open - cursor));

    ParsedTag tag;
    const std::size_t source_open = open;
    if (!ParseTag(source, source_open, tag)) {
      result.push_back(source[open]);
      cursor = open + 1;
      continue;
    }
    const std::size_t tag_size = tag.end - source_open + 1;
    const auto custom = customElementHtmls.find(tag.name);
    const bool is_template = tag.name == "template" && template_renderer != nullptr;
    if (tag.closing || (!is_template && custom == customElementHtmls.end())) {
      if (tag.closing || tag.self_closing || IsVoidTag(tag.name)) {
        result.append(source.substr(open, tag_size));
        cursor = open + tag_size;
        continue;
      }

      const std::size_t close = FindMatchingTag(source, tag);
      if (close == std::string::npos) {
        result.append(source.substr(open));
        break;
      }
      ParsedTag closing;
      if (!ParseTag(source, close, closing)) {
        result.append(source.substr(open));
        break;
      }
      result.append(source.substr(open, tag_size));
      result += ExpandCustomElementRange(source.substr(open + tag_size, close - open - tag_size), tag.name,
                                         &tag.attributes, template_renderer);
      const std::size_t close_size = closing.end - close + 1;
      result.append(source.substr(close, close_size));
      cursor = close + close_size;
      continue;
    }

    std::string_view children;
    std::size_t replacement_end = open + tag_size;
    if (!tag.self_closing && !IsVoidTag(tag.name)) {
      const std::size_t close = FindMatchingTag(source, tag);
      if (close == std::string::npos) {
        result.append(source.substr(open));
        break;
      }
      ParsedTag closing;
      if (!ParseTag(source, close, closing)) {
        result.append(source.substr(open));
        break;
      }
      children = source.substr(open + tag_size, close - open - tag_size);
      replacement_end = closing.end + 1;
    }

    const std::string expanded_children =
        ExpandCustomElementRange(children, tag.name, &tag.attributes, template_renderer);
    const HtmlElementContext context{parent_tag, parent_attributes};
    const std::string replacement = is_template ? (*template_renderer)(tag.attributes, expanded_children, context)
                                                : custom->second(tag.attributes, expanded_children, context);
    result += replacement;
    cursor = replacement_end;
  }
  return result;
}
}  // namespace

std::string ExpandCustomElements(const std::string& html) {
  std::string expanded = html;
  for (int pass = 0; pass < 8; ++pass) {
    const std::string before = expanded;
    expanded = ExpandCustomElementRange(expanded, {}, nullptr, nullptr);
    const bool changed = expanded != before;
    if (!changed) break;
  }
  return expanded;
}

std::string ExpandCustomElements(const std::string& html, CustomElementHtmlFunction template_renderer) {
  std::string expanded = html;
  for (int pass = 0; pass < 8; ++pass) {
    const std::string before = expanded;
    expanded = ExpandCustomElementRange(expanded, {}, nullptr, &template_renderer);
    if (expanded == before) break;
  }
  return expanded;
}

std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes) {
  return ExpandHtmlTemplate(html_template, attributes, {});
}

namespace {
struct InjaTemplateCache {
  inja::Environment environment;
  std::unordered_map<std::string, inja::Template> templates;

  InjaTemplateCache() { environment.set_html_autoescape(false); }
};

InjaTemplateCache& GetInjaTemplateCache() {
  static InjaTemplateCache cache;
  return cache;
}

const inja::Template& GetInjaTemplate(std::string_view source) {
  InjaTemplateCache& cache = GetInjaTemplateCache();
  const std::string key(source);
  const auto found = cache.templates.find(key);
  if (found != cache.templates.end()) return found->second;
  return cache.templates.emplace(key, cache.environment.parse(source)).first->second;
}
}  // namespace

void PrepareHtmlTemplate(std::string_view html_template) { (void)GetInjaTemplate(html_template); }

std::string ExpandHtmlTemplate(std::string_view html_template, const inja::json& data) {
  return GetInjaTemplateCache().environment.render(GetInjaTemplate(html_template), data);
}

std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes,
                               std::string_view children) {
  inja::json data = inja::json::object();
  for (const auto& [name, value] : attributes) data[name] = value;
  data["children"] = std::string(children);
  return ExpandHtmlTemplate(html_template, data);
}

float ScrollbarSizePixels(const litehtml::scrollbar_width width) {
  constexpr float auto_size = 12.0f;
  switch (width) {
    case litehtml::scrollbar_width_thin:
      return auto_size * (2.0f / 3.0f);
    case litehtml::scrollbar_width_none:
      return 0.0f;
    case litehtml::scrollbar_width_auto:
    default:
      return auto_size;
  }
}

namespace {
void CollectScrollStatesRecursive(const std::shared_ptr<litehtml::render_item>& item,
                                  const litehtml::position* clip,
                                  std::vector<ScrollState>& states) {
  if (!item || !item->src_el()) return;

  const auto& element = item->src_el();
  const auto overflow = element->css().get_overflow();
  const bool scroll_container = element->css().get_display() != litehtml::display_inline &&
                                (overflow == litehtml::overflow_scroll || overflow == litehtml::overflow_auto);

  litehtml::position current_clip;
  const litehtml::position* child_clip = clip;
  if (overflow > litehtml::overflow_visible && element->css().get_display() != litehtml::display_inline) {
    current_clip = item->get_placement();
    current_clip = clip ? clip->intersect(current_clip) : current_clip;
    child_clip = &current_clip;
  }

  if (scroll_container) {
    ScrollState state;
    state.target = element;
    state.render_target = item;
    state.scroll_box = item->get_placement();
    state.viewport_box = state.scroll_box;
    state.viewport_size = litehtml::size(state.viewport_box.width, state.viewport_box.height);
    state.max_left = item->get_max_scroll_left();
    state.max_top = item->get_max_scroll_top();
    state.left = item->get_scroll_left();
    state.top = item->get_scroll_top();
    state.content_size = litehtml::size(state.viewport_size.width + state.max_left,
                                        state.viewport_size.height + state.max_top);

    const float thickness = ScrollbarSizePixels(element->css().get_scrollbar_width());
    const litehtml::pixel_t scrollbar_size(thickness);
    const bool show_vertical = thickness > 0.0f &&
                               (overflow == litehtml::overflow_scroll || state.max_top > litehtml::pixel_t(0));
    const bool show_horizontal = thickness > 0.0f &&
                                 (overflow == litehtml::overflow_scroll || state.max_left > litehtml::pixel_t(0));
    if (show_vertical) {
      state.vertical_scrollbar_box = state.scroll_box;
      state.vertical_scrollbar_box.x = state.scroll_box.right() - scrollbar_size;
      state.vertical_scrollbar_box.width = scrollbar_size;
      state.vertical_scrollbar_box.height = std::max(
          litehtml::pixel_t(0), state.scroll_box.height - (show_horizontal ? scrollbar_size : litehtml::pixel_t(0)));
    }
    if (show_horizontal) {
      state.horizontal_scrollbar_box = state.scroll_box;
      state.horizontal_scrollbar_box.y = state.scroll_box.bottom() - scrollbar_size;
      state.horizontal_scrollbar_box.width = std::max(
          litehtml::pixel_t(0), state.scroll_box.width - (show_vertical ? scrollbar_size : litehtml::pixel_t(0)));
      state.horizontal_scrollbar_box.height = scrollbar_size;
    }
    if (clip) {
      state.has_clip = true;
      state.clip_box = *clip;
    }
    states.push_back(std::move(state));
  }

  for (const auto& child : item->children()) {
    CollectScrollStatesRecursive(child, child_clip, states);
  }
}
}  // namespace

void CollectScrollStates(const std::shared_ptr<litehtml::document>& document,
                         std::vector<ScrollState>& states) {
  states.clear();
  if (document && document->root_render()) {
    CollectScrollStatesRecursive(document->root_render(), nullptr, states);
  }
}

void RefreshScrollStateOffsets(std::vector<ScrollState>& states) {
  for (ScrollState& state : states) {
    if (!state.render_target) continue;
    state.left = state.render_target->get_scroll_left();
    state.top = state.render_target->get_scroll_top();
    state.max_left = state.render_target->get_max_scroll_left();
    state.max_top = state.render_target->get_max_scroll_top();
    state.content_size = litehtml::size(state.viewport_size.width + state.max_left,
                                        state.viewport_size.height + state.max_top);
  }
}

namespace {
struct CanvasState {
  std::shared_ptr<BrowserContainer> container;
  std::shared_ptr<litehtml::document> doc;
  std::string html;
  long long last_active_time;
  int layout_width = -1;
  int layout_height = -1;
  bool layout_dirty = true;
  std::shared_ptr<litehtml::render_item> active_scroll_target;
  bool active_scroll_vertical = false;
  float scroll_grab_offset = 0.0f;
  std::vector<ScrollState> scroll_states;
  std::vector<ScrollbarVisualState> scrollbar_visual_states;
  float last_mouse_x = 0.0f;
  float last_mouse_y = 0.0f;
  bool has_mouse_position = false;
  std::uint64_t frame_count = 0;
  std::uint64_t parse_count = 0;
  std::uint64_t layout_count = 0;
  std::uint64_t full_invalidation_count = 0;
  std::uint64_t layout_invalidation_count = 0;
};

std::unordered_map<std::string, CanvasState>& canvas_states() {
  static std::unordered_map<std::string, CanvasState> states;
  return states;
}
}  // namespace

std::shared_ptr<litehtml::document> ParseDocument(const char* id, const char* html, float width) {
  auto& states = canvas_states();
  auto it = states.find(id);
  bool reparsed = false;
  if (it == states.end()) {
    auto container = std::make_shared<BrowserContainer>(width);
    container->set_config(getCurrentConfigPtr());
    container->reset();
    it = states
             .emplace(id, CanvasState{
                              .container = container,
                              .doc = litehtml::document::createFromString(ExpandCustomElements(html), container.get()),
                              .html = html,
                             .last_active_time = std::chrono::high_resolution_clock::now().time_since_epoch().count(),
                              .layout_dirty = true,
                          })
             .first;
    reparsed = true;
  } else if (it->second.html != html) {
    it->second.doc =
        litehtml::document::createFromString(ExpandCustomElements(html), it->second.container.get());
    it->second.html = html;
    it->second.layout_width = -1;
    it->second.layout_height = -1;
    it->second.layout_dirty = true;
    it->second.active_scroll_target.reset();
    it->second.scrollbar_visual_states.clear();
    reparsed = true;
  }
  if (reparsed) ++it->second.parse_count;
  it->second.last_active_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return it->second.doc;
}

void ResetDocument(const char* id) {
  if (id == nullptr) return;
  canvas_states().erase(id);
}

bool RenderDocument(const char* id, float width, std::string* clickedURL,
                    std::vector<ScrollState>* scroll_states_out) {
  auto& states = canvas_states();
  auto it = states.find(id);
  if (it == states.end()) {
    return false;
  }
  CanvasState& state = it->second;
  ++state.frame_count;
  state.last_active_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

  state.container->set_config(getCurrentConfigPtr());
  state.container->reset();

  const ImVec2 document_origin = ImGui::GetCursorScreenPos();
  const ImVec2 document_available = ImGui::GetContentRegionAvail();

  int render_width = width > 0 ? (int)width : (int)ImGui::GetContentRegionAvail().x;
  int render_height = (int)document_available.y;
  static const bool force_relayout = std::getenv("IMHTML_FORCE_RELAYOUT") != nullptr;
  const bool did_relayout = force_relayout || state.layout_dirty || state.doc->layout_dirty() ||
                            state.doc->render_tree_dirty() || state.layout_width != render_width ||
                            state.layout_height != render_height;
  if (did_relayout) {
    state.doc->render(render_width);
    CollectScrollStates(state.doc, state.scroll_states);
    ++state.layout_count;
    state.layout_width = render_width;
    state.layout_height = render_height;
    state.layout_dirty = false;
  }
  litehtml::position clip(
      0, 0, render_width, std::max((int)state.doc->height(), (int)ImGui::GetContentRegionAvail().y));

  const ImVec2 mouse_pos = ImGui::GetMousePos();
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const ImVec2 mouse(mouse_pos.x + window_pos.x, mouse_pos.y + window_pos.y);
  auto x = mouse.x - document_origin.x;
  auto y = mouse.y - document_origin.y;

  std::vector<ScrollState>& scroll_states = state.scroll_states;
  if (!did_relayout) {
    RefreshScrollStateOffsets(scroll_states);
  }

  const auto point_inside = [](const litehtml::position& box, float px, float py) {
    return px >= static_cast<float>(box.x) && py >= static_cast<float>(box.y) &&
           px < static_cast<float>(box.x + box.width) && py < static_cast<float>(box.y + box.height);
  };

  const auto rect_contains = [](const ImVec2& point, const ImVec2& min, const ImVec2& max) {
    return point.x >= min.x && point.y >= min.y && point.x <= max.x && point.y <= max.y;
  };
  const auto point_in_scroll_clip = [&](const ScrollState& scroll_state) {
    return !scroll_state.has_clip || scroll_state.clip_box.is_point_inside(litehtml::pixel_t(x), litehtml::pixel_t(y));
  };

  const ImGuiIO& io = ImGui::GetIO();
  const bool wheel_scrolled = ImGui::IsWindowHovered() && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f);
  if (wheel_scrolled) {
    constexpr float wheel_step = 40.0f;
    const litehtml::pixel_t horizontal_delta(-io.MouseWheelH * wheel_step);
    const litehtml::pixel_t vertical_delta(-io.MouseWheel * wheel_step);
    bool horizontal_consumed = false;
    bool vertical_consumed = false;

    for (auto iter = scroll_states.rbegin(); iter != scroll_states.rend(); ++iter) {
      const auto& scroll_state = *iter;
      if (!scroll_state.render_target || !point_inside(scroll_state.scroll_box, x, y)) {
        continue;
      }
      if (!horizontal_consumed && horizontal_delta != litehtml::pixel_t(0) && scroll_state.max_left > litehtml::pixel_t(0)) {
        horizontal_consumed = scroll_state.render_target->h_scroll(horizontal_delta) != litehtml::pixel_t(0);
      }
      if (!vertical_consumed && vertical_delta != litehtml::pixel_t(0) && scroll_state.max_top > litehtml::pixel_t(0)) {
        vertical_consumed = scroll_state.render_target->v_scroll(vertical_delta) != litehtml::pixel_t(0);
      }
      if (horizontal_consumed && vertical_consumed) {
        break;
      }
    }

  }

  bool scrollbar_input = false;
  const auto apply_scrollbar_position = [&](const ScrollState& scroll_state,
                                             const ScrollbarGeometry& geometry, bool vertical,
                                             float grab_offset) {
    if (!scroll_state.render_target) {
      return;
    }
    const float mouse_coordinate = vertical ? mouse.y : mouse.x;
    const float track_start = vertical ? geometry.track_min.y : geometry.track_min.x;
    const float track_end = vertical ? geometry.track_max.y : geometry.track_max.x;
    const float thumb_size = vertical ? geometry.thumb_max.y - geometry.thumb_min.y
                                      : geometry.thumb_max.x - geometry.thumb_min.x;
    const float travel = std::max(0.0f, track_end - track_start - thumb_size);
    if (travel <= 0.0f) {
      return;
    }
    const float position = ImClamp(mouse_coordinate - grab_offset - track_start, 0.0f, travel);
    const float ratio = position / travel;
    const float maximum = static_cast<float>(vertical ? scroll_state.max_top : scroll_state.max_left);
    const float current = static_cast<float>(vertical ? scroll_state.top : scroll_state.left);
    const litehtml::pixel_t delta(ratio * maximum - current);
    if (vertical) {
      scroll_state.render_target->v_scroll(delta);
    } else {
      scroll_state.render_target->h_scroll(delta);
    }
  };

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    for (auto iter = scroll_states.rbegin(); iter != scroll_states.rend() && !scrollbar_input; ++iter) {
      const auto& scroll_state = *iter;
      ScrollbarGeometry geometry;
      if (GetScrollbarGeometry(scroll_state, document_origin, true, geometry) &&
          point_in_scroll_clip(scroll_state) &&
          rect_contains(mouse, geometry.track_min, geometry.track_max)) {
        state.active_scroll_target = scroll_state.render_target;
        state.active_scroll_vertical = true;
        state.scroll_grab_offset = rect_contains(mouse, geometry.thumb_min, geometry.thumb_max)
                                       ? mouse.y - geometry.thumb_min.y
                                       : (geometry.thumb_max.y - geometry.thumb_min.y) * 0.5f;
        if (!rect_contains(mouse, geometry.thumb_min, geometry.thumb_max)) {
          apply_scrollbar_position(scroll_state, geometry, true, state.scroll_grab_offset);
        }
        scrollbar_input = true;
        break;
      }

      if (GetScrollbarGeometry(scroll_state, document_origin, false, geometry) &&
          point_in_scroll_clip(scroll_state) &&
          rect_contains(mouse, geometry.track_min, geometry.track_max)) {
        state.active_scroll_target = scroll_state.render_target;
        state.active_scroll_vertical = false;
        state.scroll_grab_offset = rect_contains(mouse, geometry.thumb_min, geometry.thumb_max)
                                       ? mouse.x - geometry.thumb_min.x
                                       : (geometry.thumb_max.x - geometry.thumb_min.x) * 0.5f;
        if (!rect_contains(mouse, geometry.thumb_min, geometry.thumb_max)) {
          apply_scrollbar_position(scroll_state, geometry, false, state.scroll_grab_offset);
        }
        scrollbar_input = true;
        break;
      }
    }
  }

  if (state.active_scroll_target && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    bool active_target_found = false;
    for (const auto& scroll_state : scroll_states) {
      if (scroll_state.render_target != state.active_scroll_target) {
        continue;
      }
      ScrollbarGeometry geometry;
      if (GetScrollbarGeometry(scroll_state, document_origin, state.active_scroll_vertical, geometry)) {
        apply_scrollbar_position(scroll_state, geometry, state.active_scroll_vertical, state.scroll_grab_offset);
        active_target_found = true;
      }
      break;
    }
    scrollbar_input = true;
    if (!active_target_found) {
      state.active_scroll_target.reset();
    }
  }

  if (state.active_scroll_target && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    scrollbar_input = true;
    state.active_scroll_target.reset();
  }

  if (wheel_scrolled || scrollbar_input) {
    RefreshScrollStateOffsets(scroll_states);
  }

  // Apply wheel/scrollbar changes before painting the document. Native HTML
  // controls are painted by ImHTML immediately after this function returns;
  // drawing litehtml first would leave the HTML content one frame behind the
  // input overlay whenever the scroll offset changed.
  state.container->apply_requested_cursor();
  state.doc->draw(0, 0, 0, &clip);

  auto& visual_states = state.scrollbar_visual_states;
  visual_states.resize(scroll_states.size());
  const double scrollbar_now = ImGui::GetTime();
  const float delta_time = ImClamp(io.DeltaTime, 0.0f, 0.1f);
  const bool window_hovered = ImGui::IsWindowHovered();
  constexpr double idle_hold_seconds = 0.35;
  constexpr float fade_in_seconds = 0.08f;
  constexpr float fade_out_seconds = 0.20f;
  for (std::size_t index = 0; index < scroll_states.size(); ++index) {
    const ScrollState& scroll_state = scroll_states[index];
    ScrollbarVisualState& visual = visual_states[index];
    const litehtml::render_item* target = scroll_state.render_target.get();
    const litehtml::element* dom_target = scroll_state.target.get();
    if (visual.dom_target != dom_target) {
      visual = {.dom_target = dom_target, .target = target};
    } else {
      visual.target = target;
    }

    const bool hovered = window_hovered && point_inside(scroll_state.scroll_box, x, y) &&
                         point_in_scroll_clip(scroll_state);
    const bool active = target != nullptr && state.active_scroll_target.get() == target;
    if (hovered || active) {
      visual.visible_until = scrollbar_now + idle_hold_seconds;
    }
    const bool should_show = hovered || active || scrollbar_now < visual.visible_until;
    const float step = delta_time / (should_show ? fade_in_seconds : fade_out_seconds);
    visual.opacity = ImClamp(visual.opacity + (should_show ? step : -step), 0.0f, 1.0f);
  }

  if (ImDrawList* draw_list = ImGui::GetWindowDrawList(); draw_list != nullptr) {
    const float viewport_height = std::max(0.0f, document_available.y);
    PushSafeClipRect(draw_list, document_origin,
                     document_origin + ImVec2(static_cast<float>(render_width), viewport_height));
    state.container->paint_scrollbars(scroll_states, visual_states, document_origin);
    draw_list->PopClipRect();
  }

  const auto redraw_box = [](const litehtml::position&) {};
  const bool lbutton_down = !scrollbar_input && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool lbutton_up = !scrollbar_input && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  if (lbutton_down) {
    state.doc->on_lbutton_down(x, y, x, y, redraw_box);
  }
  if (lbutton_up) {
    state.doc->on_lbutton_up(x, y, x, y, redraw_box);
  }
  const bool mouse_moved =
      !state.has_mouse_position || x != state.last_mouse_x || y != state.last_mouse_y;
  if (mouse_moved || did_relayout || scrollbar_input || wheel_scrolled || lbutton_down || lbutton_up) {
    state.doc->on_mouse_over(x, y, x, y, redraw_box);
    state.last_mouse_x = x;
    state.last_mouse_y = y;
    state.has_mouse_position = true;
  }
  ImGui::SetCursorScreenPos(document_origin);
  const ImRect bb(document_origin, document_origin + state.container->get_bottom_right());
  ImGui::ItemSize(bb.GetSize());
  ImGui::ItemAdd(bb, ImGui::GetID(id));

  if (!state.container->get_tooltip().empty()) {
    ImGui::SetTooltip("%s", state.container->get_tooltip().c_str());
  }

  bool clicked = false;
  if (std::string url = state.container->pop_load_url(); !url.empty()) {
    if (clickedURL) {
      *clickedURL = url;
    }
    clicked = true;
  }

  static const bool telemetry_enabled = std::getenv("IMHTML_TELEMETRY") != nullptr;
  if (telemetry_enabled && state.frame_count % 120 == 0) {
    std::fprintf(stderr,
                 "[ImHTML telemetry] canvas=%s frames=%llu parses=%llu layouts=%llu "
                 "full-invalidations=%llu layout-invalidations=%llu\n",
                 id,
                 static_cast<unsigned long long>(state.frame_count),
                 static_cast<unsigned long long>(state.parse_count),
                 static_cast<unsigned long long>(state.layout_count),
                 static_cast<unsigned long long>(state.full_invalidation_count),
                 static_cast<unsigned long long>(state.layout_invalidation_count));
  }

  if (scroll_states_out != nullptr) {
    *scroll_states_out = scroll_states;
  }

  // Cleanup all inactive states with lastActiveTime > 1 seconds
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  for (auto cleanup_it = states.begin(); cleanup_it != states.end();) {
    if (cleanup_it->first != id && now - cleanup_it->second.last_active_time > 1000000000) {
      IMHTML_PRINTF("[ImHTML] Erased state for id=%s\n", cleanup_it->first.c_str());

      // We have to destruct in this order, otherwise we get a segfault
      cleanup_it->second.doc.reset();
      cleanup_it->second.container.reset();

      cleanup_it = states.erase(cleanup_it);
    } else {
      ++cleanup_it;
    }
  }

  return clicked;
}

bool Canvas(const char* id, const char* html, float width, std::string* clickedURL) {
  ParseDocument(id, html, width);
  return RenderDocument(id, width, clickedURL);
}
void MarkDocumentDirty(const char* id) {
  if (id == nullptr) return;
  auto& states = canvas_states();
  const auto found = states.find(id);
  if (found != states.end()) {
    ++found->second.full_invalidation_count;
    found->second.layout_dirty = true;
    if (found->second.doc) found->second.doc->mark_layout_dirty();
  }
}
void MarkDocumentLayoutDirty(const char* id) {
  if (id == nullptr) return;
  auto& states = canvas_states();
  const auto found = states.find(id);
  if (found != states.end()) {
    ++found->second.layout_invalidation_count;
    found->second.layout_dirty = true;
    if (found->second.doc) found->second.doc->mark_layout_dirty();
  }
}
};  // namespace ImHTML
