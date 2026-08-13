#pragma once

#include <string_view>

namespace ImHTML::Theme {

struct AccentColor {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};

void initialize_from_css(std::string_view stylesheet);

const AccentColor& ui_accent();
unsigned int ui_accent_u32(unsigned char alpha = 255);

}  // namespace ImHTML::Theme
