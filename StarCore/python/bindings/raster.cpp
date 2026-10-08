#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <starcore/common/image.hpp>
#include <starcore/math/color.hpp>
#include <starcore/raster/types.hpp>

#include "common.hpp"

import starcore.raster;

namespace starcore::python
{

namespace
{

using Pixels = std::vector<IVec2>;

auto toRect(Vec2 min, Vec2 max) -> Rect
{
    return Rect{min, max};
}

}  // namespace

void bindRaster(nb::module_& root)
{
    nb::module_ m = root.def_submodule(
        "raster", "Classic 2D raster algorithms (pixel space: +y down, integer pixels).");

    m.def(
        "bresenham_line",
        [](IVec2 a, IVec2 b) {
            Pixels pixels;
            raster::bresenhamLine(a, b, [&](IVec2 p) { pixels.push_back(p); });
            return pixels;
        },
        "a"_a, "b"_a);
    m.def(
        "midpoint_line",
        [](IVec2 a, IVec2 b) {
            Pixels pixels;
            raster::midpointLine(a, b, [&](IVec2 p) { pixels.push_back(p); });
            return pixels;
        },
        "a"_a, "b"_a);
    m.def(
        "dda_line",
        [](IVec2 a, IVec2 b) {
            Pixels pixels;
            raster::ddaLine(a, b, [&](IVec2 p) { pixels.push_back(p); });
            return pixels;
        },
        "a"_a, "b"_a);
    m.def(
        "draw_line",
        [](LineAlgorithm algorithm, IVec2 a, IVec2 b) {
            Pixels pixels;
            raster::drawLine(algorithm, a, b, [&](IVec2 p) { pixels.push_back(p); });
            return pixels;
        },
        "algorithm"_a, "a"_a, "b"_a, "Pixels of a line, in order from a to b.");
    m.def(
        "midpoint_ellipse",
        [](IVec2 center, int radiusX, int radiusY) {
            Pixels pixels;
            raster::midpointEllipse(center, radiusX, radiusY,
                                    [&](IVec2 p) { pixels.push_back(p); });
            return pixels;
        },
        "center"_a, "radius_x"_a, "radius_y"_a,
        "Pixels of an axis-aligned ellipse (all four quadrants, unordered).");

    m.def(
        "clip_cohen_sutherland",
        [](Vec2 a, Vec2 b, Vec2 rectMin, Vec2 rectMax) {
            return raster::clipCohenSutherland(a, b, toRect(rectMin, rectMax));
        },
        "a"_a, "b"_a, "rect_min"_a, "rect_max"_a,
        "Visible part of segment ab, or None.");
    m.def(
        "clip_midpoint_subdivision",
        [](Vec2 a, Vec2 b, Vec2 rectMin, Vec2 rectMax, float tolerance) {
            return raster::clipMidpointSubdivision(a, b, toRect(rectMin, rectMax), tolerance);
        },
        "a"_a, "b"_a, "rect_min"_a, "rect_max"_a, "tolerance"_a = 0.25F);
    m.def(
        "clip_sutherland_hodgman",
        [](const std::vector<Vec2>& polygon, Vec2 rectMin, Vec2 rectMax) {
            return raster::clipSutherlandHodgman(polygon, toRect(rectMin, rectMax));
        },
        "polygon"_a, "rect_min"_a, "rect_max"_a);

    m.def(
        "scanline_fill",
        [](const std::vector<Vec2>& polygon, int width, int height) {
            std::vector<std::tuple<int, int, int>> spans;
            raster::scanlineFill(polygon, width, height, [&](int y, int xBegin, int xEnd) {
                spans.emplace_back(y, xBegin, xEnd);
            });
            return spans;
        },
        "polygon"_a, "width"_a, "height"_a,
        "Even-odd fill sampled at pixel centers: a list of (y, x_begin, x_end) spans "
        "(x_end exclusive).");
    m.def(
        "flood_fill",
        [](const RgbaInput& rgba, IVec2 seed, Vec4 color) {
            ColorImage image = fromArray(rgba);
            const std::size_t filled = raster::floodFill(image, seed, packRgba8(color));
            return std::pair(toArray(image), filled);
        },
        "rgba"_a, "seed"_a, "color"_a,
        "4-connected flood fill on a copy of the image: returns (image, pixels changed).");

    m.def(
        "sample_curve",
        [](CurveType type, const std::vector<Vec3>& controlPoints, std::uint32_t order,
           std::size_t segments) {
            return raster::sampleCurve<Vec3>(type, controlPoints, order, segments);
        },
        "type"_a, "control_points"_a, "order"_a = 3U, "segments"_a = 64U,
        "Sample a Bezier or clamped uniform B-spline into segments + 1 points.");
}

}  // namespace starcore::python
