#pragma once

#include <memory>
#include <span>
#include <string>
#include <unordered_map>

#include "imgui.h"
#include "imhtml.hpp"

namespace lunasvg {
class Document;
}

namespace ImHTML {

class UiImageCache {
public:
    UiImageCache();
    ~UiImageCache();

    void register_image(std::string path, std::span<const unsigned char> bytes);
    void install(Config& config);
    void set_raster_scale(float scale);
    void release();

private:
    struct Entry {
        std::unique_ptr<lunasvg::Document> document;
        std::unique_ptr<ImTextureData> texture;
        ImageMeta meta{0, 0};
        int raster_width = 0;
        int raster_height = 0;
        bool attempted = false;
    };

    Entry* find_or_load(const char* src, const char* baseurl, int display_width = 0, int display_height = 0);
    ImageMeta get_meta(const char* src, const char* baseurl);
    ImTextureID get_texture(const char* src, const char* baseurl, int display_width, int display_height);

    std::unordered_map<std::string, std::span<const unsigned char>> assets_;
    std::unordered_map<std::string, Entry> entries_;
    float raster_scale_ = 1.0f;
};

}  // namespace ImHTML
