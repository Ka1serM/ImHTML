#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "imhtml.hpp"

namespace ImHTML {
class HtmlWindow;
}

class UiComponent {
public:
    virtual ~UiComponent() = default;
    virtual const char* tag() const = 0;
    virtual void before_render(ImHTML::HtmlWindow&) {}
    virtual void draw(ImRect bounds, std::map<std::string, std::string> attributes,
                      ImHTML::HtmlWindow&) {
        (void)bounds;
        (void)attributes;
    }
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

class UiComponentRegistry {
public:
    template <typename TComponent, typename... TArguments>
    TComponent& register_component(ImHTML::HtmlWindow& application, TArguments&&... arguments) {
        auto component = std::make_unique<TComponent>(std::forward<TArguments>(arguments)...);
        TComponent& result = *component;
        register_component(std::move(component), application);
        return result;
    }

    void register_component(std::unique_ptr<UiComponent> component, ImHTML::HtmlWindow& application) {
        const std::string component_tag = component->tag();
        if (const auto existing = components_.find(component_tag); existing != components_.end()) {
            ImHTML::UnregisterCustomElement(existing->first.c_str());
            ImHTML::UnregisterCustomElementHtml(existing->first.c_str());
            components_.erase(existing);
        }

        UiComponent* instance = component.get();
        install(component_tag, instance, application);
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

    void install(ImHTML::HtmlWindow& application) {
        for (const auto& [tag, component] : components_) {
            install(tag, component.get(), application);
        }
    }

    void before_render(ImHTML::HtmlWindow& application) {
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
    static void install(const std::string& tag, UiComponent* instance, ImHTML::HtmlWindow& application) {
        ImHTML::RegisterCustomElement(tag.c_str(),
                                      [instance, &application](ImRect bounds,
                                                               std::map<std::string, std::string> attributes) {
                                          instance->draw(bounds, std::move(attributes), application);
                                      });
    }
    std::map<std::string, std::unique_ptr<UiComponent>> components_;
};
