// Layout cost breakdown.
//
// Parses documents through the same path the framework uses and times
// litehtml layout in isolation, so the expensive part of a page can be
// attributed to specific markup rather than to the frame loop around it.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include <imhtml/core.hpp>

#include "internal/core_internal.hpp"
#include "litehtml/document.h"
#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace {

std::filesystem::path g_data_root;

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "diag: missing %s\n", path.string().c_str());
        std::exit(1);
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string ReadView(const char* name) { return ReadFile(g_data_root / "views" / name); }

void SetupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImFontConfig font_config;
    ImFont* font = io.Fonts->AddFontFromFileTTF((g_data_root / "Inter.ttf").string().c_str(), 16.0f, &font_config);
    io.FontDefault = font;
    ImHTML::Config config = *ImHTML::GetConfig();
    config.BaseFontSize = 16.0f;
    config.DefaultFont = {font, font, font, font};
    config.FontFamilies["sans-serif"] = config.DefaultFont;
    ImHTML::SetConfig(config);
}

struct TreeStats {
    int elements = 0;
    int max_depth = 0;
    int flex_containers = 0;
    int max_flex_depth = 0;
};

void WalkTree(const std::shared_ptr<litehtml::element>& element, const int depth, const int flex_depth,
              TreeStats& stats) {
    if (!element) return;
    ++stats.elements;
    stats.max_depth = std::max(stats.max_depth, depth);
    const auto display = element->css().get_display();
    const bool flex = display == litehtml::display_flex || display == litehtml::display_inline_flex;
    const int next_flex_depth = flex ? flex_depth + 1 : flex_depth;
    if (flex) {
        ++stats.flex_containers;
        stats.max_flex_depth = std::max(stats.max_flex_depth, next_flex_depth);
    }
    for (const auto& child : element->children()) WalkTree(child, depth + 1, next_flex_depth, stats);
}

double TimeRender(const std::shared_ptr<litehtml::document>& document, const int width) {
    const auto started = std::chrono::steady_clock::now();
    document->render(static_cast<litehtml::pixel_t>(width));
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

std::string Compose(const std::string& body, const bool with_stylesheet) {
    const std::string css = with_stylesheet ? ReadFile(g_data_root / "styles" / "styles.css") : std::string();
    return "<!doctype html><html><head><style>" + css + "</style></head><body>" + body + "</body></html>";
}

void Measure(const char* label, const std::string& body, const bool with_stylesheet = true) {
    static int counter = 0;
    const std::string id = "diag-" + std::to_string(counter++);
    const std::string html = Compose(body, with_stylesheet);

    double parse_ms = 0.0;
    std::vector<double> samples;
    TreeStats stats;
    {
        const auto parse_started = std::chrono::steady_clock::now();
        const auto document = ImHTML::ParseDocument(id.c_str(), html.c_str(), 1600.0f);
        parse_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parse_started).count();
        if (!document) {
            std::printf("%-28s PARSE FAILED\n", label);
            return;
        }

        // First render warms litehtml's internal state; later renders at fresh
        // widths are what a resize actually pays for.
        TimeRender(document, 1600);
        for (int index = 0; index < 5; ++index) samples.push_back(TimeRender(document, 1200 + index * 37));
        std::sort(samples.begin(), samples.end());
        WalkTree(document->root(), 0, 0, stats);
    }

    std::printf("%-28s parse %7.2f ms | render %7.2f ms | elems %5d | depth %3d | flex %4d | flexdepth %3d\n", label,
                parse_ms, samples[samples.size() / 2], stats.elements, stats.max_depth, stats.flex_containers,
                stats.max_flex_depth);
    std::fflush(stdout);
    ImHTML::ResetDocument(id.c_str());
}

// Synthetic nesting probe: how does layout cost grow with nested flex depth?
void MeasureNesting() {
    std::printf("\nnested flex scaling (30 leaf children at the innermost level):\n");
    for (int depth = 1; depth <= 12; ++depth) {
        std::string body;
        for (int level = 0; level < depth; ++level) {
            body += "<div style=\"display:flex;flex-direction:";
            body += (level % 2 == 0) ? "row" : "column";
            body += ";flex:1 1 auto\">";
        }
        for (int leaf = 0; leaf < 30; ++leaf) body += "<span>item</span>";
        for (int level = 0; level < depth; ++level) body += "</div>";

        const std::string id = "nest-" + std::to_string(depth);
        const std::string html = Compose(body, false);
        std::vector<double> samples;
        {
            const auto document = ImHTML::ParseDocument(id.c_str(), html.c_str(), 1600.0f);
            TimeRender(document, 1600);
            for (int index = 0; index < 3; ++index) samples.push_back(TimeRender(document, 1200 + index * 37));
            std::sort(samples.begin(), samples.end());
        }
        std::printf("  depth %2d  render %8.3f ms\n", depth, samples[1]);
        std::fflush(stdout);
        ImHTML::ResetDocument(id.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_data_root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(IMHTML_BENCH_DATA_DIR);
    SetupImGui();

    // The container measures against the current ImGui window, so everything
    // below runs inside one long-lived frame.
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("diag", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

    const std::string shell_open = R"(<div class="window-surface"><div class="app" data-switcher="main">)";
    const std::string shell_close = "</div></div>";
    const std::string sidebar =
        R"(<aside class="sidebar"><div class="sidebar-rail" id="sidebar-nav">)" + ReadView("sidebar.html") +
        "</div></aside>";

    std::printf("layout cost by document content (width sweep, median of 5)\n\n");
    Measure("empty shell", shell_open + R"(<main class="main" id="content-slot"></main>)" + shell_close);
    Measure("shell + sidebar", shell_open + sidebar + R"(<main class="main"></main>)" + shell_close);
    Measure("sidebar only", sidebar);

    for (const char* view : {"maps.html", "settings.html", "plugins.html", "about.html", "qa.html", "provider.html",
                             "versions.html", "outliner.html", "renderer.html"}) {
        Measure(view, shell_open + sidebar + R"(<main class="main">)" + ReadView(view) + "</main>" + shell_close);
    }

    // The settings page as the application actually mounts it: the selected
    // settings panel is nested inside the page's own tab layout.
    {
        std::string settings = ReadView("settings.html");
        const std::string anchor = R"(id="settings-panel-provider")";
        const std::size_t at = settings.find(anchor);
        if (at != std::string::npos) {
            const std::size_t close = settings.find('>', at);
            settings.insert(close + 1, ReadView("provider.html"));
        }
        Measure("settings + provider", shell_open + sidebar + R"(<main class="main">)" + settings + "</main>" +
                                           shell_close);
    }

    // What the running application actually lays out: every fragment mounted
    // into one document at the same time.
    std::string everything;
    for (const char* view : {"maps.html", "settings.html", "plugins.html", "about.html", "qa.html", "provider.html",
                             "encryption.html", "options.html", "versions.html", "auto-texture.html", "outliner.html",
                             "environment.html", "renderer.html", "camera.html", "details.html"}) {
        everything += ReadView(view);
    }
    Measure("all fragments", shell_open + sidebar + R"(<main class="main">)" + everything + "</main>" + shell_close);

    MeasureNesting();

    ImGui::End();
    ImGui::EndFrame();
    return 0;
}
