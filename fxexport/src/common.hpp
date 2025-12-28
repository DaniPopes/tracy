#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

enum class MarkerPhase {
    Instant = 0,
    Interval = 1,
    IntervalStart = 2,
    IntervalEnd = 3,
};

inline double ns_to_ms(int64_t ns)
{
    return static_cast<double>(ns) / 1e6;
}

inline bool is_kernel_address(uint64_t addr)
{
    return (addr >> 63) != 0;
}

inline uint32_t color_to_rgb(uint32_t color)
{
    return color & 0xFFFFFF;
}
