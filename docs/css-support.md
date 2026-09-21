# CSS and HTML support

An honest account of what works. Layout and cascade come from litehtml;
painting and controls come from ImHTML. Where a limitation belongs to litehtml
it is marked as such, because it moves when litehtml moves.

Everything below is exercised by a real application; the notes on gaps are
lessons paid for in debugging, not guesses.

## Layout

| Feature | State |
| --- | --- |
| Block, inline, inline-block | Works |
| Flexbox — direction, wrap, grow/shrink/basis, align/justify | Works |
| `gap` in flex containers | **Ignored** by this litehtml build, in both axes. Use margins on the children. |
| CSS Grid | Not supported |
| Positioning: static, relative, absolute, fixed | Works |
| Floats, tables, lists | Works (litehtml's core strength) |
| `overflow` (scroll containers, clipping) | Works via the **shorthand only**. `overflow-x` / `overflow-y` parse as unknown and are dropped, so an element declaring only those silently clips instead of scrolling. |
| `box-sizing` | Works |
| `min/max-width`, `min/max-height` | Works |
| Units: px, %, em, rem, vw, vh | Works. Prefer whole `rem` values; fractional sizes accumulate rounding across nested boxes. |

## Painting

| Feature | State |
| --- | --- |
| `background-color`, `background-image` | Works |
| `linear-gradient`, `radial-gradient`, `conic-gradient` | Works |
| `background-repeat`, `-position`, `-size`, `-clip` | Works |
| `border`, per-side widths, styles and colours | Works, including mitred non-uniform borders |
| `border-radius`, including full pills and circles | Works, snapped to the device pixel grid |
| `box-shadow`, `text-shadow` | Not supported |
| `opacity` | Not supported as a compositing property |
| `transform`, transitions, animations, filters | Not supported |
| Images: raster through the host's loader, SVG through `ImHTML::App` | Works |
| `scrollbar-width`, `scrollbar-color` | Works, painted by ImHTML |

## Text

| Feature | State |
| --- | --- |
| `font-family` (mapped to host fonts), `font-size`, `font-weight`, `font-style` | Works |
| `color`, `text-align`, `text-decoration`, `line-height`, `letter-spacing` | Works |
| `text-overflow: ellipsis`, `white-space`, `overflow-wrap` | Works |
| Text selection, caret, clipboard | Works in native text inputs |
| Bidirectional text, complex shaping (Arabic, Devanagari) | Not supported — ImGui's font atlas draws runs of glyphs, without shaping |

## Selectors and the cascade

| Feature | State |
| --- | --- |
| Type, class, id, descendant, child, sibling combinators | Works |
| Attribute selectors, `:hover`, `:active`, `:focus`, `:first-child`, `:last-child`, `:nth-child` | Works |
| `::before`, `::after` | Works, including as painted boxes |
| Custom properties (`var()`) | Parses, but **resolution order is unreliable**: a declaration using `var()` is re-resolved late and can beat a more specific rule that does not. Keep literal values in rules that must be overridable. |
| Selector lists | An invalid selector anywhere in a comma-separated list invalidates the **whole list** in litehtml. Split risky selectors into separate rules. |
| `@media` queries | Works, against the viewport ImHTML reports |

## Forms and controls

ImHTML paints and drives these natively, rather than relying on litehtml:

| Element | State |
| --- | --- |
| `input[type=text]` | Works: focus, caret, selection, clipboard, IME activation |
| `input[type=checkbox]`, `[role=switch]` | Works |
| `input[type=range]` | Works |
| `select` | Works; ImHTML paints the popup surface because litehtml does not lay out a UA popup |
| `button`, `a` | Work. Note that a flex container on `<button>` lays out wrong in this litehtml build — keep buttons `display: block`. |
| `textarea`, file inputs, radio groups | Not implemented |

## ImHTML additions

Attributes ImHTML gives meaning beyond standard HTML:

| Attribute | Purpose |
| --- | --- |
| `data-switcher` / `href="#panel-id"` | Fragment and tab switching without a reparse |
| `data-value`, `data-checked` | Control state readable through `value()` / `checked()` |
| Custom element tags | Registered from C++, either expanding to HTML or painting ImGui |

## Reporting a gap

If something in the first three tables does not work as described, that is a
bug — please open an issue with the smallest stylesheet that reproduces it. If
it is in a "not supported" row, it is a feature request, and the honest answer
may be that it belongs in litehtml rather than here.
