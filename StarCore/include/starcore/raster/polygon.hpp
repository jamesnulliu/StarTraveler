#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <span>
#include <vector>

#include "starcore/math/types.hpp"

namespace starcore::raster
{

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
void scanlineFill(std::span<const Vec2> polygon, int width, int height,
                  Fill&& fill)
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
        const float x =
            p.x + (static_cast<float>(yBegin) + 0.5F - p.y) * dxPerY;
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
                const int xBegin = std::max(
                    0, static_cast<int>(std::ceil(active[i].x - 0.5F)));
                const int xEnd = std::min(
                    width,
                    static_cast<int>(std::ceil(active[i + 1].x - 0.5F)));
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

}  // namespace starcore::raster
