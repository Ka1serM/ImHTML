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

class UiApplication;
class HtmlApplication;

// C++ representation of a browser DOM Event. The event keeps the target as
// an HTML id because applications should not depend on litehtml internals.
// `closest()` follows the same ancestor-selector semantics as
// Element.closest() in a browser and returns the matching element's id.
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
    friend class HtmlApplication;

    HtmlApplication* application_ = nullptr;
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

// Generic lifecycle and interaction hooks for application-owned HTML controls.
// ImHTML supplies DOM and text-input plumbing; control semantics stay outside
// the renderer.
class HtmlControl {
public:
    virtual ~HtmlControl() = default;
    virtual void before_render(class HtmlApplication&, UiApplication&) {}
    virtual bool on_event(class HtmlApplication&, UiApplication&,
                          const std::string&, const std::string&) { return false; }
    virtual void on_text_focus(class HtmlApplication&, UiApplication&, const std::string&) {}
    virtual bool on_text_key(class HtmlApplication&, UiApplication&, const std::string&) { return false; }
    virtual bool on_text_changed(class HtmlApplication&, UiApplication&,
                                 const std::string&, const std::string&) { return false; }
};

// A reusable fragment application framework built on top of ImHTML's
// Parse/RenderDocument: assembles a document out of one embedded/provider
// supplied shell and one HTML file per reusable
// component — Avalonia/Svelte-style, one HTML file per named UI piece — and
// handles scoped switchers and external-link activation through the
// real litehtml DOM. HTML-only fragment templates are expanded before parsing.
//
// The embedding app only needs to: register its shell/fragments, register
// any custom-drawn widgets, and call frame() once per ImGui frame. It never
// touches the litehtml document directly.
//
// Convention the HTML must follow: a sidebar slot `id="sidebar-nav"` and a
// content slot `id="content-slot"`. Internal selection uses ordinary HTML with
// a `data-switcher` scope, local-fragment links, and panels using
// `role="tabpanel"` plus an `id`. Nested switchers always mount only the
// selected registered panel fragment and keep inactive panel placeholders empty.
class HtmlApplication {
public:
    using EventListenerId = std::uint64_t;
    using EventListener = std::function<void(Event&)>;

    enum class TextUpdateMode {
        Layout,
        PaintOnly,
    };

    enum class ElementUpdateMode {
        Layout,
        PaintOnly,
    };

    // Live geometry of a scrollable element.
    struct ScrollMetrics {
        bool valid = false;
        float top = 0.0f;
        float max_top = 0.0f;
        float viewport_width = 0.0f;
        float viewport_height = 0.0f;
        float content_height = 0.0f;
    };

    // Registers the application shell fragment. The fragment registry uses
    // this alongside ordinary fragments.
    void register_shell(std::string html);
    // Supplies the full <style> body (theme CSS) injected after <head>;
    // called on every rebuild (so live theme switching just needs a
    // rebuild, not a new HtmlApplication).
    void set_stylesheet_provider(std::function<std::string()> provider);

    using FragmentHtmlProvider = std::function<std::string()>;
    // Registers a fragment whose markup is supplied by code. This is the
    // native path for C++23 projects using #embed; no runtime file lookup is
    // needed.
    void register_fragment(const std::string& name, FragmentHtmlProvider html_provider);
    void register_html_control(std::unique_ptr<HtmlControl> control);

    // Browser-style delegated event listener. The selector is matched against
    // the event target and its ancestors, just like a listener attached to a
    // DOM container using event delegation in a browser.
    EventListenerId on(const std::string& event, const std::string& selector,
                       EventListener listener);
    void off(EventListenerId listener_id);
    bool dispatch_event(const std::string& id, const std::string& event,
                        const std::string& key = {});

    // Called every frame(), inside the same Begin/End the document renders
    // in — before_render right before RenderDocument, after_render right
    // after (both still inside the window, before End()) — so the app can
    // draw its own overlays (custom widget hit-testing, motion effects)
    // against the same draw list without owning the window itself.
    std::function<void()> before_render;
    std::function<void()> after_render;

    bool initialize(UiApplication* application = nullptr);
    // Draws the whole app inside its own full-window, chromeless ImGui
    // window (Begin/End handled internally) and processes switcher/external
    // interactions.
    void frame();
    void shutdown();
    // Forces the next frame() to fully rebuild the document (e.g. after a
    // theme/DPI change).
    void mark_dirty();

    // Selects an item in any scoped switcher. The change is committed at a
    // frame boundary, just like a clicked button.
    void select(const std::string& group, const std::string& key);
    // Resolves an external URL or an internal switcher event emitted by the
    // litehtml DOM.
    bool handle_interaction(const std::string& url);
    const std::string& current_fragment() const { return current_fragment_; }

    // Live DOM mutation for a value that doesn't add new structure — e.g. a
    // movable splitter writing its dragged size straight into the target
    // element's inline style, no rebuild needed. `css` is a full
    // declaration list, e.g. "flex:0 0 320px".
    void set_style(const std::string& selector, const std::string& css);
    // Fast path for interactive splitters: update the computed flex tuple
    // without reparsing a declaration list on every mouse-move frame.
    void set_flex_pixels(const std::string& selector, long pixels);
    // Updates the text node of a live HTML element without rebuilding the DOM.
    // PaintOnly skips layout only when the text's measured width and height
    // remain unchanged; otherwise it falls back to the normal layout path.
    void set_text(const std::string& selector, const std::string& text,
                  TextUpdateMode mode = TextUpdateMode::Layout);
    // Mutates a live DOM attribute without rebuilding the document. PaintOnly
    // is appropriate for state/style selectors; Layout invalidates only the
    // affected document layout while preserving its render tree and scrolls.
    void set_attribute(const std::string& selector, const std::string& name,
                       const std::string& value,
                       ElementUpdateMode mode = ElementUpdateMode::PaintOnly);

    // Applies standard listbox selection semantics to a clicked element, so no
    // application callback has to implement them.
    //
    // Opt in with ARIA rather than a bespoke attribute: put role="listbox" (plus
    // aria-multiselectable="true" to allow more than one) on the container and
    // role="option" on the items. Elements without that markup are untouched and
    // the method reports false, so ordinary buttons keep behaving as buttons.
    //
    //   plain click  - the clicked option becomes the entire selection
    //   ctrl+click   - toggles the clicked option, leaving the rest alone
    //   shift+click  - selects the contiguous range from the anchor
    //
    // The anchor is the last option clicked without shift, matching every file
    // manager and list control. Selection is written back as aria-selected plus a
    // "selected" class so stylesheets can target either.
    bool apply_selection(const std::string& id, bool ctrl_held, bool shift_held);

    // Ids of the currently selected options under a listbox, in document order.
    std::vector<std::string> selected_ids(const std::string& listbox_id);

    // Supplies the items for standard HTML list templates. The list is
    // rendered during the next document assembly; the JSON payload is only
    // the C++ representation of the row data used by the HTML template.
    //
    // Lists are virtualized: however many items are handed over, only the rows
    // that the list's scroll container can actually show are turned into DOM.
    // Nothing about that is visible to the caller - it publishes the whole
    // model and reads selection back by row id as usual.
    void set_list(std::string id, std::vector<inja::json> items);

    // Scroll position and viewport of a scrollable element, empty (valid=false)
    // until the element has been laid out and actually scrolls.
    ScrollMetrics scroll_metrics(const std::string& id);

    // One attribute of an element, empty when either is absent.
    std::string attribute(const std::string& element_id, const std::string& name) const;

    // Browser-shaped DOM helpers. Values are keyed by element id when the
    // document is not mounted yet; once mounted, value()/checked() read the
    // live element property first, just like a browser DOM lookup.
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

    // Generic DOM helpers for application-owned controls. These expose no
    // control-specific semantics; callers decide what their markup means.
    std::vector<std::shared_ptr<litehtml::element>> query_selector_all(const std::string& selector) const;
    std::shared_ptr<litehtml::element> query_selector(const std::string& selector) const;
    void append_html(const std::string& selector, const std::string& html, bool replace_existing);
    std::string value(const std::string& selector) const;
    double value_as_number(const std::string& selector, double fallback = 0.0) const;
    bool checked(const std::string& selector) const;

    // Id of the list an element belongs to, empty when it is not in one. This
    // is what lets a view bind one handler per list instead of one per row -
    // the only workable option once a list is long enough to be virtualized.
    std::string enclosing_list(const std::string& element_id) const;

private:
    // Which slice of a list is currently built, and what the rows measured.
    struct ListWindow {
        // Rows have to be uniformly tall for a scroll offset to map onto a row
        // index. A list whose template says otherwise is materialized whole.
        bool uniform = true;
        float row_height = 0.0f;
        std::size_t first = 0;
        std::size_t count = 0;
        // Height standing in for the rows before and after the window, emitted
        // as spacer elements so the scrollbar still measures the whole list.
        float lead = 0.0f;
        float trail = 0.0f;
    };

    // Expands the item template for the rows inside the list's window, framed
    // by the two spacers. This is the one place list markup is produced, for
    // both the document build and the in-place updates.
    std::string ExpandListRows(const std::string& id, std::string_view item_template) const;
    // Re-expands one list's rows straight into the live DOM. A list whose window
    // moves while the user scrolls it cannot go through a document rebuild: that
    // would discard the very scroll offset the new rows were chosen for. Returns
    // false when the list has no live render tree yet, in which case the caller
    // falls back to the structural rebuild.
    bool UpdateListInPlace(const std::string& id);
    // Measures the materialized rows of every live list and re-windows the ones
    // whose viewport moved. Runs after the frame is rendered; any list that
    // needs different rows is rebuilt at the top of the next frame, never while
    // the document is being walked.
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

    // Shift-range anchor per listbox id: the last option clicked without shift.
    std::unordered_map<std::string, size_t> aria_selection_anchors_;
    // Selection is owned here rather than in the DOM so it survives set_list()
    // rebuilds. Keyed by listbox id, holding selected option ids.
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
    UiApplication* application_ = nullptr;

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
    // Item template source per list id, captured while the document is
    // assembled so a later in-place update can re-expand it on its own.
    mutable std::unordered_map<std::string, std::string> list_templates_;
    mutable std::unordered_map<std::string, ListWindow> list_windows_;
    // Lists whose rows are stale, applied at the top of the next frame.
    std::unordered_set<std::string> pending_list_updates_;
    std::unordered_map<std::string, std::weak_ptr<litehtml::element>> element_cache_;
    std::unordered_set<const litehtml::element*> mounted_lazy_panels_;
    std::uint64_t frame_count_ = 0;
    std::uint64_t rebuild_count_ = 0;
    std::uint64_t list_update_count_ = 0;
    std::uint64_t inactive_list_update_count_ = 0;
    std::uint64_t paint_only_list_update_count_ = 0;
    std::uint64_t in_place_list_update_count_ = 0;
    std::uint64_t paint_only_text_update_count_ = 0;
};

}  // namespace ImHTML
