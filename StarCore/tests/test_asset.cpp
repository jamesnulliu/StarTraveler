#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

import std;
import glm;
import starcore.asset;
import starcore.math;

using namespace starcore;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{

/** @brief Random triangle soup in [-10, 10]^3 (small triangles, like a real mesh). */
auto randomTriangles(std::uint32_t count, std::uint32_t seed) -> MeshHandle
{
    Rng rng(seed);
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> indices;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Vec3 center = (Vec3(rng.nextFloat(), rng.nextFloat(), rng.nextFloat()) - 0.5F) *
                            20.0F;
        for (int k = 0; k < 3; ++k) {
            indices.push_back(static_cast<std::uint32_t>(positions.size()));
            positions.push_back(center + rng.onUnitSphere() * 0.7F);
        }
    }
    return makeMesh(std::move(positions), std::move(indices)).value();
}

auto bruteForceClosest(const MeshData& mesh, const Ray& ray) -> float
{
    float closest = kInfinity;
    for (std::size_t i = 0; i < mesh.triangleCount(); ++i) {
        if (const auto hit = intersectTriangle(ray, mesh.vertex(i, 0), mesh.vertex(i, 1),
                                               mesh.vertex(i, 2), 0.0F, closest)) {
            closest = hit->t;
        }
    }
    return closest;
}

auto triangleArea(const MeshData& mesh, std::size_t i) -> float
{
    return 0.5F * glm::length(glm::cross(mesh.vertex(i, 1) - mesh.vertex(i, 0),
                                         mesh.vertex(i, 2) - mesh.vertex(i, 0)));
}

/**
 * @brief Every face is wound counter-clockwise around its outward normal:
 *        the face normal agrees with the vertex normals and points away from
 *        `inside` (for convex shapes around it).
 */
void requireOutwardFaces(const MeshData& mesh, Vec3 inside)
{
    REQUIRE(mesh.normals.size() == mesh.positions.size());
    for (std::size_t i = 0; i < mesh.triangleCount(); ++i) {
        if (triangleArea(mesh, i) < 1e-7F) {
            continue;
        }
        const Vec3 face = glm::cross(mesh.vertex(i, 1) - mesh.vertex(i, 0),
                                     mesh.vertex(i, 2) - mesh.vertex(i, 0));
        const Vec3 centroid = (mesh.vertex(i, 0) + mesh.vertex(i, 1) + mesh.vertex(i, 2)) / 3.0F;
        REQUIRE(glm::dot(face, centroid - inside) > 0.0F);
        for (std::size_t k = 0; k < 3; ++k) {
            REQUIRE(glm::dot(face, mesh.normals[mesh.indices[i * 3 + k]]) > 0.0F);
        }
    }
}

auto totalArea(const MeshData& mesh) -> float
{
    float area = 0.0F;
    for (std::size_t i = 0; i < mesh.triangleCount(); ++i) {
        area += triangleArea(mesh, i);
    }
    return area;
}

}  // namespace

TEST_CASE("BVH closest hit matches brute force", "[asset][bvh]")
{
    const MeshHandle mesh = randomTriangles(500, 7);
    REQUIRE(mesh->bvh.nodes().size() > 1);
    REQUIRE(mesh->bvh.primitives().size() == mesh->triangleCount());

    Rng rng(99);
    int hits = 0;
    for (int i = 0; i < 400; ++i) {
        const Ray ray{rng.onUnitSphere() * 25.0F, Vec3(0.0F)};
        Ray aimed = ray;
        aimed.direction = glm::normalize(rng.inUnitSphere() * 8.0F - ray.origin);

        float t = kInfinity;
        mesh->bvh.traverse(aimed, 0.0F, t, [&](std::uint32_t triangle, float& closest) {
            if (const auto hit = intersectTriangle(aimed, mesh->vertex(triangle, 0),
                                                   mesh->vertex(triangle, 1),
                                                   mesh->vertex(triangle, 2), 0.0F, closest)) {
                closest = hit->t;
            }
        });
        const float expected = bruteForceClosest(*mesh, aimed);
        REQUIRE(t == expected);

        const bool any = mesh->bvh.traverseAny(aimed, 0.0F, kInfinity, [&](std::uint32_t tri) {
            return intersectTriangle(aimed, mesh->vertex(tri, 0), mesh->vertex(tri, 1),
                                     mesh->vertex(tri, 2), 0.0F, kInfinity)
                .has_value();
        });
        REQUIRE(any == (expected != kInfinity));
        hits += expected != kInfinity ? 1 : 0;
    }
    REQUIRE(hits > 40);  // The test actually exercised hits and misses.
    REQUIRE(hits < 360);
}

TEST_CASE("BVH overlap queries match brute force", "[asset][bvh]")
{
    const MeshHandle mesh = randomTriangles(300, 11);
    Rng rng(5);
    for (int i = 0; i < 100; ++i) {
        const Vec3 center = rng.inUnitSphere() * 10.0F;
        const Aabb box{center - Vec3(1.5F), center + Vec3(1.5F)};
        std::vector<std::uint32_t> found;
        mesh->bvh.query(box, [&](std::uint32_t triangle) { found.push_back(triangle); });
        std::vector<std::uint32_t> expected;
        for (std::uint32_t t = 0; t < mesh->triangleCount(); ++t) {
            Aabb bounds;
            for (std::size_t k = 0; k < 3; ++k) {
                bounds.expand(mesh->vertex(t, k));
            }
            if (overlaps(bounds, box)) {
                expected.push_back(t);
            }
        }
        // Broadphase: every overlapping primitive is reported (plus leaf neighbours).
        std::ranges::sort(found);
        REQUIRE(std::ranges::includes(found, expected));
        REQUIRE(found.size() < mesh->triangleCount() / 4);
    }
}

TEST_CASE("BVH handles degenerate inputs", "[asset][bvh]")
{
    REQUIRE(Bvh::build({}).empty());

    // Many primitives with the same centroid still produce small leaves.
    const std::vector<Aabb> same(100, Aabb{Vec3(-1.0F), Vec3(1.0F)});
    const Bvh bvh = Bvh::build(same);
    for (const BvhNode& node : bvh.nodes()) {
        REQUIRE(node.count <= 4);
    }
    std::size_t visited = 0;
    bvh.query(Aabb{Vec3(0.0F), Vec3(0.5F)}, [&](std::uint32_t) { ++visited; });
    REQUIRE(visited == 100);
}

TEST_CASE("makeMesh validates its input", "[asset][mesh]")
{
    REQUIRE_FALSE(makeMesh({{0, 0, 0}}, {0, 1, 2}).has_value());
    REQUIRE_FALSE(makeMesh({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1}).has_value());
    REQUIRE_FALSE(makeMesh({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2}, {{0, 0, 1}}).has_value());

    // Zero normals fall back to computed ones; others are normalized.
    const auto mesh = makeMesh({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2},
                               {{0, 0, 0}, {0, 0, 5}, {0, 0, 1}});
    REQUIRE(mesh.has_value());
    for (const Vec3& normal : (*mesh)->normals) {
        REQUIRE(glm::distance(normal, Vec3{0, 0, 1}) < 1e-6F);
    }
    REQUIRE((*mesh)->bounds.max == Vec3{1, 1, 0});
}

TEST_CASE("Primitives are closed, outward-facing and correctly sized", "[asset][mesh]")
{
    SECTION("box")
    {
        const MeshHandle box = primitives::box({1.0F, 2.0F, 3.0F}).build();
        REQUIRE(box->triangleCount() == 12);
        requireOutwardFaces(*box, Vec3(0.0F));
        REQUIRE_THAT(totalArea(*box), WithinRel(2.0F * (2 * 4 + 4 * 6 + 2 * 6), 1e-5F));
        REQUIRE(box->bounds.max == Vec3{1, 2, 3});
    }
    SECTION("sphere")
    {
        const MeshHandle sphere = primitives::sphere(2.0F, 64, 32).build();
        requireOutwardFaces(*sphere, Vec3(0.0F));
        REQUIRE_THAT(totalArea(*sphere), WithinRel(4.0F * kPi * 4.0F, 0.01F));
    }
    SECTION("cylinder")
    {
        const MeshHandle cylinder = primitives::cylinder(1.0F, 3.0F, 64).build();
        requireOutwardFaces(*cylinder, Vec3(0.0F));
        REQUIRE_THAT(totalArea(*cylinder),
                     WithinRel(2.0F * kPi * 3.0F + 2.0F * kPi, 0.01F));
    }
    SECTION("torus")
    {
        const MeshHandle torus = primitives::torus(2.0F, 0.5F, 64, 32).build();
        REQUIRE_THAT(totalArea(*torus), WithinRel(4.0F * kPi * kPi * 2.0F * 0.5F, 0.01F));
        // Each face points away from the tube's center line.
        for (std::size_t i = 0; i < torus->triangleCount(); ++i) {
            const Vec3 centroid =
                (torus->vertex(i, 0) + torus->vertex(i, 1) + torus->vertex(i, 2)) / 3.0F;
            const Vec3 ring = glm::normalize(Vec3(centroid.x, 0.0F, centroid.z)) * 2.0F;
            const Vec3 face = glm::cross(torus->vertex(i, 1) - torus->vertex(i, 0),
                                         torus->vertex(i, 2) - torus->vertex(i, 0));
            REQUIRE(glm::dot(face, centroid - ring) > 0.0F);
        }
    }
    SECTION("extruded concave profile")
    {
        // L shape (area 3) extruded by 2.
        const std::vector<Vec2> profile{{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
        const MeshHandle prism = primitives::extrude(profile, 2.0F).build();
        const float perimeter = 8.0F;
        REQUIRE_THAT(totalArea(*prism), WithinRel(2.0F * 3.0F + perimeter * 2.0F, 1e-5F));
        // Clockwise input gives the same (outward) result.
        std::vector<Vec2> reversed(profile.rbegin(), profile.rend());
        REQUIRE_THAT(totalArea(*primitives::extrude(reversed, 2.0F).build()),
                     WithinRel(totalArea(*prism), 1e-5F));
    }
}

TEST_CASE("MeshBuilder transforms keep faces outward, also when mirrored", "[asset][mesh]")
{
    MeshBuilder builder;
    const Mat4 mirrored = glm::scale(glm::translate(Mat4(1.0F), Vec3{5, 0, 0}), Vec3{-1, 2, 1});
    builder.append(primitives::box(Vec3(0.5F)), mirrored);
    const MeshHandle mesh = builder.build();
    requireOutwardFaces(*mesh, Vec3{5, 0, 0});

    MeshBuilder flipped = primitives::sphere(1.0F, 16, 8);
    flipped.flip();
    const MeshHandle inward = flipped.build();
    for (std::size_t i = 0; i < inward->triangleCount(); ++i) {
        const Vec3 face = glm::cross(inward->vertex(i, 1) - inward->vertex(i, 0),
                                     inward->vertex(i, 2) - inward->vertex(i, 0));
        if (glm::length(face) > 1e-6F) {
            REQUIRE(glm::dot(face, inward->vertex(i, 0)) < 0.0F);
        }
    }
}
