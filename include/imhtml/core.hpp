#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"

#ifdef IMHTML_DEBUG_PRINTF
#define IMHTML_PRINTF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define IMHTML_PRINTF(fmt, ...)
#endif


namespace ImHTML {
/**
 * Per-document paint and layout counters, as exposed by GetDocumentRenderStats.
 */
struct DocumentRenderStats {
  std::uint64_t frames = 0;
  std::uint64_t parses = 0;
  std::uint64_t layouts = 0;
  std::uint64_t paints = 0;
  std::uint64_t text_runs = 0;
  double last_layout_ms = 0.0;
  double last_paint_ms = 0.0;
  bool last_frame_relayout = false;
};

/**
 * An axis-aligned rectangle in screen space. Deliberately layout-compatible
 * with ImGui's internal ImRect so a draw callback can be moved between the two
 * without edits, while keeping imgui_internal.h out of the public headers.
 */
struct Rect {
  ImVec2 Min{0.0f, 0.0f};
  ImVec2 Max{0.0f, 0.0f};

  Rect() = default;
  Rect(const ImVec2 &min, const ImVec2 &max) : Min(min), Max(max) {}

  ImVec2 GetSize() const { return ImVec2(Max.x - Min.x, Max.y - Min.y); }
  ImVec2 GetCenter() const { return ImVec2((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f); }
  float GetWidth() const { return Max.x - Min.x; }
  float GetHeight() const { return Max.y - Min.y; }
  bool Contains(const ImVec2 &point) const {
    return point.x >= Min.x && point.y >= Min.y && point.x < Max.x && point.y < Max.y;
  }
};

/**
 * Severity of a diagnostic message reported through Config::Log.
 */
enum class LogLevel : unsigned char { Trace, Info, Warning, Error };

/**
 * Font styles (only used internally)
 */
enum class FontStyle : unsigned char { Regular, Bold, Italic, BoldItalic };

/**
 * Meta data for an image
 */
struct ImageMeta {
  int Width;
  int Height;
};

/**
 * A font family, containing different styles of the same font.
 */
struct FontFamily {
  ImFont *Regular = nullptr;
  ImFont *Bold = nullptr;
  ImFont *Italic = nullptr;
  ImFont *BoldItalic = nullptr;
};

/**
 * Configuration for the HTML renderer
 */
struct Config {
  float BaseFontSize = 16.0f;

  // fallback when not found in FontFamilies, or no specific family provided
  FontFamily DefaultFont;

  // CSS font-family name -> family
  std::map<std::string, FontFamily> FontFamilies;

  std::function<void(const char *src, const char *baseurl)> LoadImage;
  std::function<ImageMeta(const char *src, const char *baseurl)> GetImageMeta;
  std::function<ImTextureID(const char *src, const char *baseurl, int display_width, int display_height)>
      GetImageTexture;
  std::function<std::string(const char *url, const char *baseurl)> LoadCSS;

  // Receives every diagnostic the framework emits. Unset means the built-in
  // sink, which stays silent unless the IMHTML_TELEMETRY / IMHTML_TRACE_*
  // environment variables are set.
  std::function<void(LogLevel level, const char *message)> Log;
};

typedef std::function<void(Rect bounds, std::map<std::string, std::string> attributes)> CustomElementDrawFunction;
struct HtmlElementContext {
  std::string_view parent_tag;
  const std::map<std::string, std::string>* parent_attributes = nullptr;
};
typedef std::function<std::string(const std::map<std::string, std::string>&, std::string_view,
                                  const HtmlElementContext&)> CustomElementHtmlFunction;

/**
 * Default file loader for loading CSS files
 *
 * @param url Expects a relative local path to the CSS file
 * @param baseurl The base URL of the CSS file (not used)
 * @return The content of the CSS file
 */
std::string DefaultFileLoader(const char *url, const char *baseurl);

/**
 * Get the current configuration
 *
 * @return The current configuration
 */
Config *GetConfig();

/**
 * Set the configuration
 *
 * @param config The new configuration
 */
void SetConfig(const Config &config);

/**
 * Push the configuration
 *
 * @param config The new configuration
 */
void PushConfig(const Config &config);

/**
 * Pop the configuration
 */
void PopConfig();

/**
 * Register a custom element. The draw function will be called with the position and attributes of the element.
 *
 * @param tagName The tag name of the custom element (e.g. <custom arg="value"></custom>)
 * @param draw The draw function
 */
void RegisterCustomElement(const char *tagName, CustomElementDrawFunction draw);
void RegisterCustomElementHtml(const char *tagName, CustomElementHtmlFunction render_html);

/**
 * Unregister a custom element.
 *
 * @param tagName The tag name of the custom element (e.g. <custom arg="value"></custom>)
 */
void UnregisterCustomElement(const char *tagName);
void UnregisterCustomElementHtml(const char *tagName);

std::string ExpandCustomElements(const std::string& html);

std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes);
std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes,
                               std::string_view children);
void PrepareHtmlTemplate(std::string_view html_template);

/**
 * Render the HTML
 *
 * @param id The ID of the canvas
 * @param html The HTML to render
 * @param width The width of the canvas (0.0f for using available space)
 * @param clickedURL The URL that was clicked (if any)
 * @return True if any link was clicked, false otherwise
 */
bool Canvas(const char *id, const char *html, float width = 0.0f, std::string *clickedURL = nullptr);

void ResetDocument(const char *id);

void MarkDocumentDirty(const char *id);

// While set, the document receives no button presses or releases. A native
// overlay drawn above it uses this so a click it consumes does not also reach
// the elements underneath.
void SetDocumentPointerBlocked(const char *id, bool blocked);

void MarkDocumentLayoutDirty(const char *id);
DocumentRenderStats GetDocumentRenderStats(const char *id);
};  // namespace ImHTML
