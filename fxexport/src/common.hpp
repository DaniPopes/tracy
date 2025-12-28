#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

inline double ns_to_ms(int64_t ns)
{
    return static_cast<double>(ns) / 1e6;
}

inline bool is_kernel_address(uint64_t addr)
{
    return (addr >> 63) != 0;
}

inline std::optional<std::string> toGraphColor(uint32_t rgb) {
    // Valid colors: https://github.com/firefox-devtools/profiler/blob/0d72df877672802eae9e48da1a40511b74b33010/src/types/profile.ts#L509

    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >> 8) & 0xFF;
    int b = rgb & 0xFF;

    if (r == 0xff && g == 0xff && b == 0xff) {
        return std::nullopt;
    }

    struct ColorDef {
        const char* name;
        int r, g, b;
    };

    const ColorDef palette[] = {
        {"blue",    0, 112, 243},
        {"green",   16, 185, 129},
        {"grey",    156, 163, 175},
        {"ink",     17, 24, 39},
        {"magenta", 236, 72, 153},
        {"orange",  249, 115, 22},
        {"purple",  168, 85, 247},
        {"red",     239, 68, 68},
        {"teal",    20, 184, 166},
        {"yellow",  234, 179, 8}
    };

    // Find closest color using Euclidean distance
    std::optional<std::string> closestColor = std::nullopt;
    double minDistance = std::numeric_limits<double>::max();
    for (const auto& color : palette) {
        double dr = r - color.r;
        double dg = g - color.g;
        double db = b - color.b;
        double distance = std::sqrt(dr*dr + dg*dg + db*db);

        if (distance < minDistance) {
            minDistance = distance;
            closestColor = color.name;
        }
    }

    return closestColor;
}
