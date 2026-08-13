#include "ui_image_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include "lunasvg.h"

namespace ImHTML {
namespace {

std::string strip_url_suffix(std::string_view url) {
    const std::size_t suffix = url.find_first_of("?#");
    return std::string(url.substr(0, suffix));
}

std::pair<int, int> requested_raster_size(std::string_view url) {
    const std::size_t marker = url.find("size=");
    if (marker == std::string_view::npos) return {0, 0};

    const std::size_t start = marker + 5;
    const std::size_t separator = url.find('x', start);
    if (separator == std::string_view::npos) return {0, 0};
    const std::size_t end = url.find_first_of("&#", separator + 1);
    try {
        const int width = std::stoi(std::string(url.substr(start, separator - start)));
        const int height = std::stoi(std::string(url.substr(separator + 1, end - separator - 1)));
        return {std::max(1, width), std::max(1, height)};
    } catch (...) {
        return {0, 0};
    }
}

}  // namespace

UiImageCache::UiImageCache() = default;
UiImageCache::~UiImageCache() = default;

void UiImageCache::register_image(std::string path, std::span<const unsigned char> bytes) {
    assets_[std::move(path)] = bytes;
}

void UiImageCache::set_raster_scale(float scale) {
    const float next_scale = std::max(1.0f, scale);
    if (std::abs(next_scale - raster_scale_) < 0.01f) return;
    for (auto& [_, entry] : entries_) {
        if (entry.texture) ImGui::UnregisterUserTexture(entry.texture.get());
    }
    entries_.clear();
    raster_scale_ = next_scale;
}

void UiImageCache::install(Config& config) {
    config.LoadImage = [this](const char* src, const char* baseurl) {
        (void)find_or_load(src, baseurl);
    };
    config.GetImageMeta = [this](const char* src, const char* baseurl) {
        return get_meta(src, baseurl);
    };
    config.GetImageTexture = [this](const char* src, const char* baseurl, int display_width, int display_height) {
        return get_texture(src, baseurl, display_width, display_height);
    };
}

UiImageCache::Entry* UiImageCache::find_or_load(const char* src, const char* baseurl,
                                                int display_width, int display_height) {
    (void)baseurl;
    const std::string source = src == nullptr ? std::string() : std::string(src);
    const std::string url = strip_url_suffix(source);
    if (url.empty() || url.starts_with("data:") || url.starts_with("http://") || url.starts_with("https://")) {
        return nullptr;
    }
    const auto asset = assets_.find(url);
    if (asset == assets_.end() || asset->second.empty()) return nullptr;

    const auto [requested_width, requested_height] = requested_raster_size(source);
    const int raster_width = requested_width > 0
                                 ? requested_width
                                 : display_width > 0 ? std::max(1, static_cast<int>(std::lround(display_width * raster_scale_)))
                                                     : 0;
    const int raster_height = requested_height > 0
                                  ? requested_height
                                  : display_height > 0 ? std::max(1, static_cast<int>(std::lround(display_height * raster_scale_)))
                                                      : 0;
    std::string key = url;
    if (raster_width > 0 && raster_height > 0) {
        key += "#raster=" + std::to_string(raster_width) + "x" + std::to_string(raster_height);
    }

    auto [it, inserted] = entries_.try_emplace(std::move(key));
    Entry& entry = it->second;
    if (inserted || !entry.attempted) {
        entry.attempted = true;
        entry.raster_width = raster_width;
        entry.raster_height = raster_height;
        entry.document = lunasvg::Document::loadFromData(reinterpret_cast<const char*>(asset->second.data()),
                                                          asset->second.size());
        if (entry.document) {
            entry.meta.Width = std::max(1, static_cast<int>(std::lround(entry.document->width())));
            entry.meta.Height = std::max(1, static_cast<int>(std::lround(entry.document->height())));
        }
    }
    return entry.document ? &entry : nullptr;
}

ImageMeta UiImageCache::get_meta(const char* src, const char* baseurl) {
    const Entry* entry = find_or_load(src, baseurl);
    return entry != nullptr ? entry->meta : ImageMeta{0, 0};
}

ImTextureID UiImageCache::get_texture(const char* src, const char* baseurl,
                                      int display_width, int display_height) {
    Entry* entry = find_or_load(src, baseurl, display_width, display_height);
    if (entry == nullptr) {
        return ImTextureID_Invalid;
    }
    if (entry->texture) {
        return entry->texture->GetTexID();
    }

    const int width = entry->raster_width > 0
                          ? entry->raster_width
                          : std::max(1, static_cast<int>(std::lround(entry->meta.Width * raster_scale_)));
    const int height = entry->raster_height > 0
                           ? entry->raster_height
                           : std::max(1, static_cast<int>(std::lround(entry->meta.Height * raster_scale_)));
    lunasvg::Bitmap bitmap = entry->document->renderToBitmap(width, height);
    if (bitmap.isNull()) {
        return ImTextureID_Invalid;
    }
    bitmap.convertToRGBA();

    // LunaSVG renders into premultiplied ARGB32. ImGui's Vulkan backend uses
    // straight-alpha blending, so un-premultiply anti-aliased edge pixels or
    // they retain the transparent black background as a dark fringe.
    for (int y = 0; y < bitmap.height(); ++y) {
        unsigned char* row = bitmap.data() + y * bitmap.stride();
        for (int x = 0; x < bitmap.width(); ++x) {
            unsigned char* pixel = row + x * 4;
            const unsigned int alpha = pixel[3];
            if (alpha == 0) {
                pixel[0] = pixel[1] = pixel[2] = 0;
            } else if (alpha < 255) {
                pixel[0] = static_cast<unsigned char>(std::min(255u, (pixel[0] * 255u + alpha / 2u) / alpha));
                pixel[1] = static_cast<unsigned char>(std::min(255u, (pixel[1] * 255u + alpha / 2u) / alpha));
                pixel[2] = static_cast<unsigned char>(std::min(255u, (pixel[2] * 255u + alpha / 2u) / alpha));
            }
        }
    }

    auto texture = std::make_unique<ImTextureData>();
    texture->Create(ImTextureFormat_RGBA32, bitmap.width(), bitmap.height());
    for (int y = 0; y < bitmap.height(); ++y) {
        std::memcpy(texture->Pixels + y * texture->GetPitch(), bitmap.data() + y * bitmap.stride(),
                    static_cast<std::size_t>(bitmap.width()) * 4);
    }
    texture->UseColors = true;
    ImGui::RegisterUserTexture(texture.get());
    entry->texture = std::move(texture);
    return entry->texture->GetTexID();
}

void UiImageCache::release() {
    for (auto& [_, entry] : entries_) {
        if (entry.texture) {
            ImGui::UnregisterUserTexture(entry.texture.get());
        }
    }
    entries_.clear();
}

}  // namespace ImHTML
