#pragma once

#include <cstddef>
#include <cstdint>

#include "starcore/common/image.hpp"

namespace starcore::raster
{

/**
 * @brief 4-connected flood ("seed") fill: replaces the connected region of
 *        the seed pixel's color with `fillColor` using a span-based stack.
 * @return Number of pixels changed (0 if the seed is outside the image or
 *         already has the fill color).
 *
 * The legacy BoundaryFill read the seed pixel before any bounds check and
 * indexed with unsigned coordinates (mouse = -1 wrapped to 2^64-1).
 */
auto floodFill(ColorImage& image, IVec2 seed, std::uint32_t fillColor)
    -> std::size_t;

/**
 * @brief 4-connected boundary fill: paints every pixel reachable from the
 *        seed without crossing `boundaryColor`.
 */
auto boundaryFill(ColorImage& image, IVec2 seed, std::uint32_t fillColor,
                  std::uint32_t boundaryColor) -> std::size_t;

}  // namespace starcore::raster
