#ifndef _COMPONENT_COLOR_PALETTE_H_
#define _COMPONENT_COLOR_PALETTE_H_

#include <array>
#include <cstddef>
#include <stdint.h>

struct ComponentPaletteColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char* name;
};

// 固定高亮纯色调色板，优先保证多人同时查找时的颜色辨识度。
inline constexpr std::array<ComponentPaletteColor, 8> kPureColorPalette = {{
    {255, 0, 0, "红色"},
    {0, 255, 0, "绿色"},
    {0, 0, 255, "蓝色"},
    {255, 255, 0, "黄色"},
    {255, 0, 255, "紫色"},
    {0, 255, 255, "青色"},
    {255, 255, 255, "白色"},
    {255, 128, 0, "橙色"},
}};

inline constexpr ComponentPaletteColor GetPaletteColor(size_t index) {
    return kPureColorPalette[index % kPureColorPalette.size()];
}

#endif // _COMPONENT_COLOR_PALETTE_H_
