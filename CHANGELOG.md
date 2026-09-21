# Changelog

All notable changes to ImHTML are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semantic versioning](https://semver.org/) over the public headers in
`include/imhtml/`.

## [0.1.0] — unreleased

First versioned release, and the point at which ImHTML became consumable by
projects other than the one it grew up in.

### Added

- `ImHTML::Context`: configuration and custom-element registries are per-context
  instead of per-process, so two documents no longer share registrations.
  Documents bind to their context and re-enter it each frame. The current
  context is thread-local.
- `ImHTML::Element`: an opaque handle returned by `query_selector`, replacing
  raw `std::shared_ptr<litehtml::element>` in the public API.
- `ImHTML::Rect`, replacing ImGui's internal `ImRect` in component callbacks.
- `Config::Log`: a host log sink. Diagnostics no longer write to stderr behind
  the framework's back.
- `ImHTML::PlatformHooks` as public API: the host supplies URL opening and IME
  activation; ImHTML links no windowing library.
- A paint test suite that rasterizes the draw list in software and asserts on
  coverage symmetry, centring and painted area.
- CMake package config, so an installed tree works with `find_package(ImHTML)`.
- CI: Linux and Windows, Debug and Release, plus an ASan/UBSan job and a
  formatting check.

### Changed

- Split into two targets: `ImHTML::Core` (ImGui, litehtml, nlohmann/json) and
  `ImHTML::App` (adds lunasvg). SDL3 is no longer a dependency of either.
- Headers moved under an `imhtml/` prefix: `<imhtml/core.hpp>`,
  `<imhtml/document.hpp>`, `<imhtml/app.hpp>`, `<imhtml/context.hpp>`,
  `<imhtml/platform.hpp>`, `<imhtml/theme.hpp>`, `<imhtml/components.hpp>`.
- `imgui_internal.h`, `inja/inja.hpp` and litehtml headers no longer appear in
  any public header. inja is an implementation detail; `nlohmann::json` remains
  as `ImHTML::ListItem`, the data type for list templates.
- `HtmlWindow::initialize` takes a font and an optional display scale instead of
  an `SDL_Window*`.
- Test suites build as part of the library (`IMHTML_BUILD_TESTS=ON`) and register
  with CTest, instead of being a separate project pointed at a parent build tree.

### Fixed

- Rounded fills lost their antialiased fringe to a scissor equal to their own
  box, leaving hard, faceted edges.
- Fully rounded pills rendered with a flat segment at each end, because
  `AddRectFilled` caps its rounding.
- Nested boxes drifted off centre on fractional display scales; fills and clips
  now share the device pixel grid.
- A clip rect computed from unsnapped layout cut the bottom and right edges off
  snapped fills.
- Adjacent corner arcs left duplicate path points, which ImGui's antialiasing
  turned into spikes on the outline.
