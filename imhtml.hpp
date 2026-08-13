#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <inja/inja.hpp>
#include "imgui.h"
#include "imgui_internal.h"

#ifdef IMHTML_DEBUG_PRINTF
#define IMHTML_PRINTF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define IMHTML_PRINTF(fmt, ...)
#endif

namespace litehtml {
class document;
class element;
}

namespace ImHTML {

struct ElementLayerTransform {
  ImVec2 Scale = ImVec2(1.0f, 1.0f);
  float Opacity = 1.0f;
  float Brightness = 1.0f;
  ImVec2 Origin = ImVec2(0.5f, 0.5f);
};

/**
 * Resolves the compositor transform for an element before its paint subtree
 * is emitted into ImGui's draw list. The element's DOM attributes (including
 * id) are unchanged and remain the source of truth for layout and input.
 *
 * @param element The stable litehtml DOM element being painted
 * @param bounds The element's layout border box in screen coordinates
 * @return The visual-only transform to apply to this paint subtree
 */
using ElementLayerTransformFunction =
    std::function<ElementLayerTransform(const std::shared_ptr<::litehtml::element>& element, const ImRect& bounds)>;
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
  // Links already communicate their destination through their visible label;
  // showing the raw href as a hover tooltip is noisy for embedded app UI.
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

// Computed geometry and style exposed to native interaction islands. The
// values are resolved by litehtml, so native widgets do not need to duplicate
// CSS dimensions or apply an application-wide scale of their own.
struct NativeElementContext {
  ImRect border_box;
  ImRect padding_box;
  ImRect content_box;

  // ImVec4 components are ordered left, top, right, bottom. Border radii are
  // ordered top-left, top-right, bottom-right, bottom-left and represent the
  // horizontal radius in each corner.
  ImVec4 padding = ImVec4(0, 0, 0, 0);
  ImVec4 border_width = ImVec4(0, 0, 0, 0);
  ImVec4 border_radius = ImVec4(0, 0, 0, 0);

  float font_size = 16.0f;
  float line_height = 16.0f;
  ImU32 text_color = IM_COL32(0, 0, 0, 255);
};

/**
 * A custom element draw function. HTML owns the supplied geometry and style;
 * the callback is intended for interaction and embedded ImGui rendering.
 */
typedef std::function<void(const NativeElementContext& context,
                           const std::map<std::string, std::string>& attributes)>
    CustomElementDrawFunction;
using LegacyCustomElementDrawFunction =
    std::function<void(ImRect bounds, const std::map<std::string, std::string>& attributes)>;
typedef std::function<void()> CustomElementEndDrawFunction;
// Context from the source HTML tree. This is available while custom HTML is
// expanded, before litehtml has created its DOM. It deliberately contains
// source-level information rather than a litehtml element pointer.
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
void RegisterCustomElement(const char *tagName, LegacyCustomElementDrawFunction draw);
void RegisterCustomElementEnd(const char *tagName, CustomElementEndDrawFunction draw_end);
void RegisterCustomElementHtml(const char *tagName, CustomElementHtmlFunction render_html);

/**
 * Unregister a custom element.
 *
 * @param tagName The tag name of the custom element (e.g. <custom arg="value"></custom>)
 */
void UnregisterCustomElement(const char *tagName);
void UnregisterCustomElementHtml(const char *tagName);

// Expands registered HTML components before litehtml parses the document.
// Both self-closing elements and one-level paired elements with inner HTML
// are supported, e.g. <img-button .../> and <setting-row ...><button/></setting-row>.
std::string ExpandCustomElements(const std::string& html);
// As above, but also resolves standard <template> elements. A template is
// handed to this callback with the source-level parent element, so callers can
// associate it with a surrounding <ul id="..."> without adding a marker
// attribute to the template itself.
std::string ExpandCustomElements(const std::string& html, CustomElementHtmlFunction template_renderer);

// Resolves an Inja/Jinja-style HTML template in one pass before litehtml
// parses it. The optional `children` value is available to the template as
// trusted inner markup.
std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes);
std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes,
                               std::string_view children);
void PrepareHtmlTemplate(std::string_view html_template);
std::string ExpandHtmlTemplate(std::string_view html_template, const inja::json& data);

/**
 * Register the compositor transform resolver. Transforms are applied to the
 * emitted ImGui draw range for an element and never mutate litehtml layout.
 */
void RegisterElementLayerTransform(ElementLayerTransformFunction transform);

void UnregisterElementLayerTransform();

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

/**
 * Parses (or returns the already-parsed) litehtml document cached under `id`,
 * for callers that want to inspect or mutate the DOM directly — element::
 * select_all/select_one/appendChild/removeChild/set_attr etc. — instead of
 * templating the HTML text before handing it to Canvas(). The document is
 * only re-parsed when `html` differs from what produced the cached one, so
 * in-place edits made after a previous call survive across frames as long as
 * the same source `html` is passed each time. Pair with RenderDocument() to
 * actually lay out and draw whatever the document currently contains.
 *
 * @param id The ID of the canvas (shares its cache with Canvas()/RenderDocument())
 * @param html The source HTML; only reparsed when this changes
 * @param width The width used when a *new* parse establishes the container (0.0f for available space)
 * @return The live document; mutate it before the next RenderDocument() call
 */
std::shared_ptr<litehtml::document> ParseDocument(const char *id, const char *html, float width = 0.0f);

// Drops the cached document so the next ParseDocument() creates a fresh DOM
// with the same source markup. This is useful when a caller intentionally
// mutates parsed slots and wants to rebuild them from their providers.
void ResetDocument(const char *id);

/**
 * Renders the document previously produced by ParseDocument() (or Canvas())
 * for `id`. Returns false if nothing has been parsed for `id` yet.
 *
 * @param id The ID of the canvas
 * @param width The width to render at (0.0f for using available space)
 * @param clickedURL The URL that was clicked (if any)
 * @return True if any link was clicked, false otherwise
 */
bool RenderDocument(const char *id, float width = 0.0f, std::string *clickedURL = nullptr);

// Marks the cached document's layout dirty without reparsing its HTML.
// Use this after mutating DOM attributes or inline styles directly.
void MarkDocumentDirty(const char *id);

// Marks layout dirty after the caller has already invalidated the affected
// litehtml measurement chain (used by targeted live-style updates).
void MarkDocumentLayoutDirty(const char *id);
};  // namespace ImHTML
