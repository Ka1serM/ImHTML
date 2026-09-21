// Regression coverage for persistent nested switchers and single-pass list
// updates. These interactions must not fall back to a document reparse.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "harness.h"
#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace {

int g_failures = 0;

void Check(const bool condition, const std::string& message) {
    if (condition) {
        std::printf("ok   %s\n", message.c_str());
    } else {
        std::printf("FAIL %s\n", message.c_str());
        ++g_failures;
    }
}

int RenderedTextNodes(const std::shared_ptr<litehtml::element>& element) {
    if (!element) return 0;
    if (element->is_text()) {
        std::string text;
        element->get_text(text);
        const bool non_empty = std::any_of(text.begin(), text.end(),
                                           [](const unsigned char ch) { return !std::isspace(ch); });
        const auto render = element->get_render_item();
        bool has_box = false;
        if (render) {
            render->get_rendering_boxes([&has_box](const litehtml::position& box) {
                has_box = has_box || (box.width > litehtml::pixel_t(0) && box.height > litehtml::pixel_t(0));
            });
        }
        return non_empty && has_box ? 1 : 0;
    }
    int count = 0;
    for (const auto& child : element->children()) count += RenderedTextNodes(child);
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    ImHTMLTests::DataRoot() = argc > 1 ? std::filesystem::path(argv[1])
                                       : std::filesystem::path(IMHTML_BENCH_DATA_DIR);
    ImHTMLTests::SetupHeadlessImGui();
    if (std::getenv("IMHTML_DISABLE_LAYOUT_CACHE") != nullptr) {
        litehtml::render_item::g_layout_reuse = false;
    }

    ImHTML::HtmlDocument document;
    ImHTMLTests::BuildSampleDocument(document);
    ImHTMLTests::RunFrames(document, 5);

    // Startup must have a deterministic active tab before any user input.
    int startup_visible = 0;
    for (const char* candidate : {"outliner", "environment", "renderer", "camera", "details"}) {
        const std::string panel_id = std::string("maps-panel-") + candidate;
        const auto panel = ImHTMLTests::FindRaw(document, "#" + panel_id);
        const auto control = ImHTMLTests::FindRaw(document, "#maps-tab-" + std::string(candidate));
        const bool visible = panel && !panel->is_hidden();
        if (visible) ++startup_visible;
        Check(visible == (std::string(candidate) == "outliner"),
              std::string("startup selects ") + candidate + " panel");
        const bool active = control &&
                            std::string(control->get_attr("class", "")).find("active") != std::string::npos;
        Check(active == (std::string(candidate) == "outliner"),
              std::string("startup marks ") + candidate + " tab state");
    }
    Check(startup_visible == 1, "startup has exactly one visible tab panel");

    // Exercise the path the application actually uses: ImGui mouse state ->
    // litehtml hit testing -> anchor URL -> HtmlDocument selection. Direct
    // select()/handle_interaction() calls cannot catch coordinate or event
    // lifetime bugs in that path.
    const auto click = [&](const char* id) {
        const auto element = ImHTMLTests::FindRaw(document, std::string("#") + id);
        const auto render = element ? element->get_render_item() : nullptr;
        if (!render) return false;
        litehtml::position box;
        render->get_rendering_boxes([&](const litehtml::position& rendered) {
            if (box.width <= litehtml::pixel_t(0) || box.height <= litehtml::pixel_t(0)) box = rendered;
        });
        if (box.width <= litehtml::pixel_t(0) || box.height <= litehtml::pixel_t(0)) return false;
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = ImVec2(static_cast<float>(box.x + box.width / 2),
                             static_cast<float>(box.y + box.height / 2));
        io.MouseDown[ImGuiMouseButton_Left] = true;
        ImHTMLTests::RunFrame(document);
        io.MouseDown[ImGuiMouseButton_Left] = false;
        ImHTMLTests::RunFrame(document);
        // The URL is dispatched after painting the release frame. Advance once
        // more so the newly reconciled render tree has completed its first
        // layout before geometry and hit testing are inspected.
        ImHTMLTests::RunFrame(document);
        return true;
    };
    for (const char* tab : {"environment", "renderer", "camera", "details", "outliner"}) {
        const std::string tab_id = std::string("maps-tab-") + tab;
        const std::string panel_id = std::string("maps-panel-") + tab;
        Check(click(tab_id.c_str()), std::string("real click reaches ") + tab);
        const auto panel = ImHTMLTests::FindRaw(document, "#" + panel_id);
        Check(panel && !panel->is_hidden(), std::string("real click selects ") + tab);
        for (const char* candidate : {"outliner", "environment", "renderer", "camera", "details"}) {
            const auto control = ImHTMLTests::FindRaw(document, std::string("#maps-tab-") + candidate);
            const auto control_render = control ? control->get_render_item() : nullptr;
            const auto candidate_panel = ImHTMLTests::FindRaw(document, 
                std::string("#maps-panel-") + candidate);
            const bool selected = std::string(candidate) == tab;
            const auto control_box = control_render ? control_render->get_placement() : litehtml::position{};
            Check(control_render && control_render->is_visible() && control_box.width > litehtml::pixel_t(0) &&
                      control_box.height > litehtml::pixel_t(0),
                  std::string("loaded tab keeps geometry: ") + candidate);
            Check(candidate_panel && candidate_panel->is_hidden() == !selected,
                  std::string("only selected panel is visible: ") + candidate);
        }
    }
    ImGui::GetIO().MousePos = ImVec2(-100.0f, -100.0f);

    // Visibility is inherited by the whole mounted subtree. A panel that has
    // already been laid out must stop painting and hit testing as soon as its
    // parent tab is hidden, and inactive descendants must not be rebuilt into
    // the render tree.
    document.select("maps", "environment");
    ImHTMLTests::RunFrame(document);
    const auto environment_control = ImHTMLTests::FindRaw(document, "#environment-exposure");
    const auto environment_render = environment_control ? environment_control->get_render_item() : nullptr;
    Check(environment_control && environment_control->is_visible(),
          "mounted panel descendant starts visible");
    Check(environment_render && environment_render->is_visible(),
          "mounted panel descendant render item starts visible");
    const auto selected_panel = ImHTMLTests::FindRaw(document, "#maps-panel-environment");
    const auto selected_panel_render = selected_panel ? selected_panel->get_render_item() : nullptr;
    const auto selected_panel_placement = selected_panel_render
        ? selected_panel_render->get_placement() : litehtml::position{};
    Check(selected_panel_render && selected_panel_placement.width > litehtml::pixel_t(0) &&
              selected_panel_placement.height > litehtml::pixel_t(0),
          "selected panel starts with non-zero placement " +
              std::to_string(static_cast<float>(selected_panel_placement.width)) + "x" +
              std::to_string(static_cast<float>(selected_panel_placement.height)));
    document.select("maps", "outliner");
    ImHTMLTests::RunFrame(document);
    Check(environment_control && !environment_control->is_visible(),
          "hidden panel descendant is not visible");
    Check(environment_render && !environment_render->is_visible(),
          "hidden panel descendant render item is excluded");
    const auto hidden_panel_render = ImHTMLTests::FindRaw(document, "#maps-panel-environment")
        ? ImHTMLTests::FindRaw(document, "#maps-panel-environment")->get_render_item()
        : nullptr;
    const auto hidden_panel_placement = hidden_panel_render
        ? hidden_panel_render->get_placement() : litehtml::position{};
    Check(hidden_panel_render && hidden_panel_placement.width > litehtml::pixel_t(0) &&
              hidden_panel_placement.height > litehtml::pixel_t(0),
          "hidden panel retains its last non-zero placement " +
              std::to_string(static_cast<float>(hidden_panel_placement.width)) + "x" +
              std::to_string(static_cast<float>(hidden_panel_placement.height)));

    document.select("maps", "environment");
    ImHTMLTests::RunFrame(document);
    const auto restored_panel_render = ImHTMLTests::FindRaw(document, "#maps-panel-environment")
        ? ImHTMLTests::FindRaw(document, "#maps-panel-environment")->get_render_item()
        : nullptr;
    const auto restored_panel_placement = restored_panel_render
        ? restored_panel_render->get_placement() : litehtml::position{};
    Check(restored_panel_render && restored_panel_render->is_visible() &&
              restored_panel_placement.width > litehtml::pixel_t(0) &&
              restored_panel_placement.height > litehtml::pixel_t(0),
          "reselected panel restores non-zero placement " +
              std::to_string(static_cast<float>(restored_panel_placement.width)) + "x" +
              std::to_string(static_cast<float>(restored_panel_placement.height)));

    document.select("maps", "renderer");
    ImHTMLTests::RunFrame(document);
    for (const char* id : {"renderer-tonemapping", "renderer-aov", "renderer-overlays"}) {
        const auto checkbox = ImHTMLTests::FindRaw(document, std::string("#") + id);
        const auto color = checkbox ? checkbox->css().get_bg().m_color : litehtml::web_color{};
        Check(checkbox && checkbox->get_attr("data-checked", "") == std::string("true") &&
                  color == litehtml::web_color(59, 156, 82),
              std::string("checked renderer checkbox is styled: ") + id);
        Check(ImHTMLTests::FindRaw(document, std::string("#") + id + ":checked") == checkbox,
              std::string("checked state is exposed through CSS selectors: ") + id);
    }
    const auto renderer_panel = ImHTMLTests::FindRaw(document, "#maps-panel-renderer");
    Check(RenderedTextNodes(renderer_panel) >= 8, "renderer text nodes have layout boxes");
    Check(ImHTML::GetDocumentRenderStats("app").text_runs >= 8,
          "renderer tab emits text paint commands before any resize");
    document.select("maps", "outliner");
    ImHTMLTests::RunFrame(document);
    document.select("maps", "renderer");
    ImHTMLTests::RunFrame(document);
    Check(RenderedTextNodes(renderer_panel) >= 8, "renderer text nodes survive tab refresh");
    document.set_checked("renderer-denoise", true);
    ImHTMLTests::RunFrame(document);
    const auto denoise = ImHTMLTests::FindRaw(document, "#renderer-denoise");
    const auto denoise_color = denoise ? denoise->css().get_bg().m_color : litehtml::web_color{};
    Check(denoise && denoise_color == litehtml::web_color(59, 156, 82),
          "runtime checked renderer checkbox refreshes its styling");
    Check(ImHTMLTests::FindRaw(document, "#renderer-denoise:checked") == denoise,
          "runtime checked state matches :checked");
    document.set_checked("renderer-denoise", false);
    Check(!ImHTMLTests::FindRaw(document, "#renderer-denoise:checked") &&
              ImHTMLTests::FindRaw(document, "#renderer-denoise:unchecked") == denoise,
          "runtime unchecked state updates dynamic selectors");

    // Idempotent selection must repair stale render state instead of trusting
    // the stored key and returning early.
    if (const auto startup_panel = ImHTMLTests::FindRaw(document, "#maps-panel-outliner")) {
        startup_panel->set_hidden(true);
        document.select("maps", "outliner");
        ImHTMLTests::RunFrame(document);
        Check(!startup_panel->is_hidden(), "reselect repairs stale active panel state");
    }
    document.reset_performance_stats();

    static const char* const panels[] = {
        "maps-panel-outliner", "maps-panel-environment", "maps-panel-renderer",
        "maps-panel-camera", "maps-panel-details", "maps-panel-environment",
        "maps-panel-renderer"};
    for (const char* panel : panels) {
        document.select("main", "maps");
        document.select("maps", panel);
        ImHTMLTests::RunFrame(document);

        for (const char* candidate : {"maps-panel-outliner", "maps-panel-environment",
                                      "maps-panel-renderer", "maps-panel-camera",
                                      "maps-panel-details"}) {
            const auto element = ImHTMLTests::FindRaw(document, std::string("#") + candidate);
            const bool visible = element && !element->is_hidden();
            Check(visible == (std::string(candidate) == panel),
                  std::string("only selected tab panel renders: ") + candidate);
        }
    }

    // The application uses short keys in some command paths. Stress that
    // form as well as full panel ids so a selection cannot get stuck after
    // repeated transitions.
    for (int index = 0; index < 200; ++index) {
        const char* key = panels[index % 5] + std::strlen("maps-panel-");
        document.select("maps", key);
        ImHTMLTests::RunFrame(document);
        const std::string selected = std::string("maps-panel-") + key;
        const auto active = ImHTMLTests::FindRaw(document, "#" + selected);
        Check(active && !active->is_hidden(), "short-key switching remains active");
    }

    const auto& switch_stats = document.performance_stats();
    Check(switch_stats.document_rebuilds == 0,
          "nested panel switches avoid document rebuilds after initial mount");
    Check(switch_stats.fragment_updates == 0,
          "nested panel switches retain fragment DOM");
    Check(switch_stats.layout_count >= 1,
          "nested panel switches still relayout the visible panel");

    // Exercise the same URL path used by anchor clicks, including repeated
    // switches back to an already-mounted panel.
    for (const char* panel : {"maps-panel-camera", "maps-panel-outliner",
                              "maps-panel-camera", "maps-panel-details",
                              "maps-panel-outliner"}) {
        Check(document.handle_interaction(std::string("#") + panel),
              std::string("URL switch resolves ") + panel);
        ImHTMLTests::RunFrame(document);
        for (const char* candidate : {"maps-panel-outliner", "maps-panel-environment",
                                      "maps-panel-renderer", "maps-panel-camera",
                                      "maps-panel-details"}) {
            const auto element = ImHTMLTests::FindRaw(document, std::string("#") + candidate);
            Check(element && element->is_hidden() == (std::string(candidate) != panel),
                  std::string("URL switch selects exactly one panel: ") + candidate);
        }
    }

    // Cloned rows are ordinary DOM nodes; batched insertion lays out once.
    document.select("maps", "maps-panel-outliner");
    ImHTMLTests::RunFrame(document);
    auto make_tree_rows = [&](const char* prefix, int count) {
        auto rows = document.create_document_fragment();
        auto content = document.query_selector("#maps-tree-template").content();
        for (int index = 0; index < count; ++index) {
            auto row = content.clone_node(true);
            row.query_selector("button").set_attribute("id", std::string(prefix) + std::to_string(index));
            row.query_selector("[data-field=name]").set_text_content(std::string(prefix) + " folder " + std::to_string(index));
            rows.append_child(row);
        }
        return rows;
    };
    const auto before = document.performance_stats().layout_count;
    document.query_selector("#maps-tree").replace_children(make_tree_rows("old-tree-", 120));
    ImHTMLTests::RunFrame(document);
    Check(document.performance_stats().layout_count == before + 1,
          "batched cloned rows perform one document layout");
    Check(document.query_selector_all("#maps-tree button").size() == 120,
          "all cloned rows are available through DOM queries");
    document.query_selector("#maps-tree").replace_children(make_tree_rows("new-tree-", 2));
    ImHTMLTests::RunFrames(document, 2);
    Check(!document.query_selector("#old-tree-0") && document.query_selector("#new-tree-0"),
          "replacing children removes the previous folder");
    Check(RenderedTextNodes(ImHTMLTests::FindRaw(document, "#new-tree-0")) > 0,
          "folder replacement renders its cloned text immediately");

    // Values may be populated before a nested panel exists. Mounting the Options
    // panel must reconcile those stored values instead of leaving its controls at
    // their HTML defaults.
    document.select("main", "settings");
    ImHTMLTests::RunFrame(document);
    document.set_checked("load-actors", true);
    document.select("settings", "settings-panel-options");
    ImHTMLTests::RunFrame(document);
    Check(document.checked("#load-actors"),
          "newly mounted settings checkbox applies stored true value");
    Check(document.attribute("load-actors", "data-checked") == "true",
          "newly mounted settings checkbox paints stored true value");

    document.set_checked("load-actors", false);
    Check(!document.checked("#load-actors"),
          "mounted settings checkbox accepts stored false value");
    document.select("settings", "settings-panel-provider");
    document.select("settings", "settings-panel-options");
    ImHTMLTests::RunFrame(document);
    Check(!document.checked("#load-actors"),
          "settings checkbox retains false value after tab switching");

    // Selection, delegated events, and removal operate on the actual nodes.
    document.select("settings", "settings-panel-versions");
    ImHTMLTests::RunFrame(document);
    auto option_content = document.query_selector("#options-template").content();
    for (int index = 0; index < 2; ++index) {
        auto row = option_content.clone_node(true);
        auto id = "option-" + std::to_string(index);
        row.query_selector("li").set_attribute("id", id).set_attribute("data-index", std::to_string(index));
        row.query_selector("input[name=name]").set_attribute("id", id + "-name")
            .set_attribute("value", index == 0 ? "First" : "Second");
        row.query_selector("input[name=value]").set_attribute("id", id + "-value");
        document.query_selector("#options").append_child(row);
        Check(row.query_selector_all("li").empty(), "append consumes the cloned document fragment");
    }
    ImHTMLTests::RunFrame(document);
    auto survivor = ImHTMLTests::FindRaw(document, "#option-1");
    std::string edited_index;
    document.on("input", "#options input[name=name]", [&](ImHTML::Event& event) {
        edited_index = event.closest("[data-index]").attribute("data-index");
    });
    Check(document.dispatch_event("option-0-name", "input") && edited_index == "0",
          "delegated events read row data through the DOM");
    document.apply_selection("option-0", false, false);
    for (auto row : document.query_selector_all("#options [aria-selected=true]")) row.remove();
    ImHTMLTests::RunFrame(document);
    Check(document.query_selector_all("#options [role=option]").size() == 1 &&
          ImHTMLTests::FindRaw(document, "#option-1") == survivor,
          "removing a row preserves the surviving node identity");

    // An input listener observes an edit but does not become responsible for
    // storing the control's value. The native editor must retain the value
    // after blur, just like an HTML input's value property does in a browser.
    document.select("settings", "settings-panel-provider");
    document.set_value("export-path", "seed");
    std::string observed_input;
    document.on("input", "#export-path", [&](ImHTML::Event& event) {
        observed_input = event.value();
    });
    ImHTMLTests::RunFrame(document);
    Check(click("export-path"), "text input receives focus through hit testing");
    ImGui::GetIO().AddInputCharactersUTF8("x");
    ImHTMLTests::RunFrame(document);
    Check(observed_input == "seedx", "input listener observes the edited value");

    ImGui::GetIO().MousePos = ImVec2(-100.0f, -100.0f);
    ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] = true;
    ImHTMLTests::RunFrame(document);
    ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] = false;
    ImHTMLTests::RunFrames(document, 2);
    Check(document.value("#export-path") == "seedx",
          "text edit remains committed after focus loss");

    // Exercise the editable arrow through the same native mouse path as select.
    auto editable = document.create_element("div");
    editable.set_attribute("id", "editable-test").set_attribute("data-editable-input", "editable-text")
        .set_attribute("style", "position:fixed;left:100px;top:100px;width:220px;height:36px;font-size:16px;background:#25262a;color:white");
    auto input = document.create_element("input");
    input.set_attribute("id", "editable-text").set_attribute("type", "text")
        .set_attribute("style", "width:180px;height:36px");
    editable.append_child(input);
    auto datalist = document.create_element("datalist");
    datalist.set_attribute("style", "display:none");
    for (const char* value : {"First", "Second"}) {
        auto option = document.create_element("option");
        option.set_attribute("value", value);
        datalist.append_child(option);
    }
    editable.append_child(datalist);
    document.query_selector("body").append_child(editable);
    std::string picked;
    document.on("change", "#editable-text", [&](ImHTML::Event& event) { picked = event.value(); });
    ImHTMLTests::RunFrames(document, 2);
    const auto click_at = [&](float x, float y) {
        ImGui::GetIO().MousePos = ImVec2(x, y);
        ImGui::GetIO().MouseDown[0] = true;
        ImHTMLTests::RunFrame(document);
        ImGui::GetIO().MouseDown[0] = false;
        ImHTMLTests::RunFrame(document);
    };
    click_at(305, 118);
    click_at(140, 166);
    Check(picked == "First", "editable chevron opens native popup and option dispatches change");
    picked.clear();
    click_at(305, 118);
    click_at(305, 118);
    click_at(140, 166);
    Check(picked.empty(), "editable chevron toggles native popup closed");
    click_at(140, 118);
    click_at(140, 202);
    Check(document.value("#editable-text") == "First",
          "clicking editable text does not open the popup");

    // Browser-style templates must survive parsing and populate an empty flex
    // container without reparsing the document on attribute changes.
    auto template_node = document.create_element("template");
    template_node.set_attribute("id", "clone-template").set_attribute("style", "display:none");
    auto prototype = document.create_element("div");
    prototype.set_attribute("class", "cloned-tile").set_attribute("style", "width:100px;height:100px");
    auto label = document.create_element("span");
    label.set_attribute("data-field", "name");
    prototype.append_child(label);
    template_node.content().append_child(prototype);
    auto grid_node = document.create_element("div");
    grid_node.set_attribute("id", "clone-grid")
        .set_attribute("style", "display:flex;position:fixed;left:10px;top:10px;width:300px;height:100px");
    document.query_selector("body").append_child(template_node).append_child(grid_node);
    ImHTMLTests::RunFrames(document, 2);
    auto source = document.query_selector("#clone-template");
    Check(bool(source), "HTML template survives parsing");
    auto fragment = source.content().clone_node(true);
    fragment.query_selector("[data-field=name]").set_text_content("Cloned file");
    fragment.query_selector(".cloned-tile").set_attribute("title", "Full file name");
    auto grid = document.query_selector("#clone-grid");
    const auto rebuilds = document.performance_stats().document_rebuilds;
    grid.set_attribute("data-populated", "true");
    grid.append_child(fragment);
    ImHTMLTests::RunFrames(document, 3);
    Check(document.performance_stats().document_rebuilds == rebuilds,
          "attribute and clone mutations preserve the mounted document");
    Check(document.query_selector("#clone-grid .cloned-tile").attribute("title") == "Full file name",
          "cloned tile retains its title across frames");
    Check(RenderedTextNodes(ImHTMLTests::FindRaw(document, "#clone-grid")) > 0,
          "cloned text renders inside initially empty flex container");
    grid.replace_children();
    ImHTMLTests::RunFrames(document, 2);
    Check(document.query_selector_all("#clone-grid .cloned-tile").empty(),
          "clearing the browser removes rendered tiles");
    auto replacement = source.content().clone_node(true);
    replacement.query_selector("[data-field=name]").set_text_content("Next folder");
    grid.append_child(replacement);
    ImHTMLTests::RunFrames(document, 2);
    Check(RenderedTextNodes(ImHTMLTests::FindRaw(document, "#clone-grid")) > 0,
          "browser renders again after clearing and repopulating");

    std::printf("\n%s\n", g_failures == 0 ? "all switching tests passed"
                                             : "SWITCHING TESTS FAILED");
    document.shutdown();
    return g_failures == 0 ? 0 : 1;
}
