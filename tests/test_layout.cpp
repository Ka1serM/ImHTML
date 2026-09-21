// Layout correctness tests.
//
// Layout reuse is only safe if a document that has been rendered many times
// ends up geometrically identical to a document rendered once at the same
// width. These tests render every real view through both paths and compare the
// resulting box tree element by element.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"

#include <imhtml/core.hpp>

#include "internal/core_internal.hpp"
#include "litehtml/document.h"
#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace {

std::filesystem::path g_data_root;
int g_failures = 0;

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "test: missing %s\n", path.string().c_str());
        std::exit(1);
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

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

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
}

struct Box {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

void CollectBoxes(const std::shared_ptr<litehtml::render_item>& item, std::vector<Box>& boxes) {
    if (!item) return;
    const litehtml::position placement = item->get_placement();
    boxes.push_back({static_cast<float>(placement.x), static_cast<float>(placement.y),
                     static_cast<float>(placement.width), static_cast<float>(placement.height)});
    for (const auto& child : item->children()) CollectBoxes(child, boxes);
}

std::string Compose(const std::string& body) {
    return "<!doctype html><html><head><style>" + ReadFile(g_data_root / "styles" / "styles.css") +
           "</style></head><body>" + body + "</body></html>";
}

// Renders `html` at `width`, optionally after a sequence of other widths that
// populates every layout cache along the way.
std::vector<Box> LayoutAt(const char* id, const std::string& html, const int width,
                          const std::vector<int>& warmup_widths, const bool reuse) {
    std::vector<Box> boxes;
    litehtml::render_item::g_layout_reuse = reuse;
    {
        const auto document = ImHTML::ParseDocument(id, html.c_str(), static_cast<float>(width));
        if (!document) return boxes;
        for (const int warmup : warmup_widths) document->render(static_cast<litehtml::pixel_t>(warmup));
        document->render(static_cast<litehtml::pixel_t>(width));
        CollectBoxes(document->root_render(), boxes);
    }
    ImHTML::ResetDocument(id);
    litehtml::render_item::g_layout_reuse = true;
    return boxes;
}

void ExpectStableLayout(const char* label, const std::string& body) {
    const std::string html = Compose(body);
    const int width = 1280;

    // Reference layout with memoization off, compared against a document that
    // has been relaid out at many widths with memoization on.
    const std::vector<Box> reference = LayoutAt("layout-reference", html, width, {}, false);
    const std::vector<Box> repeated =
        LayoutAt("layout-repeated", html, width, {900, 1600, 1101, 1280, 1600, 1280, 743, 1280}, true);

    if (reference.empty()) {
        std::printf("FAIL %-24s produced no layout\n", label);
        ++g_failures;
        return;
    }
    if (reference.size() != repeated.size()) {
        std::printf("FAIL %-24s box count %zu != %zu\n", label, reference.size(), repeated.size());
        ++g_failures;
        return;
    }
    for (std::size_t index = 0; index < reference.size(); ++index) {
        const Box& expected = reference[index];
        const Box& actual = repeated[index];
        if (expected.x != actual.x || expected.y != actual.y || expected.width != actual.width ||
            expected.height != actual.height) {
            std::printf("FAIL %-24s box %zu (%.1f,%.1f %.1fx%.1f) != (%.1f,%.1f %.1fx%.1f)\n", label, index,
                        expected.x, expected.y, expected.width, expected.height, actual.x, actual.y, actual.width,
                        actual.height);
            ++g_failures;
            return;
        }
    }
    std::printf("ok   %-24s %zu boxes match unmemoized reference\n", label, reference.size());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    g_data_root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(IMHTML_BENCH_DATA_DIR);
    SetupImGui();

    const std::string shell_open = R"(<div class="window-surface"><div class="app" data-switcher="main">)";
    const std::string shell_close = "</div></div>";
    const std::string sidebar = R"(<aside class="sidebar"><div class="sidebar-rail">)" +
                                ReadFile(g_data_root / "views" / "sidebar.html") + "</div></aside>";

    for (const char* view : {"maps.html", "settings.html", "plugins.html", "about.html", "qa.html", "provider.html",
                             "encryption.html", "options.html", "versions.html", "auto-texture.html", "outliner.html",
                             "environment.html", "renderer.html", "camera.html", "details.html", "sidebar.html"}) {
        ExpectStableLayout(view, shell_open + sidebar + R"(<main class="main">)" +
                                     ReadFile(g_data_root / "views" / view) + "</main>" + shell_close);
    }

    // Deeply nested flex is the shape that layout reuse targets, so it gets an
    // explicit case. The depth is capped because the reference side runs with
    // reuse disabled, which is exponential in exactly this dimension.
    for (const int depth : {3, 5, 7}) {
        std::string body;
        for (int level = 0; level < depth; ++level) {
            body += "<div style=\"display:flex;flex-direction:";
            body += (level % 2 == 0) ? "row" : "column";
            body += ";flex:1 1 auto\">";
        }
        for (int leaf = 0; leaf < 12; ++leaf) body += "<span>item</span>";
        for (int level = 0; level < depth; ++level) body += "</div>";
        ExpectStableLayout(("nested-flex-" + std::to_string(depth)).c_str(), body);
    }

    std::printf("\n%s\n", g_failures == 0 ? "all layout tests passed" : "LAYOUT TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
