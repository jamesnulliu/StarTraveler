#pragma once
// Bezier and B-spline curve evaluation, generic over the point type (Vec2 or
// Vec3), so curves are evaluated in their local 3D space and then projected.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "starcore/math/types.hpp"
#include "starcore/raster/types.hpp"

namespace starcore::raster
{

/**
 * @brief Point on a Bezier curve at t in [0, 1] (de Casteljau).
 * @param scratch Reused working storage (avoids allocations per sample).
 *
 * The legacy (dev branch) version evaluated every point n times and
 * allocated recursively.
 */
template <typename Point>
[[nodiscard]] auto evaluateBezier(std::span<const Point> controlPoints, float t,
                                  std::vector<Point>& scratch) -> Point
{
    if (controlPoints.empty()) {
        return Point{0.0F};
    }
    scratch.assign(controlPoints.begin(), controlPoints.end());
    for (std::size_t level = scratch.size() - 1; level > 0; --level) {
        for (std::size_t i = 0; i < level; ++i) {
            scratch[i] = scratch[i] * (1.0F - t) + scratch[i + 1] * t;
        }
    }
    return scratch.front();
}

/**
 * @brief Effective B-spline order: at least 2 (linear), at most the number of
 *        control points.
 */
[[nodiscard]] inline auto clampBSplineOrder(std::size_t controlPointCount,
                                            std::uint32_t order) -> std::uint32_t
{
    const auto maxOrder = static_cast<std::uint32_t>(controlPointCount);
    return std::clamp(order, std::min(2U, maxOrder), maxOrder);
}

/**
 * @brief Clamped uniform knot vector on [0, 1] (curve passes through the
 *        first and last control points).
 *
 * Deterministic, unlike the legacy random knots + random weights that made
 * the curve change shape whenever a control point was added.
 */
[[nodiscard]] inline auto makeClampedUniformKnots(std::size_t controlPointCount,
                                                  std::uint32_t order)
    -> std::vector<float>
{
    const std::size_t knotCount = controlPointCount + order;
    std::vector<float> knots(knotCount, 0.0F);
    const std::size_t interior = controlPointCount - order;  // >= 0
    for (std::size_t i = 0; i < knotCount; ++i) {
        if (i < order) {
            knots[i] = 0.0F;
        } else if (i >= controlPointCount) {
            knots[i] = 1.0F;
        } else {
            knots[i] = static_cast<float>(i - order + 1) /
                       static_cast<float>(interior + 1);
        }
    }
    return knots;
}

/**
 * @brief Point on a B-spline of the given order at t in [0, 1] (de Boor).
 * @param knots Knot vector of size controlPoints.size() + order.
 */
template <typename Point>
[[nodiscard]] auto evaluateBSpline(std::span<const Point> controlPoints,
                                   std::uint32_t order,
                                   std::span<const float> knots, float t,
                                   std::vector<Point>& scratch) -> Point
{
    const std::size_t n = controlPoints.size();
    if (n == 0) {
        return Point{0.0F};
    }
    const std::size_t degree = order - 1;
    t = std::clamp(t, knots[degree], knots[n]);

    // Knot span s with knots[s] <= t < knots[s + 1], s in [degree, n - 1].
    std::size_t span = degree;
    while (span + 1 < n && knots[span + 1] <= t) {
        ++span;
    }

    scratch.assign(controlPoints.begin() + static_cast<std::ptrdiff_t>(span - degree),
                   controlPoints.begin() + static_cast<std::ptrdiff_t>(span + 1));
    for (std::size_t r = 1; r <= degree; ++r) {
        for (std::size_t j = degree; j >= r; --j) {
            const float left = knots[j + span - degree];
            const float right = knots[j + 1 + span - r];
            const float alpha =
                right > left ? (t - left) / (right - left) : 0.0F;
            scratch[j] = scratch[j - 1] * (1.0F - alpha) + scratch[j] * alpha;
        }
    }
    return scratch[degree];
}

/**
 * @brief Sample a curve into a polyline with `segments` + 1 points.
 *        Fewer than 2 control points give an empty result.
 */
template <typename Point>
[[nodiscard]] auto sampleCurve(CurveType type,
                               std::span<const Point> controlPoints,
                               std::uint32_t order, std::size_t segments)
    -> std::vector<Point>
{
    std::vector<Point> polyline;
    if (controlPoints.size() < 2 || segments == 0) {
        return polyline;
    }
    polyline.reserve(segments + 1);
    std::vector<Point> scratch;

    if (type == CurveType::Bezier) {
        for (std::size_t i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            polyline.push_back(evaluateBezier(controlPoints, t, scratch));
        }
        return polyline;
    }

    const std::uint32_t k = clampBSplineOrder(controlPoints.size(), order);
    const auto knots = makeClampedUniformKnots(controlPoints.size(), k);
    for (std::size_t i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        polyline.push_back(
            evaluateBSpline(controlPoints, k, std::span<const float>(knots), t,
                            scratch));
    }
    return polyline;
}

}  // namespace starcore::raster
