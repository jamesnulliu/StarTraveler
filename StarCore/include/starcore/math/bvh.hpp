#pragma once
// Bounding volume hierarchy over axis-aligned boxes.
//
// Used as BLAS (triangles of a mesh), TLAS (instances of a RenderScene) and
// for physics overlap queries. The flattened layout (32-byte nodes, children
// stored next to each other) is the one a GPU traversal shader would read.

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "starcore/math/geometry.hpp"
#include "starcore/math/types.hpp"

namespace starcore
{

struct BvhNode
{
    Aabb bounds;
    // Leaf: first entry in Bvh::primitives(); inner node: index of the left
    // child (the right child is first + 1).
    std::uint32_t first = 0;
    // Leaf: number of primitives (> 0); inner node: 0.
    std::uint32_t count = 0;

    [[nodiscard]] constexpr auto isLeaf() const -> bool
    {
        return count > 0;
    }
};
static_assert(sizeof(BvhNode) == 32, "GPU-friendly node size");

/**
 * @brief Entry distance of a ray into a box within [tMin, tMax], or
 *        kInfinity if the ray misses it. `inverseDirection` = 1 / direction.
 */
[[nodiscard]] inline auto rayAabbEntry(const Ray& ray, Vec3 inverseDirection,
                                       const Aabb& box, float tMin, float tMax)
    -> float
{
    const Vec3 t0 = (box.min - ray.origin) * inverseDirection;
    const Vec3 t1 = (box.max - ray.origin) * inverseDirection;
    const Vec3 tNear = glm::min(t0, t1);
    const Vec3 tFar = glm::max(t0, t1);
    const float entry = std::max({tMin, tNear.x, tNear.y, tNear.z});
    const float exit = std::min({tMax, tFar.x, tFar.y, tFar.z});
    return entry <= exit ? entry : kInfinity;
}

[[nodiscard]] constexpr auto overlaps(const Aabb& a, const Aabb& b) -> bool
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y &&
           a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

class Bvh
{
public:
    struct BuildOptions
    {
        std::uint32_t maxLeafSize = 4;
        std::uint32_t binCount = 16;  // SAH bins per axis.
    };

    /** @brief Maximum depth; the traversal stacks are sized for it. */
    static constexpr std::uint32_t kMaxDepth = 48;

    Bvh() = default;

    /** @brief Binned-SAH build over the bounds of each primitive. */
    [[nodiscard]] static auto build(std::span<const Aabb> primitiveBounds)
        -> Bvh;
    [[nodiscard]] static auto build(std::span<const Aabb> primitiveBounds,
                                    BuildOptions options) -> Bvh;

    [[nodiscard]] auto empty() const noexcept -> bool
    {
        return m_nodes.empty();
    }

    [[nodiscard]] auto nodes() const noexcept -> std::span<const BvhNode>
    {
        return m_nodes;
    }

    /** @brief Primitive indices, referenced by leaf ranges. */
    [[nodiscard]] auto primitives() const noexcept
        -> std::span<const std::uint32_t>
    {
        return m_primitives;
    }

    [[nodiscard]] auto bounds() const -> Aabb
    {
        return m_nodes.empty() ? Aabb{} : m_nodes.front().bounds;
    }

    /**
     * @brief Closest-hit traversal: visits leaves roughly front to back and
     *        skips subtrees beyond `tMax`.
     *
     * `intersect(primitiveIndex, tMax)` tests one primitive and lowers `tMax`
     * (a float&) when it finds a closer hit.
     */
    template <typename Intersect>
    void traverse(const Ray& ray, float tMin, float& tMax,
                  Intersect&& intersect) const
    {
        if (m_nodes.empty()) {
            return;
        }
        const Vec3 inverseDirection = 1.0F / ray.direction;
        if (rayAabbEntry(ray, inverseDirection, m_nodes[0].bounds, tMin, tMax) ==
            kInfinity) {
            return;
        }
        std::array<std::pair<std::uint32_t, float>, kMaxDepth + 1> stack;
        std::size_t top = 0;
        std::uint32_t current = 0;
        while (true) {
            const BvhNode& node = m_nodes[current];
            if (node.isLeaf()) {
                for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                    intersect(m_primitives[i], tMax);
                }
            } else {
                std::uint32_t nearChild = node.first;
                std::uint32_t farChild = node.first + 1;
                float nearEntry = rayAabbEntry(ray, inverseDirection,
                                               m_nodes[nearChild].bounds, tMin, tMax);
                float farEntry = rayAabbEntry(ray, inverseDirection,
                                              m_nodes[farChild].bounds, tMin, tMax);
                if (farEntry < nearEntry) {
                    std::swap(nearChild, farChild);
                    std::swap(nearEntry, farEntry);
                }
                if (nearEntry != kInfinity) {
                    if (farEntry != kInfinity) {
                        stack[top++] = {farChild, farEntry};
                    }
                    current = nearChild;
                    continue;
                }
            }
            // Pop the next subtree that may still contain a closer hit.
            bool found = false;
            while (top > 0) {
                const auto [index, entry] = stack[--top];
                if (entry <= tMax) {
                    current = index;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return;
            }
        }
    }

    /**
     * @brief Any-hit traversal (shadow rays): stops as soon as
     *        `intersect(primitiveIndex)` returns true.
     */
    template <typename Intersect>
    [[nodiscard]] auto traverseAny(const Ray& ray, float tMin, float tMax,
                                   Intersect&& intersect) const -> bool
    {
        if (m_nodes.empty()) {
            return false;
        }
        const Vec3 inverseDirection = 1.0F / ray.direction;
        std::array<std::uint32_t, kMaxDepth + 1> stack;
        std::size_t top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const BvhNode& node = m_nodes[stack[--top]];
            if (rayAabbEntry(ray, inverseDirection, node.bounds, tMin, tMax) ==
                kInfinity) {
                continue;
            }
            if (node.isLeaf()) {
                for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                    if (intersect(m_primitives[i])) {
                        return true;
                    }
                }
            } else {
                stack[top++] = node.first;
                stack[top++] = node.first + 1;
            }
        }
        return false;
    }

    /**
     * @brief Broadphase: calls `visit(primitiveIndex)` for every primitive in
     *        the leaves overlapping `box` -- a superset of the primitives
     *        whose bounds overlap it (callers do the exact test).
     */
    template <typename Visit>
    void query(const Aabb& box, Visit&& visit) const
    {
        if (m_nodes.empty()) {
            return;
        }
        std::array<std::uint32_t, kMaxDepth + 1> stack;
        std::size_t top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const BvhNode& node = m_nodes[stack[--top]];
            if (!overlaps(node.bounds, box)) {
                continue;
            }
            if (node.isLeaf()) {
                for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                    visit(m_primitives[i]);
                }
            } else {
                stack[top++] = node.first;
                stack[top++] = node.first + 1;
            }
        }
    }

private:
    std::vector<BvhNode> m_nodes;
    std::vector<std::uint32_t> m_primitives;
};

}  // namespace starcore
