// Headless performance harness for ImHTML.
//
// Drives ImHTML::HtmlDocument through a real ImGui frame loop without any
// window, renderer, or GPU. The document, stylesheet, and fragments under
// tests/data mirror a production desktop UI, so the numbers reported here
// track the cost the real application pays.
//
// Scenarios:
//   steady   - fixed viewport, repeated frames (paint-dominated)
//   resize   - viewport width changes every frame (layout-dominated)
//   height   - only the viewport height changes every frame
//   switch   - fragment switches (rebuild-dominated)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include <imhtml/core.hpp>
#include <imhtml/document.hpp>

namespace {

std::filesystem::path g_data_root;

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "bench: missing data file %s\n", path.string().c_str());
        std::exit(1);
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string ReadView(const char* name) { return ReadFile(g_data_root / "views" / name); }

struct FrameSample {
    double total_ms = 0.0;
    double layout_ms = 0.0;
    double paint_ms = 0.0;
    int draw_commands = 0;
    int vertices = 0;
    int indices = 0;
};

struct Stats {
    double mean = 0.0;
    double median = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};

Stats Summarize(std::vector<double> values) {
    Stats stats;
    if (values.empty()) return stats;
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const double value : values) sum += value;
    stats.mean = sum / static_cast<double>(values.size());
    stats.median = values[values.size() / 2];
    stats.p95 = values[static_cast<std::size_t>(static_cast<double>(values.size() - 1) * 0.95)];
    stats.max = values.back();
    return stats;
}

// ImGui needs a live context to measure text and record draw commands. No
// backend is attached: ImGuiBackendFlags_RendererHasTextures makes the atlas
// own its own texture lifetime, and the recorded draw data is simply dropped.
class HeadlessImGui {
public:
    HeadlessImGui() {
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(1600.0f, 900.0f);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        ImFontConfig font_config;
        font_config.RasterizerDensity = 1.0f;
        const std::string font_path = (g_data_root / "Inter.ttf").string();
        ImFont* font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0f, &font_config);
        if (font == nullptr) {
            std::fprintf(stderr, "bench: failed to load %s\n", font_path.c_str());
            std::exit(1);
        }
        io.FontDefault = font;

        ImHTML::Config config = *ImHTML::GetConfig();
        config.BaseFontSize = 16.0f;
        config.DefaultFont = {font, font, font, font};
        config.FontFamilies["sans-serif"] = config.DefaultFont;
        ImHTML::SetConfig(config);
    }

    ~HeadlessImGui() { ImGui::DestroyContext(context_); }

    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    void set_display_size(const float width, const float height) {
        ImGui::GetIO().DisplaySize = ImVec2(width, height);
    }

private:
    ImGuiContext* context_ = nullptr;
};

void RegisterTemplateElement(const char* tag, const char* file) {
    // Held by reference in the callback below, so the storage must be stable.
    static std::deque<std::string> sources;
    const std::string& source = sources.emplace_back(ReadFile(g_data_root / "templates" / file));
    ImHTML::PrepareHtmlTemplate(source);
    ImHTML::RegisterCustomElementHtml(
        tag, [&source](const std::map<std::string, std::string>& attributes, std::string_view children,
                       const ImHTML::HtmlElementContext&) {
            return ImHTML::ExpandHtmlTemplate(source, attributes, children);
        });
}

void BuildDocument(ImHTML::HtmlDocument& document) {
    document.register_shell(ReadView("app.html"));
    document.set_stylesheet_provider([] { return ReadFile(g_data_root / "styles" / "styles.css"); });

    static const struct {
        const char* id;
        const char* file;
    } fragments[] = {
        {"sidebar", "sidebar.html"},
        {"maps", "maps.html"},
        {"settings", "settings.html"},
        {"qa", "qa.html"},
        {"plugins", "plugins.html"},
        {"about", "about.html"},
        {"maps-panel-outliner", "outliner.html"},
        {"maps-panel-environment", "environment.html"},
        {"maps-panel-renderer", "renderer.html"},
        {"maps-panel-camera", "camera.html"},
        {"maps-panel-details", "details.html"},
        {"settings-panel-provider", "provider.html"},
        {"settings-panel-encryption", "encryption.html"},
        {"settings-panel-options", "options.html"},
        {"settings-panel-versions", "versions.html"},
        {"settings-panel-auto-texture", "auto-texture.html"},
    };
    for (const auto& fragment : fragments) {
        const char* file = fragment.file;
        document.register_fragment(fragment.id, [file] { return ReadView(file); });
    }

    RegisterTemplateElement("combo-box", "combo_box.html");
    RegisterTemplateElement("img-button-square", "img_button_square.html");
    RegisterTemplateElement("settings-box", "settings_box.html");

    document.initialize(nullptr);
}

// One complete application frame: ImGui frame boundaries plus the document.
FrameSample RunFrame(ImHTML::HtmlDocument& document) {
    const auto started = std::chrono::steady_clock::now();
    ImGui::NewFrame();
    document.frame();
    ImGui::Render();
    const auto finished = std::chrono::steady_clock::now();

    FrameSample sample;
    sample.total_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    const auto& stats = document.performance_stats();
    sample.layout_ms = stats.last_layout_ms;
    sample.paint_ms = stats.last_paint_ms;

    // What the renderer would actually submit: one ImDrawCmd is one draw call.
    if (const ImDrawData* draw_data = ImGui::GetDrawData(); draw_data != nullptr) {
        sample.vertices = draw_data->TotalVtxCount;
        sample.indices = draw_data->TotalIdxCount;
        for (int list = 0; list < draw_data->CmdListsCount; ++list) {
            sample.draw_commands += draw_data->CmdLists[list]->CmdBuffer.Size;
        }
    }
    return sample;
}

struct ScenarioResult {
    std::string name;
    Stats total;
    Stats layout;
    Stats paint;
    std::uint64_t layouts = 0;
    std::uint64_t rebuilds = 0;
    std::uint64_t selector_scans = 0;
    std::size_t elements = 0;
    int draw_commands = 0;
    int vertices = 0;
};

ScenarioResult RunScenario(const char* name, HeadlessImGui& imgui, ImHTML::HtmlDocument& document,
                           const int frames, const std::function<void(int)>& before_frame) {
    // Warm up: first frames build the document, bake glyphs, and populate caches.
    for (int index = 0; index < 5; ++index) {
        before_frame(index);
        RunFrame(document);
    }
    document.reset_performance_stats();

    int last_draw_commands = 0;
    int last_vertices = 0;
    std::vector<double> totals;
    std::vector<double> layouts;
    std::vector<double> paints;
    totals.reserve(frames);
    layouts.reserve(frames);
    paints.reserve(frames);
    for (int index = 0; index < frames; ++index) {
        before_frame(index);
        const FrameSample sample = RunFrame(document);
        if (std::getenv("IMHTML_TRACE_PANEL") != nullptr && name == std::string("panel")) {
            std::printf("  frame %3d total %8.3f layout %8.3f paint %8.3f\n", index,
                        sample.total_ms, sample.layout_ms, sample.paint_ms);
        }
        last_draw_commands = sample.draw_commands;
        last_vertices = sample.vertices;
        totals.push_back(sample.total_ms);
        layouts.push_back(sample.layout_ms);
        paints.push_back(sample.paint_ms);
    }

    const auto& stats = document.performance_stats();
    return ScenarioResult{
        .name = name,
        .total = Summarize(std::move(totals)),
        .layout = Summarize(std::move(layouts)),
        .paint = Summarize(std::move(paints)),
        .layouts = stats.layout_count,
        .rebuilds = stats.document_rebuilds,
        .selector_scans = stats.selector_scans,
        .elements = document.query_selector_all("*").size(),
        .draw_commands = last_draw_commands,
        .vertices = last_vertices,
    };
}

void PrintHeader() {
    std::printf("%-10s %9s %9s %9s %9s %9s %9s %8s %8s %8s %8s\n", "scenario", "mean-ms", "median", "p95", "max",
                "layout", "paint", "layouts", "rebuild", "selects", "elems");
    std::printf("%s\n", std::string(127, '-').c_str());
}

void PrintResult(const ScenarioResult& result) {
    std::printf("%-10s %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %8llu %8llu %8llu %8zu %8d %8d\n", result.name.c_str(),
                result.total.mean, result.total.median, result.total.p95, result.total.max, result.layout.mean,
                result.paint.mean, static_cast<unsigned long long>(result.layouts),
                static_cast<unsigned long long>(result.rebuilds),
                static_cast<unsigned long long>(result.selector_scans), result.elements);
}

}  // namespace

int main(int argc, char** argv) {
    g_data_root = std::filesystem::path(IMHTML_BENCH_DATA_DIR);
    int frames = 100;
    std::string only;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--frames" && index + 1 < argc) {
            frames = std::atoi(argv[++index]);
        } else if (argument == "--scenario" && index + 1 < argc) {
            only = argv[++index];
        } else if (argument == "--data" && index + 1 < argc) {
            g_data_root = std::filesystem::path(argv[++index]);
        }
    }

    HeadlessImGui imgui;
    ImHTML::HtmlDocument document;
    BuildDocument(document);

    PrintHeader();
    const auto run = [&](const char* name, const std::function<void(int)>& before) {
        if (!only.empty() && only != name) return;
        PrintResult(RunScenario(name, imgui, document, frames, before));
        std::fflush(stdout);
    };

    run("steady", [&](int) { imgui.set_display_size(1600.0f, 900.0f); });

    // A drag-resize sweeps the width one pixel at a time; every frame is a
    // fresh layout at a width litehtml has never seen.
    run("resize", [&](const int index) {
        const float width = 1100.0f + static_cast<float>(index % 500);
        imgui.set_display_size(width, 900.0f);
    });

    run("height", [&](const int index) {
        const float height = 700.0f + static_cast<float>(index % 300);
        imgui.set_display_size(1600.0f, height);
    });

    // Nested panel switches are the renderer/environment/settings interaction
    // path. The panels remain mounted after the first frame, so subsequent
    // switches should not rebuild the document.
    run("panel", [&](const int index) {
        imgui.set_display_size(1600.0f, 900.0f);
        static const char* const panels[] = {
            "maps-panel-outliner", "maps-panel-environment", "maps-panel-renderer",
            "maps-panel-camera", "maps-panel-details"};
        document.select("main", "maps");
        document.select("maps", panels[index % 5]);
    });

    // This mirrors the two live splitter style writes without involving the
    // application or renderer. It isolates UI invalidation/reflow cost from
    // window resizing and GPU work.
    run("splitter", [&](const int index) {
        imgui.set_display_size(1600.0f, 900.0f);
        document.select("main", "maps");
        document.set_flex_pixels("#maps-right-panel", 180 + (index % 360));
        document.set_flex_pixels("#maps-drawer", 160 + ((index * 3) % 320));
    });

    static const bool trace_switch = std::getenv("IMHTML_TRACE_SWITCH") != nullptr;
    run("switch", [&](const int index) {
        imgui.set_display_size(1600.0f, 900.0f);
        static const char* const pages[] = {"maps", "settings", "plugins", "about"};
        if (trace_switch && index > 0) {
            std::printf("  after %-10s layout %8.2f ms\n", pages[(index - 1) % 4],
                        document.performance_stats().last_layout_ms);
        }
        document.select("main", pages[index % 4]);
    });

    document.shutdown();
    return 0;
}
