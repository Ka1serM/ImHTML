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


#include "imhtml_app.h"
#include "ui/ui_theme.h"
#include "ui/ui_component_registry.h"

struct SDL_Window;

namespace ImHTML {
using ComponentRegistry = ::UiComponentRegistry;
using Component = ::UiComponent;

inline std::uint64_t next_unique_id() {
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

class HtmlWindow;
class UiImageCache;

class Stylesheet {
public:
    virtual ~Stylesheet() = default;
    virtual std::string css() const = 0;
};

class Fragment {
public:
    virtual ~Fragment() = default;
    virtual const char* id() const { return nullptr; }
    virtual std::string html() const = 0;
    virtual const char* tag() const { return nullptr; }
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
    virtual void register_ui(HtmlWindow&) {}
    virtual void draw(HtmlWindow&) {}
    virtual void tick() {}
    virtual void flush() {}
};

class FragmentRegistry {
public:
    template <typename TFragment>
    TFragment& register_fragment(HtmlDocument& document) {
        auto fragment = std::make_unique<TFragment>();
        TFragment& result = *fragment;
        Fragment* instance = fragment.get();
        fragments_.push_back(std::move(fragment));
        const std::string markup = instance->html();
        if (markup.find("<html") != std::string::npos) {
            document.register_shell(markup);
        } else if (const char* id = instance->id()) {
            document.register_fragment(id, [instance] { return instance->html(); });
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

    void register_ui(HtmlWindow& application) {
        for (const auto& fragment : fragments_) fragment->register_ui(application);
    }

    void tick() {
        for (const auto& fragment : fragments_) fragment->tick();
    }

    void flush() {
        for (const auto& fragment : fragments_) fragment->flush();
    }

    void draw(const std::string& fragment_id, HtmlWindow& application) {
        const auto found = fragments_by_id_.find(fragment_id);
        if (found != fragments_by_id_.end()) found->second->draw(application);
    }

private:
    std::vector<std::unique_ptr<Fragment>> fragments_;
    std::map<std::string, Fragment*> fragments_by_id_;
};

class HtmlWindow {
public:
    using StylesheetProvider = std::function<std::string()>;

    HtmlWindow();
    ~HtmlWindow();

    HtmlWindow(const HtmlWindow&) = delete;
    HtmlWindow& operator=(const HtmlWindow&) = delete;

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
    TStylesheet& register_stylesheet() {
        auto stylesheet = std::make_unique<TStylesheet>();
        TStylesheet& result = *stylesheet;
        embedded_stylesheets_.push_back(std::move(stylesheet));
        return result;
    }
    float base_font_size() const;

    template <typename TFragment>
    TFragment& register_fragment() { return fragments_.register_fragment<TFragment>(document_); }

    template <typename TComponent, typename... TArguments>
    TComponent& register_component(TArguments&&... arguments) {
        return components_.register_component<TComponent>(*this, std::forward<TArguments>(arguments)...);
    }
    template <typename TControl, typename... TArguments>
    TControl& register_html_control(TArguments&&... arguments) {
        auto control = std::make_unique<TControl>(*this, std::forward<TArguments>(arguments)...);
        TControl& result = *control;
        document_.register_html_control(std::move(control));
        return result;
    }
    HtmlDocument& document() { return document_; }
    const HtmlDocument& document() const { return document_; }
    SDL_Window* window() const { return window_; }

    bool initialize(SDL_Window* window, const std::string& font_path);
    bool initialize(SDL_Window* window, std::span<const unsigned char> font_data);
    void frame();
    void shutdown();
    void release_image_resources();

private:
    bool finish_initialize();
    std::string stylesheet() const;
    HtmlDocument document_;
    FragmentRegistry fragments_;
    std::vector<std::unique_ptr<Stylesheet>> embedded_stylesheets_;
    ComponentRegistry components_;
    std::unique_ptr<UiImageCache> image_cache_;
    SDL_Window* window_ = nullptr;
    std::vector<StylesheetProvider> stylesheets_;
    bool initialized_ = false;
};
}
