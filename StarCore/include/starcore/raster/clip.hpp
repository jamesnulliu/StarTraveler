#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "starcore/math/types.hpp"

namespace starcore::raster
{

using Segment = std::pair<Vec2, Vec2>;

/**
 * @brief Cohen-Sutherland line clipping against an axis-aligned rectangle.
 * @return The visible part, or nullopt if the segment is fully outside.
 *
 * Fixes of the legacy version: intersections are computed in floating point
 * (not truncated to int), axis-parallel segments never divide by zero, and a
 * rejected segment is reported as nullopt instead of the bogus {(-1,-1),(0,0)}.
 */
[[nodiscard]] auto clipCohenSutherland(Vec2 a, Vec2 b, const Rect& rect)
    -> std::optional<Segment>;

/**
 * @brief Midpoint-subdivision line clipping: binary-search each outside
 *        endpoint towards the farthest visible point.
 * @param tolerance Search stops when the bracket is shorter than this.
 */
[[nodiscard]] auto clipMidpointSubdivision(Vec2 a, Vec2 b, const Rect& rect,
                                           float tolerance = 0.25F)
    -> std::optional<Segment>;

/**
 * @brief Sutherland-Hodgman polygon clipping against an axis-aligned
 *        rectangle (any polygon orientation).
 */
[[nodiscard]] auto clipSutherlandHodgman(std::span<const Vec2> polygon,
                                         const Rect& rect) -> std::vector<Vec2>;

}  // namespace starcore::raster
