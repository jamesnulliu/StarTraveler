#pragma once
// Line rasterization. All algorithms handle every octant and emit pixels in
// order from `a` to `b` (so dash patterns are continuous). The legacy versions
// only handled 0 <= slope < 1 and relied on flip matrices for other octants.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "starcore/raster/types.hpp"

namespace starcore::raster
{

/** @brief Integer Bresenham (error-accumulation form, all octants). */
template <PixelPlotter Plot>
void bresenhamLine(IVec2 a, IVec2 b, Plot&& plot)
{
    const int dx = std::abs(b.x - a.x);
    const int dy = -std::abs(b.y - a.y);
    const int sx = a.x < b.x ? 1 : -1;
    const int sy = a.y < b.y ? 1 : -1;
    int error = dx + dy;
    while (true) {
        plot(a);
        if (a == b) {
            return;
        }
        const int doubled = 2 * error;
        if (doubled >= dy) {
            error += dy;
            a.x += sx;
        }
        if (doubled <= dx) {
            error += dx;
            a.y += sy;
        }
    }
}

/**
 * @brief Midpoint line algorithm: steps along the major axis and decides on
 *        the minor axis by the sign of the implicit line at the midpoint.
 */
template <PixelPlotter Plot>
void midpointLine(IVec2 a, IVec2 b, Plot&& plot)
{
    const bool steep = std::abs(b.y - a.y) > std::abs(b.x - a.x);
    if (steep) {
        std::swap(a.x, a.y);
        std::swap(b.x, b.y);
    }
    const int du = std::abs(b.x - a.x);
    const int dv = std::abs(b.y - a.y);
    const int su = a.x < b.x ? 1 : -1;
    const int sv = a.y < b.y ? 1 : -1;

    auto emit = [&](int u, int v) { plot(steep ? IVec2{v, u} : IVec2{u, v}); };

    int u = a.x;
    int v = a.y;
    int decision = 2 * dv - du;
    emit(u, v);
    for (int i = 0; i < du; ++i) {
        u += su;
        if (decision > 0) {
            v += sv;
            decision += 2 * (dv - du);
        } else {
            decision += 2 * dv;
        }
        emit(u, v);
    }
}

/** @brief Digital differential analyzer (floating-point increments). */
template <PixelPlotter Plot>
void ddaLine(IVec2 a, IVec2 b, Plot&& plot)
{
    const int dx = b.x - a.x;
    const int dy = b.y - a.y;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) {
        plot(a);
        return;
    }
    const float xStep = static_cast<float>(dx) / static_cast<float>(steps);
    const float yStep = static_cast<float>(dy) / static_cast<float>(steps);
    for (int i = 0; i <= steps; ++i) {
        plot(IVec2{a.x + roundToInt(xStep * static_cast<float>(i)),
                   a.y + roundToInt(yStep * static_cast<float>(i))});
    }
}

template <PixelPlotter Plot>
void drawLine(LineAlgorithm algorithm, IVec2 a, IVec2 b, Plot&& plot)
{
    switch (algorithm) {
    case LineAlgorithm::Bresenham:
        bresenhamLine(a, b, plot);
        break;
    case LineAlgorithm::MidPoint:
        midpointLine(a, b, plot);
        break;
    case LineAlgorithm::Dda:
        ddaLine(a, b, plot);
        break;
    }
}

}  // namespace starcore::raster
