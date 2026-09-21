// Scroll containers.
//
// litehtml owns overflow and the render tree; this walks that tree once per
// frame and records every scrollable box, its viewport, its content size and
// its scrollbar lanes. Painting and hit testing read the result rather than
// walking the tree again.

#include <cmath>
#include <memory>
#include <vector>

#include "imgui.h"

#include <imhtml/core.hpp>

#include "internal/core_internal.hpp"
#include "litehtml.h"
#include "litehtml/render_item.h"
#include "litehtml/types.h"

namespace ImHTML {

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
  if (!item || !item->src_el() || item->hidden()) return;

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

}  // namespace ImHTML
