module starcore.math;

import std;

namespace starcore
{

namespace
{

struct Bin
{
    Aabb bounds;
    std::uint32_t count = 0;
};

struct Split
{
    int axis = -1;
    std::uint32_t bin = 0;  // Primitives in bins [0, bin] go left.
    float cost = kInfinity;
};

class Builder
{
public:
    Builder(std::span<const Aabb> bounds, Bvh::BuildOptions options,
            std::vector<BvhNode>& nodes, std::vector<std::uint32_t>& primitives)
        : m_bounds(bounds), m_options(options), m_nodes(nodes), m_primitives(primitives)
    {
        m_options.maxLeafSize = std::max(1U, m_options.maxLeafSize);
        m_options.binCount = std::clamp(m_options.binCount, 2U, 64U);
        m_centroids.reserve(bounds.size());
        for (const Aabb& box : bounds) {
            m_centroids.push_back(box.center());
        }
    }

    void run()
    {
        const auto count = static_cast<std::uint32_t>(m_bounds.size());
        m_primitives.resize(count);
        std::iota(m_primitives.begin(), m_primitives.end(), 0U);
        // A binary tree with n leaves (at most n) has 2n - 1 nodes: no reallocation.
        m_nodes.reserve(2 * static_cast<std::size_t>(count));
        m_nodes.push_back(BvhNode{boundsOf(0, count), 0, count});

        struct Task
        {
            std::uint32_t node;
            std::uint32_t depth;
        };
        std::vector<Task> tasks{{0, 0}};
        while (!tasks.empty()) {
            const Task task = tasks.back();
            tasks.pop_back();
            if (const auto children = subdivide(task.node, task.depth)) {
                tasks.push_back({children->first, task.depth + 1});
                tasks.push_back({children->second, task.depth + 1});
            }
        }
    }

private:
    [[nodiscard]] auto boundsOf(std::uint32_t first, std::uint32_t count) const -> Aabb
    {
        Aabb result;
        for (std::uint32_t i = first; i < first + count; ++i) {
            result.expand(m_bounds[m_primitives[i]]);
        }
        return result;
    }

    [[nodiscard]] auto findSplit(const BvhNode& node, const Aabb& centroidBounds) const
        -> Split
    {
        const std::uint32_t binCount = m_options.binCount;
        Split best;
        std::vector<Bin> bins(binCount);
        std::vector<float> rightCost(binCount);
        for (int axis = 0; axis < 3; ++axis) {
            const float extent = centroidBounds.max[axis] - centroidBounds.min[axis];
            if (!(extent > 0.0F)) {
                continue;
            }
            std::ranges::fill(bins, Bin{});
            const float scale = static_cast<float>(binCount) / extent;
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const std::uint32_t primitive = m_primitives[i];
                const auto bin = std::min(
                    binCount - 1, static_cast<std::uint32_t>(
                                      (m_centroids[primitive][axis] -
                                       centroidBounds.min[axis]) * scale));
                bins[bin].bounds.expand(m_bounds[primitive]);
                ++bins[bin].count;
            }
            // Sweep from the right: cost of the right side for splits after bin b.
            Aabb rightBounds;
            std::uint32_t rightCount = 0;
            for (std::uint32_t b = binCount - 1; b > 0; --b) {
                rightBounds.expand(bins[b].bounds);
                rightCount += bins[b].count;
                rightCost[b - 1] = rightCount > 0
                                       ? rightBounds.surfaceArea() *
                                             static_cast<float>(rightCount)
                                       : 0.0F;
            }
            Aabb leftBounds;
            std::uint32_t leftCount = 0;
            for (std::uint32_t b = 0; b + 1 < binCount; ++b) {
                leftBounds.expand(bins[b].bounds);
                leftCount += bins[b].count;
                if (leftCount == 0 || leftCount == node.count) {
                    continue;
                }
                const float cost = leftBounds.surfaceArea() *
                                       static_cast<float>(leftCount) +
                                   rightCost[b];
                if (cost < best.cost) {
                    best = Split{axis, b, cost};
                }
            }
        }
        return best;
    }

    /** @return The children's node indices, or nullopt if the node became a leaf. */
    auto subdivide(std::uint32_t nodeIndex, std::uint32_t depth)
        -> std::optional<std::pair<std::uint32_t, std::uint32_t>>
    {
        BvhNode& node = m_nodes[nodeIndex];
        if (node.count <= m_options.maxLeafSize || depth >= Bvh::kMaxDepth) {
            return std::nullopt;
        }

        Aabb centroidBounds;
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            centroidBounds.expand(m_centroids[m_primitives[i]]);
        }
        const Split split = findSplit(node, centroidBounds);

        const auto begin = m_primitives.begin() + node.first;
        const auto end = begin + node.count;
        auto middle = begin;
        if (split.axis >= 0) {
            // SAH: compare with the cost of a leaf (traversal cost ~ 1 primitive test).
            const float leafCost = node.bounds.surfaceArea() * static_cast<float>(node.count);
            if (split.cost + node.bounds.surfaceArea() >= leafCost &&
                node.count <= 4 * m_options.maxLeafSize) {
                return std::nullopt;
            }
            const float scale = static_cast<float>(m_options.binCount) /
                                (centroidBounds.max[split.axis] - centroidBounds.min[split.axis]);
            middle = std::partition(begin, end, [&](std::uint32_t primitive) {
                const auto bin = std::min(
                    m_options.binCount - 1,
                    static_cast<std::uint32_t>(
                        (m_centroids[primitive][split.axis] - centroidBounds.min[split.axis]) *
                        scale));
                return bin <= split.bin;
            });
        }
        if (middle == begin || middle == end) {
            // All centroids coincide (or float rounding): split by index.
            middle = begin + node.count / 2;
        }

        const auto leftCount = static_cast<std::uint32_t>(middle - begin);
        const std::uint32_t first = node.first;
        const std::uint32_t count = node.count;
        const auto left = static_cast<std::uint32_t>(m_nodes.size());
        // `node` may not be used after the push_backs below (no reallocation
        // thanks to reserve(), but keep the code obviously safe).
        node.first = left;
        node.count = 0;
        m_nodes.push_back(BvhNode{boundsOf(first, leftCount), first, leftCount});
        m_nodes.push_back(BvhNode{boundsOf(first + leftCount, count - leftCount),
                                  first + leftCount, count - leftCount});
        return std::pair{left, left + 1};
    }

    std::span<const Aabb> m_bounds;
    Bvh::BuildOptions m_options;
    std::vector<BvhNode>& m_nodes;
    std::vector<std::uint32_t>& m_primitives;
    std::vector<Vec3> m_centroids;
};

}  // namespace

auto Bvh::build(std::span<const Aabb> primitiveBounds) -> Bvh
{
    return build(primitiveBounds, BuildOptions{});
}

auto Bvh::build(std::span<const Aabb> primitiveBounds, BuildOptions options) -> Bvh
{
    Bvh bvh;
    if (!primitiveBounds.empty()) {
        Builder(primitiveBounds, options, bvh.m_nodes, bvh.m_primitives).run();
    }
    return bvh;
}

}  // namespace starcore
