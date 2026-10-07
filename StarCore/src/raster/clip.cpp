#include <glm/geometric.hpp>

#include "starcore/raster/clip.hpp"

namespace starcore::raster
{

namespace
{

enum OutCode : unsigned
{
    kInside = 0U,
    kLeft = 1U << 0U,
    kRight = 1U << 1U,
    kBelowMin = 1U << 2U,  // y < rect.min.y
    kAboveMax = 1U << 3U,  // y > rect.max.y
};

auto outCode(Vec2 p, const Rect& r) -> unsigned
{
    unsigned code = kInside;
    if (p.x < r.min.x) {
        code |= kLeft;
    } else if (p.x > r.max.x) {
        code |= kRight;
    }
    if (p.y < r.min.y) {
        code |= kBelowMin;
    } else if (p.y > r.max.y) {
        code |= kAboveMax;
    }
    return code;
}

}  // namespace

auto clipCohenSutherland(Vec2 a, Vec2 b, const Rect& rect)
    -> std::optional<Segment>
{
    unsigned codeA = outCode(a, rect);
    unsigned codeB = outCode(b, rect);
    while (true) {
        if ((codeA | codeB) == 0U) {
            return Segment{a, b};
        }
        if ((codeA & codeB) != 0U) {
            return std::nullopt;
        }
        // At least one endpoint is outside; its region bit guarantees the
        // segment crosses that boundary, so the divisor below is non-zero.
        const unsigned out = codeA != 0U ? codeA : codeB;
        Vec2 p{};
        if ((out & kAboveMax) != 0U) {
            p.x = a.x + (b.x - a.x) * (rect.max.y - a.y) / (b.y - a.y);
            p.y = rect.max.y;
        } else if ((out & kBelowMin) != 0U) {
            p.x = a.x + (b.x - a.x) * (rect.min.y - a.y) / (b.y - a.y);
            p.y = rect.min.y;
        } else if ((out & kRight) != 0U) {
            p.y = a.y + (b.y - a.y) * (rect.max.x - a.x) / (b.x - a.x);
            p.x = rect.max.x;
        } else {
            p.y = a.y + (b.y - a.y) * (rect.min.x - a.x) / (b.x - a.x);
            p.x = rect.min.x;
        }
        if (out == codeA) {
            a = p;
            codeA = outCode(a, rect);
        } else {
            b = p;
            codeB = outCode(b, rect);
        }
    }
}

namespace
{

/**
 * @brief Farthest visible point from `from` towards `to` on the segment, by
 *        repeatedly discarding the half that is trivially invisible.
 */
auto farthestVisible(Vec2 from, Vec2 to, const Rect& rect, float tolerance)
    -> std::optional<Vec2>
{
    if (outCode(to, rect) == kInside) {
        return to;
    }
    if ((outCode(from, rect) & outCode(to, rect)) != 0U) {
        return std::nullopt;
    }
    Vec2 near = from;  // Bracket [near, far] contains the boundary crossing.
    Vec2 far = to;
    bool sawVisible = outCode(from, rect) == kInside;
    while (glm::distance(near, far) > tolerance) {
        const Vec2 mid = (near + far) * 0.5F;
        const unsigned midCode = outCode(mid, rect);
        if (midCode == kInside) {
            sawVisible = true;
            near = mid;
        } else if ((midCode & outCode(far, rect)) != 0U) {
            // [mid, far] is trivially invisible.
            far = mid;
        } else {
            near = mid;
        }
    }
    if (!sawVisible && outCode(near, rect) != kInside) {
        return std::nullopt;
    }
    // Snap onto the rectangle to remove the tolerance overshoot.
    return glm::clamp(near, rect.min, rect.max);
}

}  // namespace

auto clipMidpointSubdivision(Vec2 a, Vec2 b, const Rect& rect, float tolerance)
    -> std::optional<Segment>
{
    if ((outCode(a, rect) & outCode(b, rect)) != 0U) {
        return std::nullopt;
    }
    // The visible part is [farthest visible from b, farthest visible from a].
    const auto end = farthestVisible(a, b, rect, tolerance);
    if (!end.has_value()) {
        return std::nullopt;
    }
    const auto start = farthestVisible(b, a, rect, tolerance);
    if (!start.has_value()) {
        return std::nullopt;
    }
    return Segment{*start, *end};
}

auto clipSutherlandHodgman(std::span<const Vec2> polygon, const Rect& rect)
    -> std::vector<Vec2>
{
    std::vector<Vec2> output(polygon.begin(), polygon.end());
    std::vector<Vec2> input;

    // Clip against the four half-planes: x >= min.x, x <= max.x, ...
    for (int edge = 0; edge < 4 && !output.empty(); ++edge) {
        const int axis = edge / 2;          // 0 = x, 1 = y
        const bool keepGreater = edge % 2 == 0;
        const float bound = keepGreater ? rect.min[axis] : rect.max[axis];
        auto inside = [&](Vec2 p) {
            return keepGreater ? p[axis] >= bound : p[axis] <= bound;
        };
        auto intersect = [&](Vec2 p, Vec2 q) {
            const float t = (bound - p[axis]) / (q[axis] - p[axis]);
            Vec2 r = p + (q - p) * t;
            r[axis] = bound;  // Exact on the boundary.
            return r;
        };

        input.swap(output);
        output.clear();
        for (std::size_t i = 0; i < input.size(); ++i) {
            const Vec2 current = input[i];
            const Vec2 next = input[(i + 1) % input.size()];
            const bool currentInside = inside(current);
            const bool nextInside = inside(next);
            if (currentInside) {
                output.push_back(current);
                if (!nextInside) {
                    output.push_back(intersect(current, next));
                }
            } else if (nextInside) {
                output.push_back(intersect(current, next));
            }
        }
    }
    return output;
}

}  // namespace starcore::raster
