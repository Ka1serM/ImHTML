#pragma once

// A tiny software rasterizer for ImDrawData.
//
// Paint bugs do not show up in layout assertions: a clipped antialiasing
// fringe, a rounding cap that flattens a pill, a fill snapped to a grid its
// clip rect was not — every one of those produces correct boxes and wrong
// pixels. So the paint suite rasterizes the draw list the same way a GPU
// backend would (clip rect, barycentric coverage, alpha blend) and asserts on
// coverage, which is stable across platforms in a way image hashes are not.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "imgui.h"

namespace ImHTMLTests {

class Raster {
public:
    Raster(const int width, const int height) : width_(width), height_(height), pixels_(width * height * 4, 0.0f) {}

    int width() const { return width_; }
    int height() const { return height_; }

    // Straight (non-premultiplied) RGBA in 0..1.
    void sample(const int x, const int y, float rgba[4]) const {
        const std::size_t index = (static_cast<std::size_t>(y) * width_ + x) * 4;
        for (int channel = 0; channel < 4; ++channel) rgba[channel] = pixels_[index + channel];
    }

    float alpha(const int x, const int y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0.0f;
        return pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4 + 3];
    }

    // Total coverage of one row / column, which is what symmetry assertions
    // compare. A shape symmetric about its centre must produce a mirrored
    // profile, whatever the antialiasing does at the edges.
    std::vector<float> row_coverage(const int x0, const int y0, const int x1, const int y1) const {
        std::vector<float> rows;
        for (int y = y0; y < y1; ++y) {
            float total = 0.0f;
            for (int x = x0; x < x1; ++x) total += alpha(x, y);
            rows.push_back(total);
        }
        return rows;
    }

    std::vector<float> column_coverage(const int x0, const int y0, const int x1, const int y1) const {
        std::vector<float> columns;
        for (int x = x0; x < x1; ++x) {
            float total = 0.0f;
            for (int y = y0; y < y1; ++y) total += alpha(x, y);
            columns.push_back(total);
        }
        return columns;
    }

    void draw(const ImDrawData& data) {
        for (int list_index = 0; list_index < data.CmdListsCount; ++list_index) {
            const ImDrawList* list = data.CmdLists[list_index];
            for (const ImDrawCmd& command : list->CmdBuffer) {
                if (command.ElemCount == 0 || command.UserCallback != nullptr) continue;
                for (unsigned int element = 0; element + 2 < command.ElemCount; element += 3) {
                    const ImDrawVert& a = vertex(*list, command, element);
                    const ImDrawVert& b = vertex(*list, command, element + 1);
                    const ImDrawVert& c = vertex(*list, command, element + 2);
                    triangle(a, b, c, command.ClipRect);
                }
            }
        }
    }

private:
    static const ImDrawVert& vertex(const ImDrawList& list, const ImDrawCmd& command, const unsigned int element) {
        return list.VtxBuffer[command.VtxOffset + list.IdxBuffer[command.IdxOffset + element]];
    }

    static float edge(const ImVec2& a, const ImVec2& b, const float x, const float y) {
        return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
    }

    void blend(const int x, const int y, const float rgba[4]) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        const std::size_t index = (static_cast<std::size_t>(y) * width_ + x) * 4;
        const float source_alpha = rgba[3];
        const float destination_alpha = pixels_[index + 3];
        const float out_alpha = source_alpha + destination_alpha * (1.0f - source_alpha);
        for (int channel = 0; channel < 3; ++channel) {
            const float source = rgba[channel] * source_alpha;
            const float destination = pixels_[index + channel] * destination_alpha;
            pixels_[index + channel] = out_alpha > 0.0f ? (source + destination * (1.0f - source_alpha)) / out_alpha
                                                        : 0.0f;
        }
        pixels_[index + 3] = out_alpha;
    }

    void triangle(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImVec4& clip) {
        const float area = edge(a.pos, b.pos, c.pos.x, c.pos.y);
        if (std::fabs(area) < 1e-9f) return;

        const int min_x = std::max(static_cast<int>(std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))),
                                   static_cast<int>(std::floor(clip.x)));
        const int min_y = std::max(static_cast<int>(std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))),
                                   static_cast<int>(std::floor(clip.y)));
        const int max_x = std::min(static_cast<int>(std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))),
                                   static_cast<int>(std::ceil(clip.z)));
        const int max_y = std::min(static_cast<int>(std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))),
                                   static_cast<int>(std::ceil(clip.w)));

        for (int y = min_y; y < max_y; ++y) {
            for (int x = min_x; x < max_x; ++x) {
                // 8x8 supersampling: ImGui's antialiasing lives in vertex alpha,
                // and the geometry edges need coverage of their own. The sample
                // count sets the quantization of partial pixels, which is the
                // noise floor for the coverage comparisons in the paint suite.
                float accumulated[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                int hits = 0;
                for (int sub_y = 0; sub_y < 8; ++sub_y) {
                    for (int sub_x = 0; sub_x < 8; ++sub_x) {
                        const float sample_x = static_cast<float>(x) + (sub_x + 0.5f) / 8.0f;
                        const float sample_y = static_cast<float>(y) + (sub_y + 0.5f) / 8.0f;
                        if (sample_x < clip.x || sample_y < clip.y || sample_x >= clip.z || sample_y >= clip.w) {
                            continue;
                        }
                        const float weight_a = edge(b.pos, c.pos, sample_x, sample_y) / area;
                        const float weight_b = edge(c.pos, a.pos, sample_x, sample_y) / area;
                        const float weight_c = edge(a.pos, b.pos, sample_x, sample_y) / area;
                        if (weight_a < 0.0f || weight_b < 0.0f || weight_c < 0.0f) continue;
                        float colour[4];
                        for (int channel = 0; channel < 4; ++channel) {
                            colour[channel] = weight_a * component(a.col, channel) +
                                              weight_b * component(b.col, channel) +
                                              weight_c * component(c.col, channel);
                        }
                        for (int channel = 0; channel < 4; ++channel) accumulated[channel] += colour[channel];
                        ++hits;
                    }
                }
                if (hits == 0) continue;
                float colour[4];
                for (int channel = 0; channel < 3; ++channel) colour[channel] = accumulated[channel] / hits;
                colour[3] = accumulated[3] / 64.0f;  // partial pixels stay partial
                blend(x, y, colour);
            }
        }
    }

    static float component(const ImU32 colour, const int channel) {
        const int shift = channel == 0   ? IM_COL32_R_SHIFT
                          : channel == 1 ? IM_COL32_G_SHIFT
                          : channel == 2 ? IM_COL32_B_SHIFT
                                         : IM_COL32_A_SHIFT;
        return static_cast<float>((colour >> shift) & 0xFF) / 255.0f;
    }

    int width_ = 0;
    int height_ = 0;
    std::vector<float> pixels_;
};

}  // namespace ImHTMLTests
