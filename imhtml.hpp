#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <inja/inja.hpp>
#include "imgui.h"
#include "imgui_internal.h"
#include "litehtml/css_control.h"
#include "litehtml/types.h"

#ifdef IMHTML_DEBUG_PRINTF
#define IMHTML_PRINTF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define IMHTML_PRINTF(fmt, ...)
#endif

namespace litehtml {
class document;
class element;
class render_item;
}

namespace ImHTML {
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
  bool AllowHrefTooltips = false;
  bool AllowImgAltTooltips = true;

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
};

struct ScrollState {
  std::shared_ptr<litehtml::element> target;
  std::shared_ptr<litehtml::render_item> render_target;
  litehtml::position scroll_box;
  litehtml::position viewport_box;
  litehtml::position vertical_scrollbar_box;
  litehtml::position horizontal_scrollbar_box;
  litehtml::size viewport_size;
  litehtml::size content_size;
  litehtml::pixel_t left = litehtml::pixel_t(0);
  litehtml::pixel_t top = litehtml::pixel_t(0);
  litehtml::pixel_t max_left = litehtml::pixel_t(0);
  litehtml::pixel_t max_top = litehtml::pixel_t(0);
  bool has_clip = false;
  litehtml::position clip_box;
};

void CollectScrollStates(const std::shared_ptr<litehtml::document>& document,
                         std::vector<ScrollState>& states);

float ScrollbarSizePixels(litehtml::scrollbar_width width);

typedef std::function<void(ImRect bounds, std::map<std::string, std::string> attributes)> CustomElementDrawFunction;
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
std::string ExpandCustomElements(const std::string& html, CustomElementHtmlFunction template_renderer);

std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes);
std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes,
                               std::string_view children);
void PrepareHtmlTemplate(std::string_view html_template);
std::string ExpandHtmlTemplate(std::string_view html_template, const inja::json& data);

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

std::shared_ptr<litehtml::document> ParseDocument(const char *id, const char *html, float width = 0.0f);

void ResetDocument(const char *id);

bool RenderDocument(const char *id, float width = 0.0f, std::string *clickedURL = nullptr,
                    std::vector<ScrollState> *scroll_states = nullptr);

void MarkDocumentDirty(const char *id);

void MarkDocumentLayoutDirty(const char *id);
};  // namespace ImHTML
