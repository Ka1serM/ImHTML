#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "imhtml.hpp"

namespace ImHTML {
class UiApplication;
}

// Every native HTML component is a real object owned by UiComponentRegistry.
// The renderer only knows this small virtual interface; component state and
// drawing policy stay in the component implementation.
class UiComponent {
public:
    virtual ~UiComponent() = default;
    virtual const char* tag() const = 0;
    virtual void before_render(ImHTML::UiApplication&) {}
    virtual void draw(const ImHTML::NativeElementContext& context,
                      const std::map<std::string, std::string>& attributes,
                      ImHTML::UiApplication& application) {
        draw(context.border_box, attributes, application);
    }
    virtual void draw(ImRect bounds, const std::map<std::string, std::string>& attributes,
                      ImHTML::UiApplication&) {
        (void)bounds;
        (void)attributes;
    }
    virtual void end_draw(ImHTML::UiApplication&) {}
    virtual bool expands_html() const { return false; }
    virtual std::string_view html_template() const { return {}; }
    virtual std::string html(const std::map<std::string, std::string>& attributes) const {
        return ImHTML::ExpandHtmlTemplate(html_template(), attributes);
    }
    virtual std::string html(const std::map<std::string, std::string>& attributes,
                             std::string_view children) const {
        return ImHTML::ExpandHtmlTemplate(html_template(), attributes, children);
    }
    virtual std::string html(const std::map<std::string, std::string>& attributes, std::string_view children,
                             const ImHTML::HtmlElementContext&) const {
        return html(attributes, children);
    }
};

// Owns the registered component instances and installs each component's tag
// directly into ImHTML. A consuming application registers component classes,
// never raw callbacks or tag wiring.
class UiComponentRegistry {
public:
    template <typename TComponent, typename... TArguments>
    TComponent& register_component(ImHTML::UiApplication& application, TArguments&&... arguments) {
        auto component = std::make_unique<TComponent>(std::forward<TArguments>(arguments)...);
        TComponent& result = *component;
        register_component(std::move(component), application);
        return result;
    }

    void register_component(std::unique_ptr<UiComponent> component, ImHTML::UiApplication& application) {
        const std::string component_tag = component->tag();
        if (const auto existing = components_.find(component_tag); existing != components_.end()) {
            ImHTML::UnregisterCustomElement(existing->first.c_str());
            ImHTML::UnregisterCustomElementHtml(existing->first.c_str());
            components_.erase(existing);
        }

        UiComponent* instance = component.get();
        install(component_tag, instance, application);
        install_end(component_tag, instance, application);
        if (instance->expands_html()) {
            ImHTML::PrepareHtmlTemplate(instance->html_template());
            ImHTML::RegisterCustomElementHtml(
                component_tag.c_str(), [instance](const std::map<std::string, std::string>& attributes,
                                                  std::string_view children,
                                                  const ImHTML::HtmlElementContext& context) {
                    return instance->html(attributes, children, context);
                });
        }
        components_.emplace(component_tag, std::move(component));
    }

    void install(ImHTML::UiApplication& application) {
        for (const auto& [tag, component] : components_) {
            install(tag, component.get(), application);
            install_end(tag, component.get(), application);
        }
    }

    void before_render(ImHTML::UiApplication& application) {
        for (const auto& [tag, component] : components_) component->before_render(application);
    }

    void unregister_all() {
        for (const auto& component : components_) {
            ImHTML::UnregisterCustomElement(component.first.c_str());
            ImHTML::UnregisterCustomElementHtml(component.first.c_str());
        }
    }

    void clear() {
        unregister_all();
        components_.clear();
    }

private:
    static void install(const std::string& tag, UiComponent* instance, ImHTML::UiApplication& application) {
        ImHTML::RegisterCustomElement(tag.c_str(),
                                      [instance, &application](const ImHTML::NativeElementContext& context,
                                                               const std::map<std::string, std::string>& attributes) {
                                          instance->draw(context, attributes, application);
                                      });
    }
    static void install_end(const std::string& tag, UiComponent* instance, ImHTML::UiApplication& application) {
        ImHTML::RegisterCustomElementEnd(tag.c_str(), [instance, &application] { instance->end_draw(application); });
    }

    std::map<std::string, std::unique_ptr<UiComponent>> components_;
};
