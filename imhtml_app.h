#pragma once

#include <set>

#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imhtml.hpp"

namespace litehtml {
class document;
class element;
}

namespace ImHTML {

class HtmlWindow;
class HtmlDocument;

class Event {
public:
    const std::string& type() const { return type_; }
    const std::string& target() const { return target_id_; }
    const std::string& current_target() const { return current_target_id_; }
    const std::string& value() const { return value_; }
    bool checked() const { return checked_; }
    const std::string& key() const { return key_; }
    std::string attribute(const std::string& name) const;
    std::string closest(const std::string& selector) const;
    void prevent_default() { default_prevented_ = true; }
    bool default_prevented() const { return default_prevented_; }
    void stop_propagation() { propagation_stopped_ = true; }
    bool propagation_stopped() const { return propagation_stopped_; }

private:
    friend class HtmlDocument;

    HtmlDocument* application_ = nullptr;
    std::shared_ptr<litehtml::element> target_element_;
    std::string type_;
    std::string target_id_;
    std::string current_target_id_;
    std::string value_;
    std::string key_;
    bool checked_ = false;
    bool default_prevented_ = false;
    bool propagation_stopped_ = false;
};

class HtmlControl {
public:
    virtual ~HtmlControl() = default;
    virtual void before_render(class HtmlDocument&, HtmlWindow&) {}
    virtual bool on_event(class HtmlDocument&, HtmlWindow&,
                          const std::string&, const std::string&) { return false; }
    virtual void on_text_focus(class HtmlDocument&, HtmlWindow&, const std::string&) {}
    virtual bool on_text_key(class HtmlDocument&, HtmlWindow&, const std::string&) { return false; }
    virtual bool on_text_changed(class HtmlDocument&, HtmlWindow&,
                                 const std::string&, const std::string&) { return false; }
};

class HtmlDocument {
public:
    using EventListenerId = std::uint64_t;
    using EventListener = std::function<void(Event&)>;

    enum class ElementUpdateMode {
        Layout,
        PaintOnly,
    };

    struct ScrollMetrics {
        bool valid = false;
        float top = 0.0f;
        float max_top = 0.0f;
        float viewport_width = 0.0f;
        float viewport_height = 0.0f;
        float content_height = 0.0f;
    };

    void register_shell(std::string html);
    void set_stylesheet_provider(std::function<std::string()> provider);

    using FragmentHtmlProvider = std::function<std::string()>;
    void register_fragment(const std::string& name, FragmentHtmlProvider html_provider);
    void register_html_control(std::unique_ptr<HtmlControl> control);

    EventListenerId on(const std::string& event, const std::string& selector,
                       EventListener listener);
    void off(EventListenerId listener_id);
    bool dispatch_event(const std::string& id, const std::string& event,
                        const std::string& key = {});

    std::function<void()> before_render;
    std::function<void()> after_render;

    bool initialize(HtmlWindow* application = nullptr);
    void frame();
    void shutdown();
    void mark_dirty();

    void select(const std::string& group, const std::string& key);
    bool handle_interaction(const std::string& url);
    const std::string& current_fragment() const { return current_fragment_; }

    void set_style(const std::string& selector, const std::string& css);
    void set_flex_pixels(const std::string& selector, long pixels);
    void set_text(const std::string& selector, const std::string& text);
    void set_attribute(const std::string& selector, const std::string& name,
                       const std::string& value,
                       ElementUpdateMode mode = ElementUpdateMode::PaintOnly);

    bool apply_selection(const std::string& id, bool ctrl_held, bool shift_held);

    std::vector<std::string> selected_ids(const std::string& listbox_id);

    void set_list(std::string id, std::vector<inja::json> items);

    ScrollMetrics scroll_metrics(const std::string& id);

    std::string attribute(const std::string& element_id, const std::string& name) const;

    bool has_value(const std::string& id) const;
    void set_value(const std::string& id, const std::string& value);
    void set_value(const std::string& id, const char* value) {
        set_value(id, std::string(value == nullptr ? "" : value));
    }
    void set_value(const std::string& id, bool value) { set_value(id, std::string(value ? "true" : "false")); }
    void set_value(const std::string& id, double value) { set_value(id, std::to_string(value)); }
    void set_checked(const std::string& id, bool checked);
    void set_options(const std::string& id, std::vector<std::string> options);
    const std::vector<std::string>& options(const std::string& id) const;

    std::vector<std::shared_ptr<litehtml::element>> query_selector_all(const std::string& selector) const;
    std::shared_ptr<litehtml::element> query_selector(const std::string& selector) const;
    void append_html(const std::string& selector, const std::string& html, bool replace_existing);
    std::string value(const std::string& selector) const;
    double value_as_number(const std::string& selector, double fallback = 0.0) const;
    bool checked(const std::string& selector) const;

    std::string enclosing_list(const std::string& element_id) const;

private:
    struct ListWindow {
        bool uniform = true;
        float row_height = 0.0f;
        std::size_t first = 0;
        std::size_t count = 0;
        float lead = 0.0f;
        float trail = 0.0f;
    };

    std::string ExpandListRows(const std::string& id, std::string_view item_template) const;
    bool UpdateListInPlace(const std::string& id);
    void UpdateListWindows();
    bool MeasureListRows(const std::string& id,
                         const std::shared_ptr<litehtml::element>& list_element);
    void ApplyPendingListUpdates();
    void RequestStructuralRebuild();
    std::string BuildFragmentHtml(const std::string& name) const;
    void SelectInitialFragment();
    void RebuildDocument();
    void MountLazyPanels();
    void SyncCheckboxes();
    void SyncTextInputs();
    void SyncSelects();
    void SyncRanges();
    void DrawTextInputs(const ImVec2& document_origin);
    void UpdateSelectFromMouse(const ImVec2& document_origin);
    void DrawSelects(const ImVec2& document_origin, bool popup_only = false);
    void UpdateRangeFromMouse(const ImVec2& document_origin);
    void DrawRanges(const ImVec2& document_origin) const;
    void ApplySwitchers() const;
    bool IsElementMounted(const std::shared_ptr<litehtml::element>& element) const;
    std::string SwitchControlTarget(const std::shared_ptr<litehtml::element>& element) const;
    std::string SwitchPanelKey(const std::shared_ptr<litehtml::element>& element) const;
    bool SwitchPanelMatches(const std::shared_ptr<litehtml::element>& element,
                            const std::string& requested) const;
    bool SelectPanelById(const std::string& id);
    bool ToggleCheckbox(const std::string& id);
    std::shared_ptr<litehtml::element> NearestSwitcher(const std::shared_ptr<litehtml::element>& element) const;
    std::string RenderListTemplate(const std::map<std::string, std::string>& attributes,
                                   std::string_view children, const HtmlElementContext& context) const;
    bool ApplyListSelectionPaint(const std::string& id,
                                 const std::vector<inja::json>& previous,
                                 const std::vector<inja::json>& next);
    bool HasFragment(const std::string& name) const;
    std::shared_ptr<litehtml::element> FindElement(const std::string& selector);
    bool DispatchHtmlControlEvent(const std::string& id, const std::string& event);
    void NotifyHtmlControlTextFocus(const std::string& id);
    bool HandleHtmlControlTextKey(const std::string& id);
    bool NotifyHtmlControlTextChanged(const std::string& id, const std::string& value);
    std::string state_value(const std::string& key, const std::string& fallback) const;
    void set_state(const std::string& key, const std::string& value);

    std::unordered_map<std::string, size_t> aria_selection_anchors_;
    std::unordered_map<std::string, std::set<std::string>> aria_selections_;
    void ApplyStoredAriaSelection() const;
    std::string active_select_id_;
    std::string active_text_id_;
    std::string active_range_id_;
    std::unordered_map<std::string, std::vector<std::string>> select_options_;
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::vector<std::string>> options_;
    struct TextInputState {
        std::string value;
        std::size_t cursor = 0;
        std::size_t anchor = 0;
        float scroll_x = 0.0f;
    };
    std::unordered_map<std::string, TextInputState> text_states_;
    std::unordered_map<std::string, float> select_scroll_offsets_;
    bool select_scroll_dragging_ = false;
    float select_scroll_drag_offset_ = 0.0f;
    bool text_mouse_selecting_ = false;
    std::vector<std::unique_ptr<HtmlControl>> html_controls_;
    struct EventListenerRegistration {
        EventListenerId id = 0;
        std::string event;
        std::string selector;
        EventListener listener;
    };
    std::vector<EventListenerRegistration> event_listeners_;
    EventListenerId next_event_listener_id_ = 1;
    HtmlWindow* application_ = nullptr;

    std::string shell_html_;
    std::function<std::string()> stylesheet_provider_;
    std::map<std::string, FragmentHtmlProvider> fragments_;
    std::string current_fragment_;
    std::string pending_fragment_;
    bool needs_rebuild_ = true;
    std::shared_ptr<litehtml::document> doc_;
    std::shared_ptr<litehtml::element> content_slot_;
    std::unordered_map<std::string, std::string> inline_styles_;
    std::unordered_map<std::string, std::string> element_texts_;
    std::unordered_map<std::string, std::vector<inja::json>> lists_;
    mutable std::unordered_map<std::string, std::string> list_templates_;
    mutable std::unordered_map<std::string, ListWindow> list_windows_;
    std::unordered_set<std::string> pending_list_updates_;
    std::unordered_map<std::string, std::weak_ptr<litehtml::element>> element_cache_;
    std::unordered_set<const litehtml::element*> mounted_lazy_panels_;
    std::uint64_t frame_count_ = 0;
    std::uint64_t rebuild_count_ = 0;
    std::uint64_t list_update_count_ = 0;
    std::uint64_t inactive_list_update_count_ = 0;
    std::uint64_t paint_only_list_update_count_ = 0;
    std::uint64_t in_place_list_update_count_ = 0;
};

}  // namespace ImHTML
