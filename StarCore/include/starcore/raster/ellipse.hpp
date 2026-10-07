#pragma once

#include <cstdint>
#include <cstdlib>

#include "starcore/raster/line.hpp"
#include "starcore/raster/types.hpp"

namespace starcore::raster
{

/**
 * @brief Midpoint ellipse algorithm for an axis-aligned ellipse with integer
 *        radii, plotting all four quadrants by symmetry.
 *
 * Decision variables are scaled by 4 and kept in 64-bit integers. The legacy
 * version initialised region 2 with square roots instead of squares
 * (`sqrt(b*x + .5b) + sqrt(a*(y-1)) - sqrt(a*b)`), producing broken arcs.
 */
template <PixelPlotter Plot>
void midpointEllipse(IVec2 center, int radiusX, int radiusY, Plot&& plot)
{
    radiusX = std::abs(radiusX);
    radiusY = std::abs(radiusY);
    if (radiusX == 0 || radiusY == 0) {
        bresenhamLine(center - IVec2{radiusX, radiusY},
                      center + IVec2{radiusX, radiusY}, plot);
        return;
    }

    const std::int64_t a2 = static_cast<std::int64_t>(radiusX) * radiusX;
    const std::int64_t b2 = static_cast<std::int64_t>(radiusY) * radiusY;

    auto plot4 = [&](std::int64_t x, std::int64_t y) {
        const int ix = static_cast<int>(x);
        const int iy = static_cast<int>(y);
        plot(IVec2{center.x + ix, center.y + iy});
        if (ix != 0) {
            plot(IVec2{center.x - ix, center.y + iy});
        }
        if (iy != 0) {
            plot(IVec2{center.x + ix, center.y - iy});
            if (ix != 0) {
                plot(IVec2{center.x - ix, center.y - iy});
            }
        }
    };

    std::int64_t x = 0;
    std::int64_t y = radiusY;

    // Region 1 (|slope| < 1): step x. d1 = 4 * (b^2 - a^2 b + a^2 / 4).
    std::int64_t d1 = 4 * b2 - 4 * a2 * radiusY + a2;
    plot4(x, y);
    // Stay in region 1 while b^2 (x + 1) < a^2 (y - 1/2).
    while (2 * b2 * (x + 1) < a2 * (2 * y - 1)) {
        if (d1 < 0) {
            d1 += 4 * b2 * (2 * x + 3);
        } else {
            d1 += 4 * b2 * (2 * x + 3) + 4 * a2 * (-2 * y + 2);
            --y;
        }
        ++x;
        plot4(x, y);
    }

    // Region 2 (|slope| >= 1): step y.
    // d2 = 4 * (b^2 (x + 1/2)^2 + a^2 (y - 1)^2 - a^2 b^2).
    std::int64_t d2 = b2 * (2 * x + 1) * (2 * x + 1) +
                      4 * a2 * (y - 1) * (y - 1) - 4 * a2 * b2;
    while (y > 0) {
        if (d2 < 0) {
            d2 += 4 * b2 * (2 * x + 2) + 4 * a2 * (-2 * y + 3);
            ++x;
        } else {
            d2 += 4 * a2 * (-2 * y + 3);
        }
        --y;
        plot4(x, y);
    }
}

}  // namespace starcore::raster
