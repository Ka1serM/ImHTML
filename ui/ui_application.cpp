#include "imhtml_ui.h"

#include <fstream>
#include <iterator>
#include <string>

#include <SDL3/SDL.h>
#include "imgui.h"
#include "imhtml.hpp"
#include "litehtml/element.h"
#include "ui/ui_image_cache.h"
#include "ui/ui_theme.h"

namespace ImHTML {

UiApplication::UiApplication() : image_cache_(std::make_unique<UiImageCache>()) {}

UiApplication::~UiApplication() = default;

void UiApplication::register_image(std::string path, std::span<const unsigned char> bytes) {
    image_cache_->register_image(std::move(path), bytes);
}

float UiApplication::base_font_size() const {
    return ImHTML::GetConfig()->BaseFontSize;
}

void UiApplication::release_image_resources() {
    image_cache_->release();
}

void UiApplication::register_stylesheet(StylesheetProvider provider) {
    stylesheets_.push_back([provider = std::move(provider)] {
        const std::string css = provider ? provider() : std::string();
        Theme::initialize_from_css(css);
        return css;
    });
}

void UiApplication::register_stylesheet_file(std::string path) {
    register_stylesheet([path = std::move(path)] {
        std::ifstream file(path);
        if (!file) return std::string();
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    });
}

std::string UiApplication::stylesheet() const {
    std::string result;
    for (const StylesheetProvider& provider : stylesheets_) {
        if (provider) result += provider() + "\n";
    }
    for (const StylesheetProvider& provider : embedded_stylesheets_.providers()) {
        if (provider) {
            const std::string css = provider();
            Theme::initialize_from_css(css);
            result += css + "\n";
        }
    }
    return result;
}

void UiApplication::set_animation(std::unique_ptr<IUiAnimation> animation) {
    animation_ = std::move(animation);
}

bool UiApplication::initialize(SDL_Window* window, const std::string& font_path) {
    window_ = window;
    const float device_scale = SDL_GetWindowDisplayScale(window_);
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig font_config;
    font_config.RasterizerDensity = device_scale > 0.0f ? device_scale : 1.0f;
    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0f, &font_config);
    if (font) {
        io.FontDefault = font;
        ImHTML::Config config = *ImHTML::GetConfig();
        config.BaseFontSize = 16.0f;
        config.DefaultFont = {font, font, font, font};
        config.FontFamilies["sans-serif"] = config.DefaultFont;
        ImHTML::SetConfig(config);
    }
    return finish_initialize();
}

bool UiApplication::initialize(SDL_Window* window, std::span<const unsigned char> font_data) {
    window_ = window;
    const float device_scale = SDL_GetWindowDisplayScale(window_);
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig font_config;
    font_config.RasterizerDensity = device_scale > 0.0f ? device_scale : 1.0f;
    // The embedded font has static storage and must not be freed by ImGui.
    font_config.FontDataOwnedByAtlas = false;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(font_data.data()), static_cast<int>(font_data.size()),
        16.0f, &font_config);
    if (font) {
        io.FontDefault = font;
        ImHTML::Config config = *ImHTML::GetConfig();
        config.BaseFontSize = 16.0f;
        config.DefaultFont = {font, font, font, font};
        config.FontFamilies["sans-serif"] = config.DefaultFont;
        ImHTML::SetConfig(config);
    }
    return finish_initialize();
}

bool UiApplication::finish_initialize() {
    ImHTML::Config config = *ImHTML::GetConfig();
    image_cache_->install(config);
    ImHTML::SetConfig(config);
    image_cache_->set_raster_scale(ImGui::GetIO().DisplayFramebufferScale.x);
    html_application_.set_stylesheet_provider([this] { return stylesheet(); });
    fragments_.register_ui(*this);
    components_.install(*this);
    if (!animation_) animation_ = std::make_unique<ImAnimUiAnimation>();
    RegisterElementLayerTransform([this](const std::shared_ptr<litehtml::element>& element, const ImRect& bounds) {
        return animation_ ? animation_->transform(element, bounds) : ImHTML::ElementLayerTransform{};
    });
    html_application_.before_render = [this] {
        components_.before_render(*this);
        if (animation_) animation_->begin_frame(ImGui::GetIO().DeltaTime);
    };
    html_application_.after_render = [this] {
        fragments_.draw(html_application_.current_fragment(), *this);
    };
    initialized_ = html_application_.initialize(this);
    return initialized_;
}

void UiApplication::frame() {
    if (!initialized_) return;
    image_cache_->set_raster_scale(ImGui::GetIO().DisplayFramebufferScale.x);
    fragments_.tick();
    html_application_.frame();
    image_cache_->set_raster_scale(ImGui::GetIO().DisplayFramebufferScale.x);
}

void UiApplication::shutdown() {
    fragments_.flush();
    if (!initialized_) return;
    html_application_.shutdown();
    UnregisterElementLayerTransform();
    components_.unregister_all();
    initialized_ = false;
}

}  // namespace ImHTML
