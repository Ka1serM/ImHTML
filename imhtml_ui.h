#pragma once

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Public UI layer for the ImHTML fork. Applications register fragments,
// view-model bindings, stylesheets, and concrete ImHTML components. Generic
// HTML/layout plumbing stays inside ImHTML while visual controls remain in
// the registered component classes.

#include "imhtml_app.h"
#include "ui/ui_animation.h"
#include "ui/ui_theme.h"
#include "ui/ui_component_registry.h"

struct SDL_Window;

namespace ImHTML {
using Animation = ::IUiAnimation;
using ImAnimAnimation = ::ImAnimUiAnimation;
using ComponentRegistry = ::UiComponentRegistry;
using Component = ::UiComponent;

// Process-wide monotonic identity source for UI elements whose DOM identity
// must survive list rebuilds. The ID is intentionally separate from a list
// item's current positional index.
inline std::uint64_t next_unique_id() {
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

class UiApplication;
class UiImageCache;

class Stylesheet {
public:
    virtual ~Stylesheet() = default;
    virtual const char* id() const = 0;
    virtual std::string css() const = 0;
};

// A fragment is the universal self-contained markup unit. It can provide a
// reusable slot, a switchable view, or the application shell.
class Fragment {
public:
    virtual ~Fragment() = default;
    // HTML-only fragments do not need an ID. Slot/view fragments override it.
    virtual const char* id() const { return nullptr; }
    virtual std::string html() const = 0;
    // Fragments may optionally also provide a registered HTML-only custom
    // element. The registry expands its template before litehtml parses it.
    virtual const char* tag() const { return nullptr; }
    // A tag identifies an HTML-only fragment. Runtime/native components have
    // their own registry and do not use this class.
    virtual bool expands_html() const { return tag() != nullptr; }
    virtual std::string html(const std::map<std::string, std::string>& attributes) const {
        return ExpandHtmlTemplate(html(), attributes);
    }
    virtual std::string html(const std::map<std::string, std::string>& attributes,
                             std::string_view children) const {
        return ExpandHtmlTemplate(html(), attributes, children);
    }
    virtual std::string html(const std::map<std::string, std::string>& attributes, std::string_view children,
                             const HtmlElementContext&) const {
        return html(attributes, children);
    }
    virtual void register_ui(UiApplication&) {}
    virtual void draw(UiApplication&) {}
    virtual void tick() {}
    virtual void flush() {}
};

class FragmentRegistry {
public:
    template <typename TFragment>
    TFragment& register_fragment(HtmlApplication& html_application) {
        auto fragment = std::make_unique<TFragment>();
        TFragment& result = *fragment;
        Fragment* instance = fragment.get();
        fragments_.push_back(std::move(fragment));
        const std::string markup = instance->html();
        if (markup.find("<html") != std::string::npos) {
            html_application.register_shell(markup);
        } else if (const char* id = instance->id()) {
            html_application.register_fragment(id, [instance] { return instance->html(); });
            fragments_by_id_[id] = instance;
        }
        if (instance->tag() != nullptr && instance->expands_html()) {
            ImHTML::PrepareHtmlTemplate(instance->html());
            ImHTML::RegisterCustomElementHtml(
                instance->tag(), [instance](const std::map<std::string, std::string>& attributes,
                                            std::string_view children, const HtmlElementContext& context) {
                    return instance->html(attributes, children, context);
                });
        }
        return result;
    }

    void register_ui(UiApplication& application) {
        for (const auto& fragment : fragments_) fragment->register_ui(application);
    }

    void tick() {
        for (const auto& fragment : fragments_) fragment->tick();
    }

    void flush() {
        for (const auto& fragment : fragments_) fragment->flush();
    }

    void draw(const std::string& fragment_id, UiApplication& application) {
        const auto found = fragments_by_id_.find(fragment_id);
        if (found != fragments_by_id_.end()) found->second->draw(application);
    }

private:
    std::vector<std::unique_ptr<Fragment>> fragments_;
    std::map<std::string, Fragment*> fragments_by_id_;
};

class StylesheetRegistry {
public:
    template <typename TStylesheet>
    TStylesheet& register_stylesheet() {
        auto stylesheet = std::make_unique<TStylesheet>();
        TStylesheet& result = *stylesheet;
        Stylesheet* instance = stylesheet.get();
        stylesheets_.push_back(std::move(stylesheet));
        providers_.push_back([instance] { return instance->css(); });
        return result;
    }

    const std::vector<std::function<std::string()>>& providers() const { return providers_; }

private:
    std::vector<std::unique_ptr<Stylesheet>> stylesheets_;
    std::vector<std::function<std::string()>> providers_;
};

// Generic runtime owned by the fork. It owns fragment/switcher state, hosts native
// HTML components, and delegates application-specific element interaction
// without requiring the application to know about litehtml callbacks.
class UiApplication {
public:
    using StylesheetProvider = std::function<std::string()>;

    UiApplication();
    ~UiApplication();

    UiApplication(const UiApplication&) = delete;
    UiApplication& operator=(const UiApplication&) = delete;

    // Registers bytes for an embedded image path. The span is borrowed and
    // must remain valid for the lifetime of the application.
    void register_image(std::string path, std::span<const unsigned char> bytes);

    void register_stylesheet(StylesheetProvider provider);
    void register_stylesheet(std::string css) {
        Theme::initialize_from_css(css);
        stylesheets_.push_back([css = std::move(css)] { return css; });
    }
    void register_stylesheet(std::span<const unsigned char> css) {
        register_stylesheet(std::string(reinterpret_cast<const char*>(css.data()), css.size()));
    }
    void register_stylesheet_file(std::string path);
    template <typename TStylesheet>
    TStylesheet& register_stylesheet() { return embedded_stylesheets_.register_stylesheet<TStylesheet>(); }
    float base_font_size() const;

    template <typename TFragment>
    TFragment& register_fragment() { return fragments_.register_fragment<TFragment>(html_application_); }

    template <typename TComponent, typename... TArguments>
    TComponent& register_component(TArguments&&... arguments) {
        return components_.register_component<TComponent>(*this, std::forward<TArguments>(arguments)...);
    }
    template <typename TControl, typename... TArguments>
    TControl& register_html_control(TArguments&&... arguments) {
        auto control = std::make_unique<TControl>(*this, std::forward<TArguments>(arguments)...);
        TControl& result = *control;
        html_application_.register_html_control(std::move(control));
        return result;
    }
    HtmlApplication& html() { return html_application_; }
    const HtmlApplication& html() const { return html_application_; }
    SDL_Window* window() const { return window_; }

    void set_animation(std::unique_ptr<IUiAnimation> animation);
    bool initialize(SDL_Window* window, const std::string& font_path);
    bool initialize(SDL_Window* window, std::span<const unsigned char> font_data);
    void frame();
    void shutdown();
    // Releases ImGui texture registrations while the renderer and ImGui
    // context are still alive. The host calls this before tearing them down.
    void release_image_resources();

private:
    bool finish_initialize();
    std::string stylesheet() const;
    HtmlApplication html_application_;
    FragmentRegistry fragments_;
    StylesheetRegistry embedded_stylesheets_;
    ComponentRegistry components_;
    std::unique_ptr<IUiAnimation> animation_;
    std::unique_ptr<UiImageCache> image_cache_;
    SDL_Window* window_ = nullptr;
    std::vector<StylesheetProvider> stylesheets_;
    bool initialized_ = false;
};
}
