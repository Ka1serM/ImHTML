# ImHTML

Build desktop UI with HTML and CSS, painted through a Dear ImGui draw list.

[litehtml](https://github.com/litehtml/litehtml) owns parsing, the CSS cascade,
layout and hit testing. ImHTML owns everything from there down: painting,
input, native controls, the component system, and a document runtime that keeps
a page live across frames. Your application logic stays in C++.

ImHTML creates no window, owns no render loop, and links no windowing library.
It draws into the ImGui frame you already have.

> ImHTML began as a fork of [BigJk/ImHTML](https://github.com/BigJk/ImHTML) and
> keeps its MIT licence. This fork turns the original renderer into a full UI
> framework: a persistent document, native controls, fragments and components,
> list virtualization, and a paint backend with device-pixel snapping.

## Targets

| Target | Links | Use when |
| --- | --- | --- |
| `ImHTML::Core` | ImGui, litehtml, nlohmann/json | You have an ImGui frame loop and want HTML UI in it. |
| `ImHTML::App` | Core + lunasvg | You want the batteries-included shell: font setup, stylesheet aggregation, SVG image cache, fragments, theme. |

Neither target calls a platform API. The two services ImHTML cannot provide
itself — opening a URL and toggling the IME — are installed by the host:

```cpp
ImHTML::PlatformHooks hooks;
hooks.open_url = [](const std::string& url) { return SDL_OpenURL(url.c_str()); };
hooks.set_text_input_active = [](bool active) { /* your windowing system */ };
ImHTML::SetPlatformHooks(std::move(hooks));
```

Leave them unset and those two features are no-ops; everything else works.

## Integration

ImHTML does not vendor its dependencies — the embedding project owns them and
their versions. Provide the targets, then add the library:

```cmake
# imgui, litehtml and pantor::inja must already exist as targets.
add_subdirectory(external/ImHTML)
target_link_libraries(your_app PRIVATE ImHTML::App)   # or ImHTML::Core
```

Options: `IMHTML_BUILD_APP` (default ON), `IMHTML_BUILD_TESTS` (default OFF),
`IMHTML_INSTALL` (default ON), and `IMHTML_LUNASVG_TARGET` if your lunasvg
target is named differently.

An installed tree is consumable with `find_package`:

```cmake
find_package(ImHTML 0.1 REQUIRED)
target_link_libraries(your_app PRIVATE ImHTML::Core)
```

`ci/CMakeLists.txt` is a complete, working example of assembling the dependency
graph from scratch.

Requires C++23. Headers are included by prefix: `<imhtml/core.hpp>`,
`<imhtml/document.hpp>`, `<imhtml/app.hpp>`.

## A minimal document

```cpp
#include <imhtml/document.hpp>

ImHTML::HtmlDocument document;
document.register_shell("<html><body><h1 id='title'>Hello</h1></body></html>");
document.set_stylesheet_provider([] { return "h1 { color: #4ade80; }"; });
document.on("click", "#title", [](ImHTML::Event& event) {
    std::printf("clicked %s\n", event.target().c_str());
});
document.initialize(nullptr);

// Once per ImGui frame, between NewFrame and Render:
document.frame();
```

The `ImHTML::App` shell adds fonts, stylesheet collection and page fragments on
top of the same document. See [docs/integration.md](docs/integration.md).

## What ImHTML gives you

- **Live documents.** Attributes, values and text update in place; a fragment
  switch replaces only the affected subtree, and layout runs once per frame.
- **Native controls.** `input[type=text]`, `input[type=checkbox]`,
  `input[type=range]`, `select`, focus and keyboard handling, text selection.
- **DOM templates.** Clone a standard HTML template's content, fill its fields,
  and insert nodes or document fragments. Insertion preserves node identity.
- **Components.** Register a C++ custom element that either expands to HTML or
  paints ImGui directly into its layout rectangle.
- **A paint backend that gets the details right.** Fully rounded corners,
  gradients (linear, radial, conic), device-pixel snapping so nested boxes stay
  centred on fractional display scales, and clip rects that never eat the
  antialiased edge of the shape they contain.

## Documentation

- [Integration guide](docs/integration.md) — from a bare ImGui loop to a page.
- [Architecture](docs/architecture.md) — how the pieces fit, and where to make
  a given change.
- [CSS support](docs/css-support.md) — what actually works, honestly.
- [CHANGELOG](CHANGELOG.md)

## Testing

```bash
cmake -S ci -B build -DIMHTML_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Five suites: layout, scroll, styles, fragment switching, and paint. The paint
suite rasterizes the draw list in software and asserts on coverage — symmetry,
centring and painted area — because rendering defects produce correct layout
boxes and wrong pixels.

## Versioning

Semantic versioning applies to the headers under `include/imhtml/` only.
Anything in `src/` is implementation detail. `IMHTML_VERSION_STRING` and
`ImHTML::RuntimeVersion()` report the header and binary versions respectively.

ImHTML uses a small amount of ImGui's internal API (`ImDrawList::_Path`,
`_ClipRectStack`, `_VtxCurrentIdx`) to get antialiasing and clipping right.
Supported versions are pinned in `ci/fetch_dependencies.cmake` and tested there.

## Licence

MIT — see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
