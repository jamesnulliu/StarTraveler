#pragma once

#include <algorithm>
#include <array>
#include <optional>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "starcore/math/types.hpp"

namespace starcore
{

struct Ray
{
    Vec3 origin{0.0F};
    Vec3 direction{0.0F, 0.0F, -1.0F};

    [[nodiscard]] constexpr auto at(float t) const -> Vec3
    {
        return origin + direction * t;
    }
};

struct Aabb
{
    Vec3 min{kInfinity};
    Vec3 max{-kInfinity};

    [[nodiscard]] constexpr auto isValid() const -> bool
    {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    constexpr void expand(Vec3 point)
    {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    constexpr void expand(const Aabb& other)
    {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    [[nodiscard]] constexpr auto center() const -> Vec3
    {
        return (min + max) * 0.5F;
    }

    [[nodiscard]] constexpr auto surfaceArea() const -> float
    {
        const Vec3 d = max - min;
        return 2.0F * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    /** @brief Bounds of this box after an affine transform. */
    [[nodiscard]] auto transformed(const Mat4& m) const -> Aabb
    {
        Aabb result;
        for (int i = 0; i < 8; ++i) {
            const Vec3 corner{(i & 1) != 0 ? max.x : min.x,
                              (i & 2) != 0 ? max.y : min.y,
                              (i & 4) != 0 ? max.z : min.z};
            result.expand(Vec3(m * Vec4(corner, 1.0F)));
        }
        return result;
    }
};

/**
 * @brief Slab test. `inverseDirection` = 1 / ray.direction (IEEE inf is fine).
 * @return true if the ray overlaps the box within [tMin, tMax].
 */
[[nodiscard]] inline auto intersectAabb(const Ray& ray, Vec3 inverseDirection,
                                        const Aabb& box, float tMin,
                                        float tMax) -> bool
{
    const Vec3 t0 = (box.min - ray.origin) * inverseDirection;
    const Vec3 t1 = (box.max - ray.origin) * inverseDirection;
    const Vec3 tNear = glm::min(t0, t1);
    const Vec3 tFar = glm::max(t0, t1);
    tMin = std::max({tMin, tNear.x, tNear.y, tNear.z});
    tMax = std::min({tMax, tFar.x, tFar.y, tFar.z});
    return tMin <= tMax;
}

/**
 * @brief Nearest intersection of a ray with a sphere in (tMin, tMax).
 *
 * Unlike the legacy Sphere::Hit, the far root is used when the origin is
 * inside the sphere, and the ray direction need not be normalized.
 */
[[nodiscard]] inline auto intersectSphere(const Ray& ray, Vec3 center,
                                          float radius, float tMin, float tMax)
    -> std::optional<float>
{
    const Vec3 oc = ray.origin - center;
    const float a = glm::dot(ray.direction, ray.direction);
    const float halfB = glm::dot(oc, ray.direction);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float discriminant = halfB * halfB - a * c;
    if (discriminant < 0.0F) {
        return std::nullopt;
    }
    const float sqrtD = std::sqrt(discriminant);
    float t = (-halfB - sqrtD) / a;
    if (t <= tMin || t >= tMax) {
        t = (-halfB + sqrtD) / a;
        if (t <= tMin || t >= tMax) {
            return std::nullopt;
        }
    }
    return t;
}

struct TriangleHit
{
    float t = 0.0F;
    // Barycentric coordinates of v1 and v2.
    float u = 0.0F;
    float v = 0.0F;
};

/**
 * @brief Closest point of triangle abc to p (Ericson, "Real-Time Collision
 *        Detection", 5.1.5): checks the Voronoi regions of vertices and edges.
 */
[[nodiscard]] inline auto closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) -> Vec3
{
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return a;
    }
    const Vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const float denominator = va + vb + vc;
    if (denominator == 0.0F) {
        return a;  // Degenerate triangle.
    }
    const float v = vb / denominator;
    const float w = vc / denominator;
    return a + ab * v + ac * w;
}

/** @brief Closest point of the solid box [-halfExtents, halfExtents] to p (p if inside). */
[[nodiscard]] inline auto closestPointOnBox(Vec3 p, Vec3 halfExtents) -> Vec3
{
    return glm::clamp(p, -halfExtents, halfExtents);
}

/** @brief Double-sided Moller-Trumbore ray/triangle test in (tMin, tMax). */
[[nodiscard]] inline auto intersectTriangle(const Ray& ray, Vec3 v0, Vec3 v1,
                                            Vec3 v2, float tMin, float tMax)
    -> std::optional<TriangleHit>
{
    constexpr float kParallelEpsilon = 1e-12F;
    const Vec3 edge1 = v1 - v0;
    const Vec3 edge2 = v2 - v0;
    const Vec3 p = glm::cross(ray.direction, edge2);
    const float determinant = glm::dot(edge1, p);
    if (std::abs(determinant) < kParallelEpsilon) {
        return std::nullopt;
    }
    const float inverseDeterminant = 1.0F / determinant;
    const Vec3 s = ray.origin - v0;
    const float u = glm::dot(s, p) * inverseDeterminant;
    if (u < 0.0F || u > 1.0F) {
        return std::nullopt;
    }
    const Vec3 q = glm::cross(s, edge1);
    const float v = glm::dot(ray.direction, q) * inverseDeterminant;
    if (v < 0.0F || u + v > 1.0F) {
        return std::nullopt;
    }
    const float t = glm::dot(edge2, q) * inverseDeterminant;
    if (t <= tMin || t >= tMax) {
        return std::nullopt;
    }
    return TriangleHit{t, u, v};
}

}  // namespace starcore
