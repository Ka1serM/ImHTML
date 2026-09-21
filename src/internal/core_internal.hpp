#pragma once

// Declarations shared between ImHTML's own translation units. Everything here
// exposes litehtml or inja types and therefore must not appear in a public
// header — a consumer compiles against <imhtml/core.hpp> alone.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <inja/inja.hpp>

#include "litehtml/css_control.h"
#include "litehtml/types.h"

#include <imhtml/core.hpp>
#include <imhtml/document.hpp>

namespace litehtml {
class document;
class element;
class render_item;
}  // namespace litehtml

namespace ImHTML {

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

// Scroll containers as collected during this frame's RenderDocument call.
// Reusing this avoids walking the whole render tree again per native control.
const std::vector<ScrollState>& FrameScrollStates(const char* id);

float ScrollbarSizePixels(litehtml::scrollbar_width width);

std::shared_ptr<litehtml::document> ParseDocument(const char* id, const char* html, float width = 0.0f);

bool RenderDocument(const char* id, float width = 0.0f, std::string* clickedURL = nullptr);

// Template expansion with structured data. inja is an implementation detail:
// custom components take attribute maps; runtime DOM templates are cloned nodes.
std::string ExpandHtmlTemplate(std::string_view html_template, const inja::json& data);

// The litehtml element behind a public handle. Internal and test-only: it is
// declared as a friend of Element in the public header.
const std::shared_ptr<litehtml::element>& RawElement(const Element& element);

// Routes a diagnostic to Config::Log, or to the built-in environment-variable
// sink when the host installed no logger.
void LogMessage(LogLevel level, const std::string& message);

// printf-style form, so the framework's existing diagnostics keep their format
// strings. Never writes to stderr directly when a host logger is installed.
void LogPrintf(LogLevel level, const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace ImHTML
