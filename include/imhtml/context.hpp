#pragma once

// Where ImHTML keeps its per-instance state.
//
// Fonts, image callbacks and custom-element registrations used to live in file
// scope, which meant a process had exactly one of each: a library embedding
// ImHTML in a tool panel could not register a <chart> element without the host
// seeing it too. A Context owns that state instead.
//
// There is always a current context — Context::Default() — so the free
// functions in core.hpp keep working unchanged. Hosts that need isolation
// create their own and scope it:
//
//     ImHTML::Context panel_context;
//     {
//         ImHTML::ContextScope scope(panel_context);
//         ImHTML::RegisterCustomElement("chart", draw_chart);
//     }
//
// HtmlDocument binds to whichever context was current when it was constructed
// and re-enters it for the duration of every frame, so documents in different
// contexts never see each other's registrations.
//
// Threading: a Context is single-threaded, and the current context is
// thread-local. Two threads can each drive their own context; sharing one
// context across threads is not supported.

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <imhtml/core.hpp>

namespace ImHTML {

class Context {
public:
    Context();
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // The context used when no other one is current.
    static Context& Default();

    Config& config();
    const Config& config() const;

    // Configuration overrides, as pushed by PushConfig/PopConfig.
    void push_config(const Config& config);
    void pop_config();

    void register_custom_element(const std::string& tag, CustomElementDrawFunction draw);
    void register_custom_element_html(const std::string& tag, CustomElementHtmlFunction render);
    void unregister_custom_element(const std::string& tag);
    void unregister_custom_element_html(const std::string& tag);

    const CustomElementDrawFunction* find_custom_element(const std::string& tag) const;
    const CustomElementHtmlFunction* find_custom_element_html(const std::string& tag) const;
    bool has_custom_element(const std::string& tag) const;

private:
    friend const Config* CurrentConfig();
    Config config_;
    std::vector<Config> config_stack_;
    std::unordered_map<std::string, CustomElementDrawFunction> draw_elements_;
    std::unordered_map<std::string, CustomElementHtmlFunction> html_elements_;
};

// The context the free functions act on, for this thread.
Context& CurrentContext();
void SetCurrentContext(Context& context);

// Makes `context` current for as long as the scope lives.
class ContextScope {
public:
    explicit ContextScope(Context& context);
    ~ContextScope();

    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    Context* previous_;
};

}  // namespace ImHTML
