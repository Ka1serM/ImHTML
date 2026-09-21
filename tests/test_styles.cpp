// Runtime style-matching tests.
//
// The framework shows and hides regions by toggling classes and then asking
// litehtml to restyle. A rule that did not match when the document was parsed
// must start applying once the class is added, and stop applying once it is
// removed.

#include <cstdio>
#include <string>

#include "harness.h"

#include "litehtml/document.h"
#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace {

int g_failures = 0;

const char* kHtml = R"(<!doctype html><html><head><style>
.panel { display: block; height: 40px; }
.panel.is-hidden { display: none; }
.slot > .panel.is-off { display: none; }
</style></head><body>
<div class="slot">
  <div id="a" class="panel">a</div>
  <div id="b" class="panel">b</div>
</div>
</body></html>)";

bool IsDisplayed(const std::shared_ptr<litehtml::document>& document, const char* id) {
    const auto element = document->root()->select_one(std::string("#") + id);
    return element && element->css().get_display() != litehtml::display_none;
}

void Expect(const bool condition, const std::string& message) {
    if (condition) {
        std::printf("ok   %s\n", message.c_str());
    } else {
        std::printf("FAIL %s\n", message.c_str());
        ++g_failures;
    }
    std::fflush(stdout);
}

void CheckToggle(const char* class_name, const char* label) {
    const auto document = ImHTML::ParseDocument("styles", kHtml, 800.0f);
    document->render(800);
    Expect(IsDisplayed(document, "a"), std::string(label) + ": visible before the class is added");

    document->root()->select_one("#a")->set_class(class_name, true);
    document->refresh_styles();
    document->render(800);
    Expect(!IsDisplayed(document, "a"), std::string(label) + ": hidden after the class is added");

    document->root()->select_one("#a")->set_class(class_name, false);
    document->refresh_styles();
    document->render(800);
    Expect(IsDisplayed(document, "a"), std::string(label) + ": visible again after the class is removed");
}

void CheckSameWidthAppend() {
    constexpr const char* html = R"(<!doctype html><html><head><style>
html, body { width: 100%; height: 100%; }
#shell { display: flex; width: 100%; height: 600px; }
#lazy-panel { display: flex; flex: 1 1 0; flex-direction: column; min-width: 0; }
</style></head><body>
<div id="shell"><div id="lazy-panel"></div></div>
</body></html>)";
    const auto document = ImHTML::ParseDocument("same-width-append", html, 800.0f);
    document->render(800);
    const auto panel = document->root()->select_one("#lazy-panel");
    document->append_children_from_string(
        *panel, "<span id=lazy-label>Renderer controls</span>", false);

    // This is intentionally the same width as the cached initial layout.
    document->render(800);
    const auto label = document->root()->select_one("#lazy-label");
    const auto render = label ? label->get_render_item() : nullptr;
    bool painted_box = false;
    if (render) {
        render->get_rendering_boxes([&](const litehtml::position& box) {
            painted_box = painted_box ||
                          (box.width > litehtml::pixel_t(0) && box.height > litehtml::pixel_t(0));
        });
    }
    Expect(painted_box, "same-width lazy append invalidates the cached subtree");
    ImHTML::ResetDocument("same-width-append");
}

}  // namespace

int main(int argc, char** argv) {
    ImHTMLTests::DataRoot() = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(IMHTML_BENCH_DATA_DIR);
    ImHTMLTests::SetupHeadlessImGui();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("styles", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

    CheckToggle("is-hidden", "compound class rule");
    ImHTML::ResetDocument("styles");
    CheckToggle("is-off", "descendant compound class rule");
    ImHTML::ResetDocument("styles");
    CheckSameWidthAppend();

    ImGui::End();
    ImGui::EndFrame();
    std::printf("\n%s\n", g_failures == 0 ? "all style tests passed" : "STYLE TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
