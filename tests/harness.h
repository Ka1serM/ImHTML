#pragma once

// Shared headless setup: a live ImGui context with no backend, plus the sample
// application document assembled from tests/data.

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

#include "imgui.h"

#include <imhtml/core.hpp>
#include <imhtml/document.hpp>

#include "internal/core_internal.hpp"

namespace ImHTMLTests {

// The suites are white-box: they inspect litehtml render items and computed
// styles, which the public Element handle deliberately does not expose.
inline std::shared_ptr<litehtml::element> FindRaw(const ImHTML::HtmlDocument& document,
                                                 const std::string& selector) {
    return ImHTML::RawElement(document.query_selector(selector));
}


inline std::filesystem::path& DataRoot() {
    static std::filesystem::path root;
    return root;
}

inline std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "harness: missing data file %s\n", path.string().c_str());
        std::exit(1);
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

inline std::string ReadView(const char* name) { return ReadFile(DataRoot() / "views" / name); }

// No renderer backend is attached: ImGuiBackendFlags_RendererHasTextures lets
// the font atlas own its texture, and recorded draw data is simply dropped.
inline void SetupHeadlessImGui(const float width = 1600.0f, const float height = 900.0f) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImFontConfig font_config;
    font_config.RasterizerDensity = 1.0f;
    const std::string font_path = (DataRoot() / "Inter.ttf").string();
    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0f, &font_config);
    if (font == nullptr) {
        std::fprintf(stderr, "harness: failed to load %s\n", font_path.c_str());
        std::exit(1);
    }
    io.FontDefault = font;

    ImHTML::Config config = *ImHTML::GetConfig();
    config.BaseFontSize = 16.0f;
    config.DefaultFont = {font, font, font, font};
    config.FontFamilies["sans-serif"] = config.DefaultFont;
    ImHTML::SetConfig(config);
}

inline void RegisterTemplateElement(const char* tag, const char* file) {
    // Referenced by the callback below, so the storage has to stay put.
    static std::deque<std::string> sources;
    const std::string& source = sources.emplace_back(ReadFile(DataRoot() / "templates" / file));
    ImHTML::PrepareHtmlTemplate(source);
    ImHTML::RegisterCustomElementHtml(
        tag, [&source](const std::map<std::string, std::string>& attributes, std::string_view children,
                       const ImHTML::HtmlElementContext&) {
            return ImHTML::ExpandHtmlTemplate(source, attributes, children);
        });
}

inline void BuildSampleDocument(ImHTML::HtmlDocument& document) {
    document.register_shell(ReadView("app.html"));
    document.set_stylesheet_provider([] { return ReadFile(DataRoot() / "styles" / "styles.css"); });

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

inline void RunFrame(ImHTML::HtmlDocument& document) {
    ImGui::NewFrame();
    document.frame();
    ImGui::Render();
}

inline void RunFrames(ImHTML::HtmlDocument& document, const int count) {
    for (int index = 0; index < count; ++index) RunFrame(document);
}

}  // namespace ImHTMLTests
