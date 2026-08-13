#include "ui_theme.h"

namespace ImHTML::Theme {
namespace {

constexpr AccentColor kFallbackAccent{0x3b, 0x9c, 0x52};

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_css_accent(std::string_view stylesheet, AccentColor& result) {
    const std::size_t declaration = stylesheet.find("--accent:");
    if (declaration == std::string_view::npos) return false;

    const std::size_t colon = declaration + 8;
    const std::size_t hash = stylesheet.find('#', colon + 1);
    if (hash == std::string_view::npos || hash + 7 > stylesheet.size()) return false;

    unsigned char channels[3]{};
    for (int channel = 0; channel < 3; ++channel) {
        const int high = hex_digit(stylesheet[hash + 1 + channel * 2]);
        const int low = hex_digit(stylesheet[hash + 2 + channel * 2]);
        if (high < 0 || low < 0) return false;
        channels[channel] = static_cast<unsigned char>((high << 4) | low);
    }

    result = AccentColor{channels[0], channels[1], channels[2]};
    return true;
}

AccentColor& accent_storage() {
    static AccentColor accent = kFallbackAccent;
    return accent;
}

}  // namespace

void initialize_from_css(std::string_view stylesheet) {
    AccentColor parsed;
    if (parse_css_accent(stylesheet, parsed)) accent_storage() = parsed;
}

const AccentColor& ui_accent() {
    return accent_storage();
}

unsigned int ui_accent_u32(unsigned char alpha) {
    const AccentColor& accent = ui_accent();
    return (static_cast<unsigned int>(alpha) << 24) | (static_cast<unsigned int>(accent.b) << 16) |
           (static_cast<unsigned int>(accent.g) << 8) | static_cast<unsigned int>(accent.r);
}

}  // namespace ImHTML::Theme
