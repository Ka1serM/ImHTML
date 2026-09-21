#pragma once

#include <array>
#include <set>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "imgui.h"
#include <imhtml/context.hpp>
#include <imhtml/core.hpp>

namespace litehtml {
class document;
class element;
}

namespace ImHTML {


class HtmlWindow;
class HtmlDocument;

/**
 * A handle to an element in the live document.
 *
 * Copyable and cheap. A handle keeps the element alive but does not keep it
 * mounted: after a fragment switch or a list update, an old handle can refer to
 * an element that is no longer in the tree, so query again rather than caching
 * handles across frames. Converts to false when empty.
 */
class Element {
public:
    Element();
    Element(const Element& other);
    Element(Element&& other) noexcept;
    Element& operator=(const Element& other);
    Element& operator=(Element&& other) noexcept;
    ~Element();

    explicit operator bool() const { return static_cast<bool>(element_) || static_cast<bool>(node_); }

    // Tag name, lowercased, or an empty string for an empty handle.
    std::string tag() const;
    // Value of an attribute, or `fallback` when the attribute is absent.
    std::string attribute(const std::string& name, const std::string& fallback = {}) const;
    // First descendant matching the CSS selector, or an empty handle.
    // DOM queries and template operations.
    Element query_selector(const std::string& selector) const;
    std::vector<Element> query_selector_all(const std::string& selector) const;
    Element content() const;
    Element clone_node(bool deep = false) const;
    std::string text_content() const;
    Element parent_element() const;
    // Border box in the screen coordinates ImGui and custom-element draw()
    // callbacks use. Reflects the most recent layout; empty if not rendered.
    Rect get_bounding_client_rect() const;

    // Builder/mutation API modelled after the JavaScript DOM. Elements made
    // by HtmlDocument::create_element() are detached until appended.
    Element& set_attribute(std::string name, std::string value);
    Element& set_text_content(std::string text);
    Element& append(Element child);
    Element& append_child(Element child);
    Element& replace_children();
    Element& replace_children(Element child);
    void remove();

private:
    friend class HtmlDocument;
    friend class Event;
    struct Node;
    // The framework's own tests reach through to litehtml; consumers use the
    // handle's own accessors.
    friend const std::shared_ptr<litehtml::element>& RawElement(const Element& element);
    explicit Element(std::shared_ptr<litehtml::element> element, HtmlDocument* owner = nullptr);
    explicit Element(std::shared_ptr<Node> node);
    std::string html() const;
    std::shared_ptr<litehtml::element> element_;
    std::shared_ptr<Node> node_;
    HtmlDocument* owner_ = nullptr;
};

class Event {
public:
    const std::string& type() const { return type_; }
    const std::string& target() const { return target_id_; }
    const std::string& current_target() const { return current_target_id_; }
    const std::string& value() const { return value_; }
    bool checked() const { return checked_; }
    const std::string& key() const { return key_; }
    std::string attribute(const std::string& name) const;
    Element closest(const std::string& selector) const;
    void prevent_default() { default_prevented_ = true; }
    bool default_prevented() const { return default_prevented_; }
    void stop_propagation() { propagation_stopped_ = true; }
    bool propagation_stopped() const { return propagation_stopped_; }

    // Screen-space pointer position, mirroring MouseEvent.clientX/clientY.
    // Only populated for mousedown/mousemove/mouseup.
    float client_x() const { return client_x_; }
    float client_y() const { return client_y_; }
    // Delta since last frame, mirroring MouseEvent.movementX/movementY.
    float movement_x() const { return movement_x_; }
    float movement_y() const { return movement_y_; }

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
    float client_x_ = 0.0f;
    float client_y_ = 0.0f;
    float movement_x_ = 0.0f;
    float movement_y_ = 0.0f;
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

    struct PerformanceStats {
        std::uint64_t frames = 0;
        std::uint64_t document_rebuilds = 0;
        std::uint64_t selector_scans = 0;
        std::uint64_t style_refreshes = 0;
        std::uint64_t layout_count = 0;
        std::uint64_t paint_count = 0;
        std::uint64_t fragment_updates = 0;
        double last_frame_ms = 0.0;
        double last_layout_ms = 0.0;
        double last_paint_ms = 0.0;
    };

    void register_shell(std::string html);
    void set_stylesheet_provider(std::function<std::string()> provider);

    using FragmentHtmlProvider = std::function<std::string()>;
    void register_fragment(const std::string& name, FragmentHtmlProvider html_provider);
    void register_html_control(std::unique_ptr<HtmlControl> control);

    // Supported event types: "click", "input", "change" (dispatched by the
    // native controls themselves) and "mousedown"/"mousemove"/"mouseup"
    // (dispatched every frame from raw mouse state, mirroring the DOM). A
    // mousedown match on a selector captures the pointer for that selector:
    // the matching mousemove/mouseup listeners keep firing every frame until
    // release, even once the cursor leaves the element, the same way a real
    // browser keeps delivering events to whichever element called
    // setPointerCapture.
    EventListenerId on(const std::string& event, const std::string& selector,
                       EventListener listener);
    void off(EventListenerId listener_id);
    bool dispatch_event(const std::string& id, const std::string& event,
                        const std::string& key = {});

    std::function<void()> before_render;
    std::function<void()> after_render;

    bool initialize(HtmlWindow* application = nullptr);
    // The context this document registers into and renders from. Captured when
    // the document is constructed; re-entered for the duration of each frame.
    Context& context() const { return *context_; }
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


    ScrollMetrics scroll_metrics(const std::string& id);
    const PerformanceStats& performance_stats() const { return performance_stats_; }
    void reset_performance_stats() { performance_stats_ = {}; }

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

    std::vector<Element> query_selector_all(const std::string& selector) const;
    Element query_selector(const std::string& selector) const;
    Element create_element(std::string tag);
    Element create_document_fragment();
    std::string value(const std::string& selector) const;
    double value_as_number(const std::string& selector, double fallback = 0.0) const;
    bool checked(const std::string& selector) const;


private:
    friend class Element;
    enum class DirtyState : std::uint8_t {
        Clean = 0,
        Paint = 1 << 0,
        Layout = 1 << 1,
        Structure = 1 << 2,
        Document = 1 << 3,
    };

    // Event::closest walks the document's cached selector matches.
    friend class Event;

    Context* context_ = &CurrentContext();

    void InvalidateDomCaches();
    void InvalidateSelectorCaches();
    void Invalidate(DirtyState state);
    // Raw element lookup for the framework's own use; the public API hands out
    // Element handles instead.
    std::shared_ptr<litehtml::element> SelectOne(const std::string& selector) const;
    std::shared_ptr<litehtml::element> MaterializeFragment(std::string_view html) const;
    void MutateElement(const std::shared_ptr<litehtml::element>& target,
                       std::string_view html, bool replace_existing);
    void DomChanged(const std::shared_ptr<litehtml::element>& target);
    const std::vector<std::shared_ptr<litehtml::element>>& CachedSelectorAll(
        const std::string& selector) const;
    struct SwitcherIndexEntry {
        std::shared_ptr<litehtml::element> switcher;
        std::string group;
        std::vector<std::shared_ptr<litehtml::element>> panels;
        std::vector<std::shared_ptr<litehtml::element>> controls;
    };
    const std::vector<SwitcherIndexEntry>& CachedSwitcherIndex() const;
    void RefreshStylesIfNeeded();
    void RefreshElementStyles(std::span<const std::shared_ptr<litehtml::element>> elements);
    void RestorePendingScrollOffsets();
    void RequestStructuralRebuild();
    std::string BuildFragmentHtml(const std::string& name) const;
    void SelectInitialFragment();
    void RebuildDocument();
    void MountLazyPanels();
    void SyncCheckboxes();
    void SyncTextInputs();
    void SyncSelects();
    void SyncRanges();
    void SyncColors();
    void DrawTextInputs(const ImVec2& document_origin);
    void UpdateSelectFromMouse(const ImVec2& document_origin);
    void BlockPointerBehindSelectPopup(const ImVec2& document_origin);
    void DrawSelects(const ImVec2& document_origin, bool popup_only = false);
    void UpdateRangeFromMouse(const ImVec2& document_origin);
    void DrawRanges(const ImVec2& document_origin) const;
    void UpdateColorFromMouse(const ImVec2& document_origin);
    void DrawColors(const ImVec2& document_origin);
    void DispatchPointerEvents(const ImVec2& document_origin);
    void ApplySwitchers();
    bool IsElementMounted(const std::shared_ptr<litehtml::element>& element) const;
    bool IsElementVisible(const std::shared_ptr<litehtml::element>& element) const;
    std::string SwitchControlTarget(const std::shared_ptr<litehtml::element>& element) const;
    std::string SwitchPanelKey(const std::shared_ptr<litehtml::element>& element) const;
    bool SwitchPanelMatches(const std::shared_ptr<litehtml::element>& element,
                            const std::string& requested) const;
    bool SelectPanelById(const std::string& id);
    bool ToggleCheckbox(const std::string& id);
    std::shared_ptr<litehtml::element> NearestSwitcher(const std::shared_ptr<litehtml::element>& element) const;
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
    void ApplyStoredAriaSelection();
    // Ctrl+A selects every selectable option of the multi-select listbox last clicked.
    void HandleListboxSelectAll();
    std::string active_listbox_id_;
    void RebuildRenderTreeForTabSwitch(bool structure_changed);
    std::string active_select_id_;
    std::string active_text_id_;
    std::string active_number_drag_id_;
    double number_drag_start_value_ = 0.0;
    float number_drag_start_y_ = 0.0f;
    int number_drag_click_direction_ = 0;
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
    bool select_popup_pointer_blocked_ = false;
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
    struct EventMatchCache {
        std::uint64_t generation = 0;
        std::unordered_set<const litehtml::element*> elements;
    };
    std::unordered_map<EventListenerId, EventMatchCache> event_match_cache_;
    std::uint64_t selector_generation_ = 1;
    EventListenerId next_event_listener_id_ = 1;
    HtmlWindow* application_ = nullptr;
    ImVec2 document_origin_{0.0f, 0.0f};
    // The selector/id a mousedown match captured the pointer for; empty when
    // no drag is in progress. See HtmlDocument::on() for the capture semantics.
    std::string pointer_capture_selector_;
    std::string pointer_capture_id_;

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
    struct PendingScrollOffset {
        std::shared_ptr<litehtml::element> target;
        // Plain floats so the header needs no litehtml definitions;
        // litehtml::pixel_t converts implicitly in both directions.
        float left = 0.0f;
        float top = 0.0f;
    };
    std::vector<PendingScrollOffset> pending_scroll_offsets_;
    std::unordered_map<std::string, std::weak_ptr<litehtml::element>> element_cache_;
    mutable std::unordered_map<std::string, std::vector<std::shared_ptr<litehtml::element>>>
        selector_cache_;
    mutable std::unordered_map<const litehtml::element*, std::weak_ptr<litehtml::element>>
        switcher_owner_cache_;
    mutable std::uint64_t switcher_index_generation_ = 0;
    mutable std::vector<SwitcherIndexEntry> switcher_index_;
    std::unordered_set<const litehtml::element*> mounted_lazy_panels_;
    // Short-lived switch/list work uses this arena so normal interaction does
    // not repeatedly ask the general-purpose allocator for tiny containers.
    // Persistent DOM and render objects intentionally remain heap-owned.
    static constexpr std::size_t kScratchArenaBytes = 32u * 1024u;
    std::array<std::byte, kScratchArenaBytes> scratch_storage_{};
    std::pmr::monotonic_buffer_resource scratch_arena_{
        scratch_storage_.data(), scratch_storage_.size()};
    DirtyState dirty_state_ = DirtyState::Document;
    bool styles_dirty_ = false;
    bool dom_render_tree_dirty_ = false;
    std::uint64_t dom_generation_ = 1;
    bool document_recreate_required_ = true;
    std::uint64_t frame_count_ = 0;
    std::uint64_t rebuild_count_ = 0;
    mutable PerformanceStats performance_stats_;
    std::chrono::steady_clock::time_point frame_started_at_;
};

}  // namespace ImHTML
