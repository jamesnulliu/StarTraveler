// starcore.raster -- the 2D raster algorithms of the original drawing board:
//
//   lines      Bresenham, midpoint and DDA, every octant, in order from a to b
//   ellipses   the midpoint algorithm, by symmetry
//   curves     Bezier (de Casteljau) and B-spline (de Boor) evaluation, sampling
//   polygons   even-odd scan-line fill
//   clipping   Cohen-Sutherland, midpoint subdivision, Sutherland-Hodgman
//   filling    flood ("seed") and boundary fill
//   strokes    the overlay's pixel writer: dash patterns, thickness, entity ids
//
// :types holds the vocabulary shared with the scene's components
// (LineAlgorithm, CurveType, ...). Definitions that are not templates live in
// implementation units (src/raster/*.cpp: `module starcore.raster;`).

export module starcore.raster;

import std;
import starcore.common;
import starcore.math;
export import :types;

export namespace starcore::raster
{

// Lines -------------------------------------------------------------------------------------------
// All algorithms handle every octant and emit pixels in order from `a` to `b`
// (so dash patterns are continuous). The legacy versions only handled
// 0 <= slope < 1 and relied on flip matrices for other octants.

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

// Ellipses ----------------------------------------------------------------------------------------

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
        bresenhamLine(center - IVec2{radiusX, radiusY}, center + IVec2{radiusX, radiusY}, plot);
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
    std::int64_t d2 = b2 * (2 * x + 1) * (2 * x + 1) + 4 * a2 * (y - 1) * (y - 1) - 4 * a2 * b2;
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

// Curves ------------------------------------------------------------------------------------------
// Bezier and B-spline curve evaluation, generic over the point type (Vec2 or
// Vec3), so curves are evaluated in their local 3D space and then projected.

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
[[nodiscard]] inline auto clampBSplineOrder(std::size_t controlPointCount, std::uint32_t order)
    -> std::uint32_t
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
                                                  std::uint32_t order) -> std::vector<float>
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
            knots[i] = static_cast<float>(i - order + 1) / static_cast<float>(interior + 1);
        }
    }
    return knots;
}

/**
 * @brief Point on a B-spline of the given order at t in [0, 1] (de Boor).
 * @param knots Knot vector of size controlPoints.size() + order.
 */
template <typename Point>
[[nodiscard]] auto evaluateBSpline(std::span<const Point> controlPoints, std::uint32_t order,
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
            const float alpha = right > left ? (t - left) / (right - left) : 0.0F;
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
[[nodiscard]] auto sampleCurve(CurveType type, std::span<const Point> controlPoints,
                               std::uint32_t order, std::size_t segments) -> std::vector<Point>
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
            evaluateBSpline(controlPoints, k, std::span<const float>(knots), t, scratch));
    }
    return polyline;
}

// Polygons ----------------------------------------------------------------------------------------

/** @brief Callable receiving a horizontal span: (y, xBegin, xEnd exclusive). */
template <typename F>
concept SpanFiller = std::invocable<F&, int, int, int>;

/**
 * @brief Even-odd scan-line polygon fill with an edge table and an active
 *        edge list, sampling at pixel centers.
 *
 * A pixel (x, y) is filled when its center (x + 0.5, y + 0.5) is inside the
 * polygon; edges are half-open in y so shared vertices are counted once.
 * Fixes of the legacy ScanLineFill: no unsigned wrap-around for negative
 * coordinates, yMin/yMax are recomputed per call, spans are clipped to
 * [0, width) x [0, height).
 */
template <SpanFiller Fill>
void scanlineFill(std::span<const Vec2> polygon, int width, int height, Fill&& fill)
{
    struct Edge
    {
        int yBegin;    // First scan line whose center the edge crosses.
        int yEnd;      // Exclusive.
        float x;       // Intersection x at the current scan line center.
        float dxPerY;  // Inverse slope.
    };

    if (polygon.size() < 3 || width <= 0 || height <= 0) {
        return;
    }

    // Edge table, sorted by the first scan line.
    std::vector<Edge> edges;
    edges.reserve(polygon.size());
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        Vec2 p = polygon[i];
        Vec2 q = polygon[(i + 1) % polygon.size()];
        if (p.y == q.y) {
            continue;  // Horizontal edges never cross a scan-line center.
        }
        if (p.y > q.y) {
            std::swap(p, q);
        }
        // Scan line y has its center at y + 0.5; edge covers [p.y, q.y).
        const int yBegin = static_cast<int>(std::ceil(p.y - 0.5F));
        const int yEnd = static_cast<int>(std::ceil(q.y - 0.5F));
        if (yBegin >= yEnd) {
            continue;
        }
        const float dxPerY = (q.x - p.x) / (q.y - p.y);
        const float x = p.x + (static_cast<float>(yBegin) + 0.5F - p.y) * dxPerY;
        edges.push_back({yBegin, yEnd, x, dxPerY});
    }
    if (edges.empty()) {
        return;
    }
    std::ranges::sort(edges, {}, &Edge::yBegin);

    int yMax = edges.front().yEnd;
    for (const auto& edge : edges) {
        yMax = std::max(yMax, edge.yEnd);
    }
    yMax = std::min(yMax, height);

    std::vector<Edge> active;
    std::size_t nextEdge = 0;
    for (int y = edges.front().yBegin; y < yMax; ++y) {
        // Add edges starting at this scan line, drop finished ones.
        while (nextEdge < edges.size() && edges[nextEdge].yBegin == y) {
            active.push_back(edges[nextEdge++]);
        }
        std::erase_if(active, [y](const Edge& e) { return e.yEnd <= y; });
        if (active.empty() && nextEdge == edges.size()) {
            break;
        }

        if (y >= 0) {
            std::ranges::sort(active, {}, &Edge::x);
            for (std::size_t i = 0; i + 1 < active.size(); i += 2) {
                // Pixels whose centers lie in [x0, x1).
                const int xBegin =
                    std::max(0, static_cast<int>(std::ceil(active[i].x - 0.5F)));
                const int xEnd =
                    std::min(width, static_cast<int>(std::ceil(active[i + 1].x - 0.5F)));
                if (xBegin < xEnd) {
                    fill(y, xBegin, xEnd);
                }
            }
        }
        for (auto& edge : active) {
            edge.x += edge.dxPerY;
        }
    }
}

// Clipping (src/raster/clip.cpp) ------------------------------------------------------------------

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
                                           float tolerance = 0.25F) -> std::optional<Segment>;

/**
 * @brief Sutherland-Hodgman polygon clipping against an axis-aligned
 *        rectangle (any polygon orientation).
 */
[[nodiscard]] auto clipSutherlandHodgman(std::span<const Vec2> polygon, const Rect& rect)
    -> std::vector<Vec2>;

// Filling (src/raster/fill.cpp) -------------------------------------------------------------------

/**
 * @brief 4-connected flood ("seed") fill: replaces the connected region of
 *        the seed pixel's color with `fillColor` using a span-based stack.
 * @return Number of pixels changed (0 if the seed is outside the image or
 *         already has the fill color).
 *
 * The legacy BoundaryFill read the seed pixel before any bounds check and
 * indexed with unsigned coordinates (mouse = -1 wrapped to 2^64-1).
 */
auto floodFill(ColorImage& image, IVec2 seed, std::uint32_t fillColor) -> std::size_t;

/**
 * @brief 4-connected boundary fill: paints every pixel reachable from the
 *        seed without crossing `boundaryColor`.
 */
auto boundaryFill(ColorImage& image, IVec2 seed, std::uint32_t fillColor,
                  std::uint32_t boundaryColor) -> std::size_t;

// Strokes (src/raster/stroke.cpp) -----------------------------------------------------------------

/**
 * @brief Pixel writer used by the overlay renderer: applies a dash pattern,
 *        stamps a thickness x thickness square per point (clipped to the
 *        image), and records the entity id for mouse picking.
 *
 * Satisfies PixelPlotter (operator()(IVec2)) and SpanFiller (operator()(y,
 * xBegin, xEnd)). The legacy Artist::PutPoint aborted the whole stamp at the
 * first out-of-bounds pixel and wrapped negative coordinates to huge unsigned
 * ones.
 */
class StrokeWriter
{
public:
    StrokeWriter(ColorImage& color, IdImage* ids, std::uint32_t packedColor,
                 std::uint32_t entityId, std::uint32_t thickness = 1,
                 LinePattern pattern = LinePattern::Solid);

    /** @brief Plot a pattern-filtered, thick point. */
    void operator()(IVec2 position);

    /** @brief Write one pixel (bounds-checked, no pattern/thickness). */
    void putPixel(int x, int y);

    /** @brief Fill pixels [xBegin, xEnd) of row y (bounds-checked). */
    void operator()(int y, int xBegin, int xEnd);

    /** @brief Restart the dash pattern (call at the start of each shape). */
    void resetPattern() noexcept
    {
        m_step = 0;
    }

    void setColor(std::uint32_t packedColor) noexcept
    {
        m_color = packedColor;
    }

private:
    [[nodiscard]] auto patternVisible() noexcept -> bool;

    ColorImage* m_image;
    IdImage* m_ids;
    std::uint32_t m_color;
    std::uint32_t m_entityId;
    int m_thickness;
    LinePattern m_pattern;
    std::uint32_t m_step = 0;
};

}  // namespace starcore::raster
