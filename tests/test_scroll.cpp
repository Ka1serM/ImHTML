// Scrolling regression tests.
//
// The tab panels on the maps page (Environment, Renderer, Camera) are
// `overflow: auto` regions whose content is taller than the panel. They must
// report a scrollable range and must actually move when the wheel is used.

#include <cstdio>
#include <string>

#include "harness.h"

#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace {

int g_failures = 0;

void Fail(const std::string& message) {
    std::printf("FAIL %s\n", message.c_str());
    std::fflush(stdout);
    ++g_failures;
}

void Pass(const std::string& message) {
    std::printf("ok   %s\n", message.c_str());
    std::fflush(stdout);
}

// Sends a wheel notch to the document. ImGui reads the wheel from the IO state
// of the frame being built, so it has to be set between frames.
void ScrollWheel(ImHTML::HtmlDocument& document, const float notches) {
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(io.DisplaySize.x - 120.0f, 150.0f);
    io.MouseWheel = notches;
    ImHTMLTests::RunFrame(document);
    io.MouseWheel = 0.0f;
}

void CheckPanelScrolls(ImHTML::HtmlDocument& document, const char* panel) {
    document.select("main", "maps");
    ImHTMLTests::RunFrames(document, 3);
    document.select("maps", panel);
    ImHTMLTests::RunFrames(document, 3);

    const auto metrics = document.scroll_metrics(panel);
    if (!metrics.valid) {
        Fail(std::string(panel) + ": not reported as a scroll container");
        return;
    }
    if (metrics.viewport_height <= 0.0f) {
        Fail(std::string(panel) + ": viewport height is 0");
        return;
    }
    if (metrics.content_height <= metrics.viewport_height) {
        Fail(std::string(panel) + ": content " + std::to_string(metrics.content_height) + " fits viewport " +
             std::to_string(metrics.viewport_height) + ", nothing to scroll");
        return;
    }
    if (metrics.max_top <= 0.0f) {
        Fail(std::string(panel) + ": scrollable range is 0 (content " + std::to_string(metrics.content_height) +
             " > viewport " + std::to_string(metrics.viewport_height) + ")");
        return;
    }

    const float before = document.scroll_metrics(panel).top;
    ScrollWheel(document, -3.0f);
    const float after = document.scroll_metrics(panel).top;
    if (after <= before) {
        Fail(std::string(panel) + ": wheel did not move the panel (top stayed at " + std::to_string(after) + ")");
        return;
    }

    Pass(std::string(panel) + ": viewport " + std::to_string(static_cast<int>(metrics.viewport_height)) +
         "px, content " + std::to_string(static_cast<int>(metrics.content_height)) + "px, scrolled to " +
         std::to_string(static_cast<int>(after)));
}

}  // namespace

int main(int argc, char** argv) {
    ImHTMLTests::DataRoot() = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(IMHTML_BENCH_DATA_DIR);

    // A short window guarantees the panels overflow.
    ImHTMLTests::SetupHeadlessImGui(1400.0f, 600.0f);
    ImHTML::HtmlDocument document;
    ImHTMLTests::BuildSampleDocument(document);
    ImHTMLTests::RunFrames(document, 5);

    for (const char* panel : {"maps-panel-environment", "maps-panel-renderer", "maps-panel-camera"}) {
        CheckPanelScrolls(document, panel);
    }

    std::printf("\n%s\n", g_failures == 0 ? "all scroll tests passed" : "SCROLL TESTS FAILED");
    document.shutdown();
    return g_failures == 0 ? 0 : 1;
}
