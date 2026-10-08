#include <cmath>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/geometric.hpp>

#include <starcore/common/image.hpp>
#include <starcore/raster/types.hpp>

// The raster algorithms are a C++20 module.
import starcore.raster;

using namespace starcore;
using namespace starcore::raster;
using Catch::Matchers::WithinAbs;

namespace
{

auto collectLine(LineAlgorithm algorithm, IVec2 a, IVec2 b) -> std::vector<IVec2>
{
    std::vector<IVec2> pixels;
    drawLine(algorithm, a, b, [&](IVec2 p) { pixels.push_back(p); });
    return pixels;
}

auto shoelaceArea(const std::vector<Vec2>& polygon) -> float
{
    float area = 0.0F;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2 p = polygon[i];
        const Vec2 q = polygon[(i + 1) % polygon.size()];
        area += p.x * q.y - q.x * p.y;
    }
    return std::abs(area) * 0.5F;
}

}  // namespace

TEST_CASE("Line algorithms cover all octants", "[raster][line]")
{
    const auto algorithm = GENERATE(LineAlgorithm::Bresenham,
                                    LineAlgorithm::MidPoint, LineAlgorithm::Dda);
    const IVec2 a{10, 10};
    const IVec2 b = GENERATE(IVec2{17, 12}, IVec2{12, 17}, IVec2{3, 12},
                             IVec2{8, 17}, IVec2{3, 8}, IVec2{8, 3},
                             IVec2{17, 8}, IVec2{12, 3}, IVec2{10, 10},
                             IVec2{10, 20}, IVec2{25, 10});
    CAPTURE(enumToString(algorithm), b.x, b.y);

    const auto pixels = collectLine(algorithm, a, b);
    REQUIRE(pixels.front() == a);
    REQUIRE(pixels.back() == b);
    // One pixel per step along the major axis.
    REQUIRE(pixels.size() ==
            static_cast<std::size_t>(std::max(std::abs(b.x - a.x), std::abs(b.y - a.y)) + 1));
    // 8-connected, no gaps.
    for (std::size_t i = 1; i < pixels.size(); ++i) {
        const IVec2 d = pixels[i] - pixels[i - 1];
        REQUIRE(std::max(std::abs(d.x), std::abs(d.y)) == 1);
    }
}

TEST_CASE("Bresenham and midpoint produce the same pixels", "[raster][line]")
{
    const IVec2 b = GENERATE(IVec2{37, 5}, IVec2{-20, 31}, IVec2{4, -29});
    REQUIRE(collectLine(LineAlgorithm::Bresenham, {0, 0}, b) ==
            collectLine(LineAlgorithm::MidPoint, {0, 0}, b));
}

TEST_CASE("Cohen-Sutherland clipping", "[raster][clip]")
{
    const Rect rect{{0.0F, 0.0F}, {100.0F, 50.0F}};

    SECTION("inside segments are unchanged")
    {
        const auto s = clipCohenSutherland({10, 10}, {20, 30}, rect);
        REQUIRE(s.has_value());
        REQUIRE(s->first == Vec2{10, 10});
        REQUIRE(s->second == Vec2{20, 30});
    }
    SECTION("rejected segments are nullopt (legacy returned {(-1,-1),(0,0)})")
    {
        REQUIRE_FALSE(clipCohenSutherland({-10, -10}, {-5, 40}, rect));
        REQUIRE_FALSE(clipCohenSutherland({-10, 60}, {120, 60}, rect));
    }
    SECTION("axis-parallel segments do not divide by zero")
    {
        const auto vertical = clipCohenSutherland({50, -20}, {50, 80}, rect);
        REQUIRE(vertical.has_value());
        REQUIRE(vertical->first == Vec2{50, 0});
        REQUIRE(vertical->second == Vec2{50, 50});
    }
    SECTION("intersections keep sub-pixel precision")
    {
        const auto s = clipCohenSutherland({-10, 0}, {10, 3}, rect);
        REQUIRE(s.has_value());
        REQUIRE_THAT(s->first.y, WithinAbs(1.5, 1e-5));
    }
}

TEST_CASE("Midpoint subdivision agrees with Cohen-Sutherland", "[raster][clip]")
{
    const Rect rect{{0.0F, 0.0F}, {100.0F, 50.0F}};
    const auto [a, b] = GENERATE(std::pair{Vec2{-30, -10}, Vec2{130, 70}},
                                 std::pair{Vec2{50, 25}, Vec2{200, 25}},
                                 std::pair{Vec2{-50, 25}, Vec2{50, 100}},
                                 std::pair{Vec2{10, 10}, Vec2{90, 40}});
    const auto expected = clipCohenSutherland(a, b, rect);
    const auto actual = clipMidpointSubdivision(a, b, rect, 0.01F);
    REQUIRE(expected.has_value() == actual.has_value());
    if (expected) {
        REQUIRE(glm::distance(expected->first, actual->first) < 0.05F);
        REQUIRE(glm::distance(expected->second, actual->second) < 0.05F);
    }
    REQUIRE_FALSE(clipMidpointSubdivision({-10, -10}, {-5, 80}, rect));
    // x + y = 12 passes the (5, 5) corner without entering the rectangle.
    const Rect small{{0, 0}, {5, 5}};
    REQUIRE_FALSE(clipCohenSutherland({-20, 32}, {32, -20}, small));
    REQUIRE_FALSE(clipMidpointSubdivision({-20, 32}, {32, -20}, small));
}

TEST_CASE("Sutherland-Hodgman clips polygons to the rectangle", "[raster][clip]")
{
    const Rect rect{{0.0F, 0.0F}, {10.0F, 10.0F}};
    const std::vector<Vec2> square{{-5, -5}, {5, -5}, {5, 5}, {-5, 5}};
    REQUIRE_THAT(shoelaceArea(clipSutherlandHodgman(square, rect)), WithinAbs(25.0, 1e-4));

    std::vector<Vec2> clockwise(square.rbegin(), square.rend());
    REQUIRE_THAT(shoelaceArea(clipSutherlandHodgman(clockwise, rect)), WithinAbs(25.0, 1e-4));

    const std::vector<Vec2> outside{{20, 20}, {30, 20}, {25, 30}};
    REQUIRE(clipSutherlandHodgman(outside, rect).empty());
}

TEST_CASE("Scan-line fill samples pixel centers", "[raster][polygon]")
{
    auto fillCount = [](const std::vector<Vec2>& polygon, int w, int h) {
        std::set<std::pair<int, int>> pixels;
        scanlineFill(polygon, w, h, [&](int y, int x0, int x1) {
            for (int x = x0; x < x1; ++x) {
                REQUIRE(pixels.emplace(x, y).second);  // No pixel twice.
            }
        });
        return pixels.size();
    };

    // [1, 5) x [2, 6) contains exactly 4 x 4 pixel centers.
    REQUIRE(fillCount({{1, 2}, {5, 2}, {5, 6}, {1, 6}}, 20, 20) == 16);
    // Clipped to the image, negative coordinates are fine.
    REQUIRE(fillCount({{-5, -5}, {3, -5}, {3, 3}, {-5, 3}}, 20, 20) == 9);
    // A large triangle: count close to its area.
    const std::vector<Vec2> triangle{{0, 0}, {100, 0}, {0, 100}};
    const auto count = static_cast<float>(fillCount(triangle, 200, 200));
    REQUIRE(std::abs(count - 5000.0F) < 100.0F);
    // Degenerate input.
    REQUIRE(fillCount({{0, 0}, {5, 5}}, 20, 20) == 0);
}

TEST_CASE("Midpoint ellipse stays on the ellipse", "[raster][ellipse]")
{
    const auto [a, b] = GENERATE(std::pair{20, 10}, std::pair{7, 31},
                                 std::pair{15, 15}, std::pair{1, 1});
    std::set<std::pair<int, int>> pixels;
    midpointEllipse({0, 0}, a, b, [&](IVec2 p) { pixels.emplace(p.x, p.y); });

    for (const auto& [x, y] : pixels) {
        // Every pixel is within ~1 pixel of the ideal curve.
        const float fx = static_cast<float>(x) / static_cast<float>(a);
        const float fy = static_cast<float>(y) / static_cast<float>(b);
        const float radial = std::sqrt(fx * fx + fy * fy);
        REQUIRE(std::abs(radial - 1.0F) * static_cast<float>(std::min(a, b)) < 1.0F);
        // Four-fold symmetry.
        REQUIRE(pixels.contains({-x, y}));
        REQUIRE(pixels.contains({x, -y}));
    }
    REQUIRE(pixels.contains({a, 0}));
    REQUIRE(pixels.contains({0, b}));
}

TEST_CASE("Flood fill and boundary fill", "[raster][fill]")
{
    ColorImage image(8, 8, 0U);
    constexpr std::uint32_t kWall = 0xFF0000FFU;
    for (std::uint32_t i = 0; i < 8; ++i) {
        image.at(4, i) = kWall;  // Vertical wall splits the image.
    }

    REQUIRE(floodFill(image, {1, 1}, 0xFFFFFFFFU) == 32);  // Left half.
    REQUIRE(image.at(0, 7) == 0xFFFFFFFFU);
    REQUIRE(image.at(6, 6) == 0U);
    REQUIRE(floodFill(image, {1, 1}, 0xFFFFFFFFU) == 0);   // Already filled.
    REQUIRE(floodFill(image, {-1, 3}, 0x1U) == 0);         // Outside: no crash.

    REQUIRE(boundaryFill(image, {6, 6}, 0xFF00FF00U, kWall) == 24);
    REQUIRE(image.at(4, 0) == kWall);
}

TEST_CASE("Bezier and B-spline curves", "[raster][curve]")
{
    const std::vector<Vec2> controlPoints{{0, 0}, {1, 2}, {3, 2}, {4, 0}, {6, 1}};
    std::vector<Vec2> scratch;

    SECTION("Bezier interpolates its end points")
    {
        REQUIRE(evaluateBezier<Vec2>(controlPoints, 0.0F, scratch) == controlPoints.front());
        REQUIRE(glm::distance(evaluateBezier<Vec2>(controlPoints, 1.0F, scratch),
                              controlPoints.back()) < 1e-5F);
    }
    SECTION("clamped B-spline interpolates its end points, deterministically")
    {
        const auto order = GENERATE(2U, 3U, 4U, 5U);
        const auto first = sampleCurve<Vec2>(CurveType::BSpline, controlPoints, order, 64);
        const auto second = sampleCurve<Vec2>(CurveType::BSpline, controlPoints, order, 64);
        REQUIRE(first == second);  // Legacy used random knots and weights.
        REQUIRE(glm::distance(first.front(), controlPoints.front()) < 1e-5F);
        REQUIRE(glm::distance(first.back(), controlPoints.back()) < 1e-5F);
    }
    SECTION("order 2 B-spline is the control polygon")
    {
        const auto knots = makeClampedUniformKnots(controlPoints.size(), 2);
        // Knot 0.25 corresponds to the second control point.
        REQUIRE(glm::distance(evaluateBSpline<Vec2>(controlPoints, 2, knots, 0.25F, scratch),
                              controlPoints[1]) < 1e-5F);
    }
    SECTION("degenerate input")
    {
        REQUIRE(sampleCurve<Vec2>(CurveType::BSpline, std::span(controlPoints).first(1), 3, 8).empty());
        REQUIRE(clampBSplineOrder(2, 7) == 2U);
    }
}

TEST_CASE("StrokeWriter clips, stamps and records ids", "[raster][stroke]")
{
    ColorImage color(10, 10, 0U);
    IdImage ids(10, 10, kNoEntityId);
    StrokeWriter writer(color, &ids, 0xFFFFFFFFU, 42, 3);

    writer(IVec2{0, 0});  // Partially outside: the visible part is still drawn.
    REQUIRE(color.at(0, 0) == 0xFFFFFFFFU);
    REQUIRE(color.at(1, 1) == 0xFFFFFFFFU);
    REQUIRE(color.at(2, 2) == 0U);
    REQUIRE(ids.at(1, 0) == 42U);

    writer(IVec2{-100, 5});  // Fully outside: no effect, no crash.
    writer(5, -3, 100);      // Span clipped to the row.
    REQUIRE(color.at(9, 5) == 0xFFFFFFFFU);
}
