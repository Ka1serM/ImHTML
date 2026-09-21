#include <imhtml/app.hpp>

#include <fstream>
#include <iterator>
#include <string>

#include "imgui.h"
#include <imhtml/core.hpp>
#include "litehtml/element.h"
#include "app/image_cache.hpp"
#include <imhtml/theme.hpp>

namespace ImHTML {

HtmlWindow::HtmlWindow() : image_cache_(std::make_unique<UiImageCache>()) {}

HtmlWindow::~HtmlWindow() = default;

void HtmlWindow::register_image(std::string path, std::span<const unsigned char> bytes) {
    image_cache_->register_image(std::move(path), bytes);
}

float HtmlWindow::base_font_size() const {
    return ImHTML::GetConfig()->BaseFontSize;
}

void HtmlWindow::release_image_resources() {
    image_cache_->release();
}

void HtmlWindow::register_stylesheet(StylesheetProvider provider) {
    stylesheets_.push_back([provider = std::move(provider)] {
        const std::string css = provider ? provider() : std::string();
        Theme::initialize_from_css(css);
        return css;
    });
}

void HtmlWindow::register_stylesheet_file(std::string path) {
    register_stylesheet([path = std::move(path)] {
        std::ifstream file(path);
        if (!file) return std::string();
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    });
}

std::string HtmlWindow::stylesheet() const {
    std::string result;
    for (const StylesheetProvider& provider : stylesheets_) {
        if (provider) result += provider() + "\n";
    }
    for (const auto& stylesheet : embedded_stylesheets_) {
        if (stylesheet) {
            const std::string css = stylesheet->css();
            Theme::initialize_from_css(css);
            result += css + "\n";
        }
    }
    return result;
}

namespace {

// The shell rasterizes fonts at the framebuffer's density so text stays crisp
// on a scaled display. When the caller does not pass a scale, take the one the
// ImGui backend already reported rather than asking a windowing system for it.
float ResolveDisplayScale(const float requested) {
    if (requested > 0.0f) return requested;
    const float reported = ImGui::GetIO().DisplayFramebufferScale.x;
    return reported > 0.0f ? reported : 1.0f;
}

void AdoptFont(ImFont* font) {
    if (font == nullptr) return;
    ImGui::GetIO().FontDefault = font;
    ImHTML::Config config = *ImHTML::GetConfig();
    config.BaseFontSize = 16.0f;
    config.DefaultFont = {font, font, font, font};
    config.FontFamilies["sans-serif"] = config.DefaultFont;
    ImHTML::SetConfig(config);
}

}  // namespace

bool HtmlWindow::initialize(const std::string& font_path, const float display_scale) {
    ImFontConfig font_config;
    font_config.RasterizerDensity = ResolveDisplayScale(display_scale);
    AdoptFont(ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0f, &font_config));
    return finish_initialize();
}

bool HtmlWindow::initialize(std::span<const unsigned char> font_data, const float display_scale) {
    ImFontConfig font_config;
    font_config.RasterizerDensity = ResolveDisplayScale(display_scale);
    font_config.FontDataOwnedByAtlas = false;
    AdoptFont(ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(font_data.data()), static_cast<int>(font_data.size()), 16.0f,
        &font_config));
    return finish_initialize();
}

bool HtmlWindow::finish_initialize() {
    ImHTML::Config config = *ImHTML::GetConfig();
    image_cache_->install(config);
    ImHTML::SetConfig(config);
    image_cache_->set_raster_scale(ImGui::GetIO().DisplayFramebufferScale.x);
    document_.set_stylesheet_provider([this] { return stylesheet(); });
    fragments_.register_ui(*this);
    components_.install(*this);
    document_.before_render = [this] {
        components_.before_render(*this);
    };
    document_.after_render = [this] {
        fragments_.draw(document_.current_fragment(), *this);
    };
    initialized_ = document_.initialize(this);
    return initialized_;
}

void HtmlWindow::frame() {
    if (!initialized_) return;
    image_cache_->set_raster_scale(ImGui::GetIO().DisplayFramebufferScale.x);
    fragments_.tick();
    document_.frame();
    image_cache_->set_raster_scale(ImGui::GetIO().DisplayFramebufferScale.x);
}

void HtmlWindow::shutdown() {
    fragments_.flush();
    if (!initialized_) return;
    document_.shutdown();
    components_.unregister_all();
    initialized_ = false;
}

}  // namespace ImHTML
