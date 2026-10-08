#pragma once
// Triangle meshes as shared, immutable assets.
//
// A MeshData is built once (from a model file, a primitive generator or user
// data) and then shared by every entity that shows it: MeshComponent holds a
// MeshHandle, copying an entity copies the handle, and the renderer/physics use
// the prebuilt BVH (the mesh's BLAS) without rebuilding anything per frame.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "starcore/math/bvh.hpp"
#include "starcore/math/geometry.hpp"
#include "starcore/math/types.hpp"

namespace starcore
{

struct MeshData
{
    std::vector<Vec3> positions;
    // One per position (smooth shading), or empty (flat shading).
    std::vector<Vec3> normals;
    // Three per triangle, all < positions.size().
    std::vector<std::uint32_t> indices;
    Aabb bounds;
    Bvh bvh;  // Over triangles (BLAS).

    [[nodiscard]] auto triangleCount() const noexcept -> std::size_t
    {
        return indices.size() / 3;
    }

    [[nodiscard]] auto vertex(std::size_t triangle, std::size_t corner) const -> const Vec3&
    {
        return positions[indices[triangle * 3 + corner]];
    }
};

using MeshHandle = std::shared_ptr<const MeshData>;

/**
 * @brief Validate the triangle list and build bounds and BVH.
 * @param normals Per-vertex normals or empty (flat shading). They are
 *        normalized; zero-length normals fall back to the face normal.
 * @return An error for out-of-range indices, an index count that is not a
 *         multiple of 3, or a normal count that does not match.
 */
[[nodiscard]] auto makeMesh(std::vector<Vec3> positions, std::vector<std::uint32_t> indices,
                            std::vector<Vec3> normals = {})
    -> std::expected<MeshHandle, std::string>;

/** @brief Area-weighted vertex normals of a triangle list. */
[[nodiscard]] auto computeSmoothNormals(std::span<const Vec3> positions,
                                        std::span<const std::uint32_t> indices)
    -> std::vector<Vec3>;

/**
 * @brief Incremental mesh construction (vertices with normals + triangles),
 *        with helpers to combine transformed parts into one mesh.
 */
class MeshBuilder
{
public:
    auto addVertex(Vec3 position, Vec3 normal) -> std::uint32_t;
    void addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c);
    /** @brief Two triangles (a, b, c) and (a, c, d). */
    void addQuad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d);

    /** @brief Append another builder's geometry, transformed. */
    void append(const MeshBuilder& other, const Mat4& transform = Mat4(1.0F));
    /** @brief Transform positions (and normals by the inverse transpose). */
    void transform(const Mat4& matrix);
    /** @brief Reverse the winding of every triangle and negate the normals. */
    void flip();

    [[nodiscard]] auto positions() const noexcept -> std::span<const Vec3>
    {
        return m_positions;
    }
    [[nodiscard]] auto normals() const noexcept -> std::span<const Vec3>
    {
        return m_normals;
    }
    [[nodiscard]] auto indices() const noexcept -> std::span<const std::uint32_t>
    {
        return m_indices;
    }
    [[nodiscard]] auto empty() const noexcept -> bool
    {
        return m_indices.empty();
    }

    [[nodiscard]] auto build() const -> MeshHandle;

private:
    std::vector<Vec3> m_positions;
    std::vector<Vec3> m_normals;
    std::vector<std::uint32_t> m_indices;
};

/** @brief Procedural meshes (counter-clockwise front faces, outward normals). */
namespace primitives
{

/** @brief Axis-aligned box centered at the origin (flat-shaded faces). */
[[nodiscard]] auto box(Vec3 halfExtents) -> MeshBuilder;

/** @brief Rectangle in the XZ plane facing +Y, centered at the origin. */
[[nodiscard]] auto plane(Vec2 size) -> MeshBuilder;

/** @brief Rectangle in the XY plane facing +Z, centered at the origin. */
[[nodiscard]] auto quad(Vec2 size) -> MeshBuilder;

/**
 * @brief Part of a sphere around the origin. `polar` is measured from +Y
 *        (0 = north pole, pi = south pole), `azimuth` around +Y starting at +X
 *        towards -Z. Full sphere by default.
 */
[[nodiscard]] auto sphere(float radius, std::uint32_t segments = 32, std::uint32_t rings = 16,
                          Vec2 polar = {0.0F, kPi}, Vec2 azimuth = {0.0F, kTwoPi})
    -> MeshBuilder;

/**
 * @brief Cylinder along Y centered at the origin; `azimuth` restricts it to a
 *        curved panel (no caps then).
 */
[[nodiscard]] auto cylinder(float radius, float height, std::uint32_t segments = 32,
                            bool caps = true, Vec2 azimuth = {0.0F, kTwoPi}) -> MeshBuilder;

/** @brief Torus around Y in the XZ plane. */
[[nodiscard]] auto torus(float majorRadius, float minorRadius, std::uint32_t segments = 32,
                         std::uint32_t sides = 16) -> MeshBuilder;

/**
 * @brief Prism from a convex or concave (simple, counter-clockwise) polygon in
 *        the XY plane, extruded from z = -depth/2 to +depth/2.
 */
[[nodiscard]] auto extrude(std::span<const Vec2> profile, float depth) -> MeshBuilder;

}  // namespace primitives

}  // namespace starcore
