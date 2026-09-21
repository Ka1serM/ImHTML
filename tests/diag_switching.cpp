// Tab switching diagnostic.
//
// Drives the sample application through random tab switches, splitter drags
// and window resizes with layout reuse on, and after each step compares every
// visible text box against an unmemoized relayout of the same state. Also
// reports how long the memoized layouts took.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "harness.h"
#include "litehtml/document.h"
#include "litehtml/element.h"
#include "litehtml/render_item.h"

namespace {

struct TextBox {
    std::string text;
    litehtml::position box;
    bool visible = false;
    std::string hidden_by;
};

void CollectText(const std::shared_ptr<litehtml::element>& element, std::vector<TextBox>& out) {
    if (!element || !element->is_visible()) return;
    if (element->is_text()) {
        std::string text;
        element->get_text(text);
        if (std::none_of(text.begin(), text.end(), [](const unsigned char ch) { return !std::isspace(ch); })) return;
        TextBox entry;
        entry.text = text;
        const auto render = element->get_render_item();
        // Text without a box or hidden by CSS (e.g. <title>) is not a layout bug.
        if (!render || !render->is_visible()) return;
        {
            entry.box = render->get_placement();
            entry.visible = true;
            // Layout and paint stop at the first invisible render ancestor,
            // including anonymous boxes that are not part of the DOM.
            for (auto ancestor = render->parent(); ancestor && entry.visible; ancestor = ancestor->parent()) {
                if (!ancestor->is_visible()) {
                    const auto source = ancestor->src_el();
                    const auto owner = source->parent();
                    entry.visible = false;
                    entry.hidden_by = std::string(source->get_tagName()) + " skip=" +
                                      (ancestor->skip() ? "1" : "0") + " el-visible=" +
                                      (source->is_visible() ? "1" : "0") + " display-none=" +
                                      (source->css().get_display() == litehtml::display_none ? "1" : "0") +
                                      " css-visible=" +
                                      (source->css().get_visibility() == litehtml::visibility_visible ? "1" : "0") +
                                      " parent=" + (owner ? owner->get_tagName() : "null") + "#" +
                                      (owner ? owner->get_attr("id", "") : "") + "." +
                                      (owner ? owner->get_attr("class", "") : "");
                }
            }
        }
        out.push_back(entry);
        return;
    }
    for (const auto& child : element->children()) CollectText(child, out);
}

std::vector<TextBox> Snapshot(ImHTML::HtmlDocument& document) {
    std::vector<TextBox> boxes;
    CollectText(ImHTMLTests::FindRaw(document, "html"), boxes);
    return boxes;
}

bool Same(const litehtml::position& a, const litehtml::position& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

}  // namespace

int main(int argc, char** argv) {
    ImHTMLTests::DataRoot() = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(IMHTML_BENCH_DATA_DIR);
    const int steps = argc > 2 ? std::atoi(argv[2]) : 300;
    const bool reuse = std::getenv("DIAG_NO_REUSE") == nullptr;
    const bool check = reuse && std::getenv("DIAG_NO_CHECK") == nullptr;
    litehtml::render_item::g_layout_reuse = reuse;
    ImHTMLTests::SetupHeadlessImGui();

    ImHTML::HtmlDocument document;
    ImHTMLTests::BuildSampleDocument(document);
    ImHTMLTests::RunFrames(document, 3);
    document.select("main", "maps");
    ImHTMLTests::RunFrames(document, 3);

    static const char* const maps_tabs[] = {"outliner", "environment", "renderer", "camera", "details"};
    static const char* const settings_tabs[] = {"provider", "encryption", "options", "versions", "auto-texture"};
    std::mt19937 rng(1234);
    std::vector<double> layout_ms;
    int mismatches = 0;
    std::string current_main = "maps";

    for (int step = 0; step < steps; ++step) {
        std::string action;
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 60) {
            const char* tab = maps_tabs[rng() % 5];
            if (current_main != "maps") document.select("main", "maps");
            current_main = "maps";
            document.select("maps", tab);
            action = std::string("maps:") + tab;
        } else if (roll < 75) {
            const long pixels = 240 + static_cast<long>(rng() % 300);
            document.set_flex_pixels("#maps-right-panel", pixels);
            action = "splitter:" + std::to_string(pixels);
        } else if (roll < 88) {
            const float width = 1100.0f + static_cast<float>(rng() % 800);
            ImGui::GetIO().DisplaySize = ImVec2(width, 900.0f);
            action = "window:" + std::to_string(static_cast<int>(width));
        } else {
            const char* tab = settings_tabs[rng() % 5];
            document.select("main", "settings");
            current_main = "settings";
            document.select("settings", std::string("settings-panel-") + tab);
            action = std::string("settings:") + tab;
        }
        ImHTMLTests::RunFrame(document);
        const auto stats = ImHTML::GetDocumentRenderStats("app");
        if (stats.last_frame_relayout) layout_ms.push_back(stats.last_layout_ms);
        if (!check) continue;

        const std::vector<TextBox> cached = Snapshot(document);
        int lost = 0;
        for (const TextBox& entry : cached) {
            if (entry.visible) continue;
            if (lost++ < 4)
                std::printf("step %3d %-22s \"%s\" is DOM-visible but hidden by render ancestor <%s>\n", step,
                            action.c_str(), entry.text.c_str(), entry.hidden_by.c_str());
        }
        if (lost) ++mismatches;
        litehtml::render_item::g_layout_reuse = false;
        ImHTML::MarkDocumentLayoutDirty("app");
        ImHTMLTests::RunFrame(document);
        const std::vector<TextBox> reference = Snapshot(document);
        litehtml::render_item::g_layout_reuse = true;
        ImHTML::MarkDocumentLayoutDirty("app");

        if (cached.size() != reference.size()) {
            std::printf("step %3d %-22s text count %zu != reference %zu\n", step, action.c_str(), cached.size(),
                        reference.size());
            ++mismatches;
            continue;
        }
        int reported = 0;
        for (std::size_t index = 0; index < cached.size(); ++index) {
            if (Same(cached[index].box, reference[index].box) && cached[index].visible == reference[index].visible)
                continue;
            if (reported++ < 4) {
                const auto& a = cached[index].box;
                const auto& b = reference[index].box;
                std::printf("step %3d %-22s \"%s\" cached (%.1f,%.1f %.1fx%.1f) ref (%.1f,%.1f %.1fx%.1f)\n", step,
                            action.c_str(), cached[index].text.c_str(), static_cast<float>(a.x),
                            static_cast<float>(a.y), static_cast<float>(a.width), static_cast<float>(a.height),
                            static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.width),
                            static_cast<float>(b.height));
            }
        }
        if (reported) ++mismatches;
    }

    std::sort(layout_ms.begin(), layout_ms.end());
    double total = 0.0;
    for (const double ms : layout_ms) total += ms;
    if (!layout_ms.empty()) {
        std::printf("layouts %zu  median %.2f ms  p90 %.2f ms  max %.2f ms  total %.1f ms\n", layout_ms.size(),
                    layout_ms[layout_ms.size() / 2], layout_ms[layout_ms.size() * 9 / 10], layout_ms.back(), total);
    }
    std::printf("%d mismatching steps\n", mismatches);
    document.shutdown();
    return mismatches == 0 ? 0 : 1;
}
