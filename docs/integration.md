# Integration guide

Two ways in. Start with the core if you already have an ImGui application and
want HTML inside it; start with the shell if you want the batteries included.

## 1. Core, inside an existing ImGui loop

`ImHTML::Core` needs an ImGui context with a font atlas and a frame in progress.
It never creates a window and never calls a platform API.

```cpp
#include <imhtml/document.hpp>

ImHTML::HtmlDocument document;

void setup() {
    // Fonts and image loading are host-supplied.
    ImHTML::Config config = *ImHTML::GetConfig();
    config.BaseFontSize = 16.0f;
    config.DefaultFont = {font, bold, italic, bold_italic};
    config.FontFamilies["sans-serif"] = config.DefaultFont;
    config.Log = [](ImHTML::LogLevel level, const char* message) {
        my_logger().write(level, message);
    };
    ImHTML::SetConfig(config);

    document.register_shell(R"(
        <html><body>
            <div class="card">
                <h1 id="title">Hello</h1>
                <input id="name" type="text" value="" />
                <button id="save">Save</button>
            </div>
        </body></html>)");
    document.set_stylesheet_provider([] { return load_my_css(); });

    document.on("click", "#save", [](ImHTML::Event& event) {
        save(document.value("#name"));
    });

    document.initialize(nullptr);
}

void frame() {
    ImGui::NewFrame();
    document.frame();     // lays out, paints, dispatches events
    ImGui::Render();
}
```

That is the whole contract: construct, register markup and CSS, call `frame()`
once per ImGui frame.

## 2. The App shell

`ImHTML::App` adds font setup, stylesheet aggregation, an SVG image cache,
page fragments and a theme helper.

```cpp
#include <imhtml/app.hpp>

ImHTML::HtmlWindow window;
window.register_stylesheet("body { margin: 0; }");
window.register_fragment<SettingsPage>();     // derives from ImHTML::Fragment
window.register_component<ChartComponent>();  // derives from UiComponent

window.initialize(font_bytes, display_scale);

// once per frame
window.frame();
```

`display_scale` is the framebuffer density (1.0, 1.5, 2.0…). Pass 0 and the
shell reads `io.DisplayFramebufferScale` from the ImGui backend.

## Platform services

ImHTML links no windowing library. Two features need one, and the host supplies
them once at startup:

```cpp
#include <imhtml/platform.hpp>

ImHTML::PlatformHooks hooks;
hooks.open_url = [](const std::string& url) { return SDL_OpenURL(url.c_str()); };
hooks.set_text_input_active = [](bool active) {
    SDL_Window* focused = SDL_GetKeyboardFocus();
    if (!focused) return;
    active ? SDL_StartTextInput(focused) : SDL_StopTextInput(focused);
};
ImHTML::SetPlatformHooks(std::move(hooks));
```

Unset hooks are no-ops: links do not open, and IME composition is not started.

## Updating the page

```cpp
document.set_text("#title", "Updated");                    // text content
document.set_value("#name", "Ada");                        // control value
document.set_attribute("#panel", "data-open", "true");     // paint-only by default
document.set_attribute("#panel", "class", "panel wide",
                       ImHTML::HtmlDocument::ElementUpdateMode::Layout);

// DOM-style structural updates, analogous to element.append() and
// element.replaceChildren() in JavaScript.
document.append("#messages", "<p>A new message</p>");
document.replace_children("#status", "<strong>Ready</strong>");
```

Prefer the default paint-only mode. Reserve `ElementUpdateMode::Layout` for
attributes that change geometry — it forces a relayout.

## DOM templates

Declare inert markup once, then clone and insert it after its container mounts:

```html
<div id="files" class="grid" role="listbox"></div>
<template id="file-template">
    <div class="row" role="option">
        <img alt=""><span data-field="name"></span>
    </div>
</template>
```

```cpp
auto content = document.query_selector("#file-template").content();
auto batch = document.create_document_fragment();
for (const File& file : files) {
    auto item = content.clone_node(true);
    item.query_selector("img").set_attribute("src", file.icon);
    item.query_selector("[data-field=name]").set_text_content(file.name);
    batch.append_child(item);
}
document.query_selector("#files").replace_children(batch);
```

Use `create_element` for programmatically constructed elements. `append_child`
and `append` move existing nodes rather than copy them; inserting a document
fragment consumes its children. `remove()` detaches one node, preserving the
identity of its siblings. `clone_node()` is shallow by default; pass `true`
to copy descendants. Template contents are excluded from document queries.

Rows are ordinary DOM nodes, not a hidden JSON model or virtual-list renderer.
Use attributes such as `data-index` or domain IDs for delegated events, e.g.
`event.closest("[data-index]").attribute("data-index")`. Populate each newly
mounted container; querying before the first document frame returns no element.
For very large datasets, applications should page their results explicitly.

## Custom elements

Two kinds. One expands to HTML:

```cpp
ImHTML::RegisterCustomElementHtml("settings-box",
    [](const std::map<std::string, std::string>& attributes,
       std::string_view children, const ImHTML::HtmlElementContext&) {
        return "<div class=\"row\"><b>" + attributes.at("title") + "</b>" +
               std::string(children) + "</div>";
    });
```

The other paints ImGui directly into its layout rectangle:

```cpp
ImHTML::RegisterCustomElement("viewport",
    [](ImHTML::Rect bounds, std::map<std::string, std::string> attributes) {
        ImGui::SetCursorScreenPos(bounds.Min);
        ImGui::Image(my_texture, bounds.GetSize());
    });
```

Registrations belong to the current `ImHTML::Context`. To keep a library's
elements out of a host's namespace, scope them:

```cpp
ImHTML::Context panel_context;
{
    ImHTML::ContextScope scope(panel_context);
    ImHTML::RegisterCustomElement("chart", draw_chart);
    // documents constructed here bind to panel_context
}
```

## Diagnostics

Set `Config::Log` to route messages into your own logger. With no sink
installed, ImHTML writes to stderr, and the `IMHTML_TELEMETRY`,
`IMHTML_TRACE_INTERACTIONS`, `IMHTML_TRACE_REBUILD` and `IMHTML_TRACE_LAYOUT`
environment variables enable per-frame counters.
