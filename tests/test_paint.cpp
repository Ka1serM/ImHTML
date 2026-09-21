// Paint regressions.
//
// Every check here corresponds to a defect that shipped at some point:
//
//   * a scissor equal to the element's own box clipped the antialiased fringe
//     off rounded corners,
//   * AddRectFilled caps rounding at half the shorter side minus a pixel, which
//     flattens the ends of a fully rounded pill,
//   * fills snapped to the device pixel grid while their clip rects did not, so
//     a snapped box lost its bottom and right edges,
//   * a nested box sat half a pixel off centre because it and its parent
//     rounded to different sub-pixel phases.
//
// All four produce correct layout boxes, so the layout suites cannot see them.
// This one rasterizes the draw list and asserts on coverage: a shape symmetric
// about its centre must produce a mirrored coverage profile, and a centred box
// must share its parent's centre of mass, whatever the antialiasing does.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "harness.h"
#include "rasterizer.h"

namespace {

constexpr int kWidth = 200;
constexpr int kHeight = 120;

int g_failures = 0;

void Check(const bool condition, const std::string& message) {
    if (condition) {
        std::printf("ok   %s\n", message.c_str());
    } else {
        std::printf("FAIL %s\n", message.c_str());
        ++g_failures;
    }
}

std::string Profile(const std::vector<float>& values) {
    std::string text;
    for (const float value : values) {
        if (value <= 0.01f) continue;
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%.1f ", value);
        text += buffer;
    }
    return text;
}

std::vector<float> Trimmed(const std::vector<float>& values) {
    std::size_t first = 0;
    while (first < values.size() && values[first] <= 0.5f) ++first;
    std::size_t last = values.size();
    while (last > first && values[last - 1] <= 0.5f) --last;
    return std::vector<float>(values.begin() + first, values.begin() + last);
}

// Symmetry is checked two ways, both robust to the per-column noise that
// triangle tessellation leaves in the interior of a shape:
//
//   * the ends must match — a clipped or flattened edge changes the outermost
//     entries of the profile, which is exactly what every clipping defect did,
//   * the two halves must carry the same total coverage.
bool EndsMatch(const std::vector<float>& values, const std::size_t depth, const float tolerance) {
    const std::vector<float> trimmed = Trimmed(values);
    if (trimmed.size() < depth * 2) return false;
    for (std::size_t index = 0; index < depth; ++index) {
        const float near_edge = trimmed[index];
        const float far_edge = trimmed[trimmed.size() - 1 - index];
        if (std::fabs(near_edge - far_edge) > tolerance * std::max(1.0f, std::max(near_edge, far_edge))) {
            return false;
        }
    }
    return true;
}

bool HalvesBalanced(const std::vector<float>& values, const float tolerance) {
    const std::vector<float> trimmed = Trimmed(values);
    if (trimmed.size() < 2) return false;
    const std::size_t half = trimmed.size() / 2;
    double front = 0.0;
    double back = 0.0;
    for (std::size_t index = 0; index < half; ++index) {
        front += trimmed[index];
        back += trimmed[trimmed.size() - 1 - index];
    }
    const double larger = std::max(front, back);
    return larger <= 0.0 || std::fabs(front - back) / larger <= tolerance;
}

bool IsMirrored(const std::vector<float>& values, const float tolerance) {
    // Per-entry mirroring is too brittle: ImGui tessellates arcs into a fixed
    // number of segments, so interior columns wobble by a fraction of a pixel.
    // Ends and balance are what the clipping defects actually broke.
    return EndsMatch(values, 2, std::max(tolerance, 0.3f)) && HalvesBalanced(values, tolerance);
}

float TotalCoverage(const ImHTMLTests::Raster& raster) {
    float total = 0.0f;
    for (const float row : raster.row_coverage(0, 0, kWidth, kHeight)) total += row;
    return total;
}

// Centre of mass of a coverage profile: sub-pixel, and insensitive to where the
// shape happens to land on the pixel grid.
double Centroid(const std::vector<float>& values) {
    double weighted = 0.0;
    double total = 0.0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        weighted += static_cast<double>(index) * values[index];
        total += values[index];
    }
    return total > 0.0 ? weighted / total : 0.0;
}

ImHTMLTests::Raster PaintHtml(const std::string& html, const std::string& css,
                              const float framebuffer_scale = 1.0f) {
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(framebuffer_scale, framebuffer_scale);

    ImHTML::HtmlDocument document;
    document.register_shell("<html><head></head><body>" + html + "</body></html>");
    document.set_stylesheet_provider([css] { return css; });
    document.initialize(nullptr);

    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        document.frame();
        ImGui::Render();
    }

    ImHTMLTests::Raster raster(kWidth, kHeight);
    raster.draw(*ImGui::GetDrawData());
    document.shutdown();
    return raster;
}

// A switch: a fully rounded track with a fully rounded knob centred in it.
// Centring is done with flex rather than margins, so the test measures painting
// and not CSS margin collapsing.
const char* kSwitchCss = R"css(
/* Nothing paints a background except the shapes themselves, so alpha coverage
   measures the shapes alone. */
html, body { margin: 0; padding: 0; }
.track {
    display: flex;
    align-items: center;
    width: 47px;
    height: 20px;
    margin: 10px;
    padding: 0;
    border: 0;
    border-radius: 999px;
    background: #ffffff;
}
.knob {
    display: block;
    width: 29px;
    height: 16px;
    margin-left: 16px;
    border-radius: 999px;
    background: #ff0000;
}
.hidden-track { background: transparent; }
/* Pushes the shapes off the pixel grid. Snapping bugs only show when the
   layout position is fractional, which is the normal case in a real page. */
.offset { padding: 3.4px 0 0 2.6px; }
)css";

void TestPillIsSymmetric(const float scale) {
    const ImHTMLTests::Raster raster = PaintHtml("<div class=\"track\"></div>", kSwitchCss, scale);
    const std::vector<float> rows = raster.row_coverage(0, 0, kWidth, kHeight);
    const std::vector<float> columns = raster.column_coverage(0, 0, kWidth, kHeight);

    char label[64];
    std::snprintf(label, sizeof(label), " (framebuffer scale %.2f)", scale);

    const bool rows_ok = IsMirrored(rows, 0.06f);
    const bool columns_ok = IsMirrored(columns, 0.06f);
    Check(rows_ok, std::string("pill coverage mirrors top to bottom") + label);
    Check(columns_ok, std::string("pill coverage mirrors left to right") + label);
    if (!rows_ok || !columns_ok) {
        std::printf("     rows: %s\n     cols: %s\n", Profile(rows).c_str(), Profile(columns).c_str());
    }
}

// The regression that reads as "the knob has more space below it than above":
// the knob and its track rounded to different sub-pixel phases.
void TestKnobStaysCentred(const float scale) {
    const ImHTMLTests::Raster track = PaintHtml("<div class=\"track\"></div>", kSwitchCss, scale);
    const ImHTMLTests::Raster knob =
        PaintHtml("<div class=\"track hidden-track\"><div class=\"knob\"></div></div>", kSwitchCss, scale);

    const double track_centre = Centroid(track.row_coverage(0, 0, kWidth, kHeight));
    const double knob_centre = Centroid(knob.row_coverage(0, 0, kWidth, kHeight));
    const double offset = knob_centre - track_centre;

    char label[96];
    std::snprintf(label, sizeof(label), " (framebuffer scale %.2f, offset %+.2f px)", scale, offset);
    Check(std::fabs(offset) <= 0.2, std::string("knob shares the track's vertical centre") + label);
}

// Painted area is the sharpest invariant available: clipping an edge removes a
// strip of it, squaring off the caps adds the corners back, and neither is
// affected by where the shape lands on the pixel grid.
void TestPillCoversItsArea(const float scale, const bool off_grid) {
    const ImHTMLTests::Raster raster =
        PaintHtml(off_grid ? "<div class=\"offset\"><div class=\"track\"></div></div>"
                           : "<div class=\"track\"></div>",
                  kSwitchCss, scale);

    // A 47x20 pill: the rectangle minus the four corners the radius cuts away.
    constexpr float kPi = 3.14159265f;
    constexpr float radius = 10.0f;
    const float expected = 47.0f * 20.0f - (4.0f - kPi) * radius * radius;
    const float measured = TotalCoverage(raster);
    const float ratio = measured / expected;

    char label[160];
    std::snprintf(label, sizeof(label), " (framebuffer scale %.2f%s, %.0f of %.0f, ratio %.3f)", scale,
                  off_grid ? ", off-grid" : "", measured, expected, ratio);
    // Antialiasing fades the outermost half pixel of the perimeter, so a
    // correctly painted shape lands a few percent under its ideal area. Losing a
    // whole edge to a clip costs far more than that band allows.
    Check(ratio >= 0.85f && ratio <= 1.03f, std::string("pill covers its geometric area") + label);
}

// A fully rounded box must reach its corners: with the radius capped, the
// corner of the bounding box gains coverage a true pill does not have.
void TestFullyRoundedCornersAreRound() {
    const ImHTMLTests::Raster raster = PaintHtml("<div class=\"track\"></div>", kSwitchCss, 1.0f);

    int min_x = kWidth, min_y = kHeight, max_x = -1, max_y = -1;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (raster.alpha(x, y) <= 0.05f) continue;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    Check(max_x > min_x && max_y > min_y, "fully rounded box was painted at all");
    if (max_x <= min_x || max_y <= min_y) return;

    const float corner = raster.alpha(min_x, min_y);
    // Three pixels in from the left edge at mid height is solidly inside a pill of
    // this size, and outside a box whose caps were squared off.
    const float cap = raster.alpha(min_x + 3, (min_y + max_y) / 2);
    Check(corner < 0.35f, "fully rounded corner leaves the bounding-box corner empty");
    Check(cap > 0.6f, "fully rounded cap is filled at its midpoint");
    if (corner >= 0.35f || cap <= 0.6f) {
        std::printf("     corner alpha=%.3f cap alpha=%.3f\n", corner, cap);
    }
}

// Content inside an overflow:hidden box keeps its antialiased edge: a clip may
// round outward, never inward through the shape it contains.
void TestClipKeepsTheShapeItContains() {
    const char* css = R"css(
html, body { margin: 0; padding: 0; }
.frame { display: block; width: 60px; height: 40px; margin: 10px; overflow: hidden; }
.dot { display: block; width: 40px; height: 40px; border-radius: 999px; background: #ffffff; }
)css";
    const ImHTMLTests::Raster raster = PaintHtml("<div class=\"frame\"><div class=\"dot\"></div></div>", css, 1.3f);
    const std::vector<float> rows = raster.row_coverage(0, 0, kWidth, kHeight);
    const std::vector<float> columns = raster.column_coverage(0, 0, kWidth, kHeight);

    const bool rows_ok = IsMirrored(rows, 0.08f);
    const bool columns_ok = IsMirrored(columns, 0.08f);
    Check(rows_ok, "a clipped circle keeps its top and bottom edges");
    Check(columns_ok, "a clipped circle keeps its left and right edges");
    if (!rows_ok || !columns_ok) {
        std::printf("     rows: %s\n     cols: %s\n", Profile(rows).c_str(), Profile(columns).c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    ImHTMLTests::DataRoot() = argc > 1 ? std::filesystem::path(argv[1])
                                       : std::filesystem::path(IMHTML_TEST_DATA_DIR);
    ImHTMLTests::SetupHeadlessImGui(400.0f, 300.0f);

    // Integral and fractional density: the snapping defects only appear when the
    // framebuffer grid and the layout grid disagree.
    for (const float scale : {1.0f, 1.3f, 2.0f}) {
        TestPillIsSymmetric(scale);
        TestPillCoversItsArea(scale, false);
        TestPillCoversItsArea(scale, true);
        TestKnobStaysCentred(scale);
    }
    TestFullyRoundedCornersAreRound();
    TestClipKeepsTheShapeItContains();

    std::printf("%s\n", g_failures == 0 ? "paint: all checks passed" : "paint: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
