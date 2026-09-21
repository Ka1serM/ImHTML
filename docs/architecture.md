# Architecture

## The split with litehtml

litehtml is the HTML/CSS engine: it parses markup, matches selectors, computes
styles, runs layout, builds the render tree, and answers hit tests. ImHTML is
everything litehtml deliberately leaves to its host — painting, input, fonts,
images, controls — plus a runtime that keeps a document alive across frames.

The boundary is `litehtml::document_container`. ImHTML implements it once, in
`src/core.cpp`, as `BrowserContainer`.

## Layers

```
your application
      │
      ├─ ImHTML::App        src/app/         optional shell: fonts, stylesheets,
      │                                      SVG image cache, fragments, theme
      │
      ├─ HtmlDocument       src/document.cpp document lifecycle, events, native
      │                                      controls, switchers, lists
      │
      ├─ BrowserContainer   src/core.cpp     paint, text, input, clipping
      │  ├─ scroll          src/scroll/      scroll containers and scrollbars
      │  └─ templates       src/template/    custom elements and inja
      │
      └─ litehtml                            parse, cascade, layout, hit test
```

`ImHTML::Context` (`include/imhtml/context.hpp`) owns configuration and the
custom-element registries. Documents bind to the context that was current when
they were constructed and re-enter it for every frame, so two documents in one
process do not share registrations. The current context is thread-local; one
context is single-threaded.

## Where to make a change

| You want to change | Look in |
| --- | --- |
| How a background, border or gradient is painted | `src/core.cpp`, the `draw_*` overrides |
| Antialiasing, clipping, device-pixel snapping | `src/core.cpp`, `SnapToDevicePixels` / `GrowToDevicePixels` / `PushSafeClipRect` |
| Scroll behaviour or scrollbar geometry | `src/scroll/scroll_state.cpp` |
| A native control (text, checkbox, range, select) | `src/document.cpp` |
| Fragment switching, list virtualization, events | `src/document.cpp` |
| Custom elements, `{{ }}` expansion | `src/template/templates.cpp` |
| Fonts, images, stylesheet collection | `src/app/` |

## Frame flow

1. The host calls `HtmlDocument::frame()` inside its ImGui frame.
2. Pending structural changes are applied: fragment swaps, list windows,
   attribute updates. A document reparse happens only if the structure demands
   it; attribute-only updates refresh styles in place.
3. litehtml lays out once, if anything invalidated layout.
4. `BrowserContainer` paints into the ImGui draw list. Backgrounds and borders
   snap to the device pixel grid; clip rects round outward so they never cut the
   antialiased edge of the shape inside them.
5. Native controls draw on top and collect input, then events dispatch to
   handlers registered with `on()`.

## Painting rules worth knowing

- **Device-pixel snapping.** Layout works on the logical grid, but the
  framebuffer may be denser (a 1.25 or 1.5 display scale). Fills and borders
  round to the device grid so that two boxes inset from each other by the same
  amount stay inset by the same amount after rasterization. Without it a centred
  knob visibly sits high or low.
- **Clips round outward.** A scissor is pushed only when it is actually smaller
  than the box being painted, and its edges round away from the shape. A clip
  that lands mid-pixel costs the shape its outermost row of coverage.
- **Full radii are drawn as arcs.** `ImDrawList::AddRectFilled` caps rounding at
  half the shorter side minus a pixel, which leaves a flat segment on the ends of
  a pill. Radii at or near that cap go through an explicit arc path instead.

Each of these has a regression test in `tests/test_paint.cpp`.
