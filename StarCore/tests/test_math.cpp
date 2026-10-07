#include <cmath>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <starcore/math/color.hpp>
#include <starcore/math/geometry.hpp>
#include <starcore/math/random.hpp>
#include <starcore/math/transform.hpp>

using namespace starcore;
using Catch::Matchers::WithinAbs;

TEST_CASE("packRgba8 clamps, rounds and keeps alpha", "[math][color]")
{
    // Legacy RGBA_to_Uint32 let 2.0 overflow into the next channel and
    // forced alpha to 255.
    REQUIRE(packRgba8({2.0F, -1.0F, 0.5F, 0.0F}) == 0x008000FFU);
    REQUIRE(packRgba8({1.0F, 1.0F, 1.0F, 1.0F}) == 0xFFFFFFFFU);

    const Vec4 color{0.2F, 0.4F, 0.6F, 0.8F};
    const Vec4 roundTrip = unpackRgba8(packRgba8(color));
    for (int i = 0; i < 4; ++i) {
        REQUIRE_THAT(roundTrip[i], WithinAbs(color[i], 1.0 / 255.0));
    }
}

TEST_CASE("sRGB conversion round-trips", "[math][color]")
{
    for (const float value : {0.0F, 0.001F, 0.18F, 0.5F, 1.0F}) {
        REQUIRE_THAT(srgbToLinear(linearToSrgb(value)), WithinAbs(value, 1e-5));
    }
}

TEST_CASE("Ray-sphere intersection", "[math][geometry]")
{
    const Ray ray{{0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, -1.0F}};

    SECTION("hit from outside returns the near root")
    {
        const auto t = intersectSphere(ray, Vec3(0.0F), 2.0F, 0.0F, kInfinity);
        REQUIRE(t.has_value());
        REQUIRE_THAT(*t, WithinAbs(8.0, 1e-5));
    }
    SECTION("origin inside the sphere returns the far root")
    {
        const Ray inside{Vec3(0.0F), {1.0F, 0.0F, 0.0F}};
        const auto t = intersectSphere(inside, Vec3(0.0F), 3.0F, 0.0F, kInfinity);
        REQUIRE(t.has_value());
        REQUIRE_THAT(*t, WithinAbs(3.0, 1e-5));
    }
    SECTION("miss and range limits")
    {
        REQUIRE_FALSE(intersectSphere(ray, {5.0F, 0.0F, 0.0F}, 1.0F, 0.0F, kInfinity));
        REQUIRE_FALSE(intersectSphere(ray, Vec3(0.0F), 2.0F, 0.0F, 7.0F));
    }
    SECTION("unnormalized direction scales t")
    {
        const Ray scaled{ray.origin, ray.direction * 2.0F};
        const auto t = intersectSphere(scaled, Vec3(0.0F), 2.0F, 0.0F, kInfinity);
        REQUIRE(t.has_value());
        REQUIRE_THAT(*t, WithinAbs(4.0, 1e-5));
    }
}

TEST_CASE("Ray-triangle intersection is double-sided", "[math][geometry]")
{
    const Vec3 v0{-1.0F, -1.0F, 0.0F};
    const Vec3 v1{1.0F, -1.0F, 0.0F};
    const Vec3 v2{0.0F, 1.0F, 0.0F};

    const Ray front{{0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -1.0F}};
    const Ray back{{0.0F, 0.0F, -5.0F}, {0.0F, 0.0F, 1.0F}};
    const auto hitFront = intersectTriangle(front, v0, v1, v2, 0.0F, kInfinity);
    const auto hitBack = intersectTriangle(back, v0, v1, v2, 0.0F, kInfinity);
    REQUIRE(hitFront.has_value());
    REQUIRE(hitBack.has_value());
    REQUIRE_THAT(hitFront->t, WithinAbs(5.0, 1e-5));
    REQUIRE_THAT(hitBack->t, WithinAbs(5.0, 1e-5));

    const Ray outside{{3.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -1.0F}};
    REQUIRE_FALSE(intersectTriangle(outside, v0, v1, v2, 0.0F, kInfinity));
    const Ray parallel{{0.0F, 0.0F, 5.0F}, {1.0F, 0.0F, 0.0F}};
    REQUIRE_FALSE(intersectTriangle(parallel, v0, v1, v2, 0.0F, kInfinity));
}

TEST_CASE("AABB slab test and transform", "[math][geometry]")
{
    const Aabb box{Vec3(-1.0F), Vec3(1.0F)};
    const Ray ray{{0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -1.0F}};
    REQUIRE(intersectAabb(ray, 1.0F / ray.direction, box, 0.0F, kInfinity));
    REQUIRE_FALSE(intersectAabb(ray, 1.0F / ray.direction, box, 0.0F, 3.0F));

    const Ray axisParallelMiss{{2.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -1.0F}};
    REQUIRE_FALSE(intersectAabb(axisParallelMiss, 1.0F / axisParallelMiss.direction,
                                box, 0.0F, kInfinity));

    const Mat4 transform =
        composeTransform({10.0F, 0.0F, 0.0F}, Quat(1.0F, 0.0F, 0.0F, 0.0F), Vec3(2.0F));
    const Aabb moved = box.transformed(transform);
    REQUIRE_THAT(moved.min.x, WithinAbs(8.0, 1e-5));
    REQUIRE_THAT(moved.max.x, WithinAbs(12.0, 1e-5));
}

TEST_CASE("PCG RNG is deterministic and well-formed", "[math][random]")
{
    Rng a(42);
    Rng b(42);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(a.nextUint() == b.nextUint());
    }

    Rng rng(7);
    for (int i = 0; i < 10000; ++i) {
        const float f = rng.nextFloat();
        REQUIRE(f >= 0.0F);
        REQUIRE(f < 1.0F);
        REQUIRE_THAT(glm::length(rng.onUnitSphere()), WithinAbs(1.0, 1e-4));
        const Vec3 n{0.0F, 1.0F, 0.0F};
        REQUIRE(glm::dot(rng.cosineHemisphere(n), n) >= 0.0F);
    }

    // Legacy seeding (pixelIndex * frameIndex) gave pixel 0 the same stream
    // every frame; forPixel streams differ per pixel and frame.
    std::set<std::uint32_t> firstValues;
    for (std::uint32_t frame = 0; frame < 4; ++frame) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            firstValues.insert(Rng::forPixel(x, 0, frame).nextUint());
        }
    }
    REQUIRE(firstValues.size() == 16);
}

TEST_CASE("Transform compose/decompose round-trips", "[math][transform]")
{
    const Vec3 translation{1.0F, -2.0F, 3.0F};
    const Quat rotation = glm::normalize(Quat(0.9F, 0.1F, -0.3F, 0.2F));
    const Vec3 scale{2.0F, 0.5F, 1.5F};

    const auto result =
        decomposeTransform(composeTransform(translation, rotation, scale));
    for (int i = 0; i < 3; ++i) {
        REQUIRE_THAT(result.translation[i], WithinAbs(translation[i], 1e-5));
        REQUIRE_THAT(result.scale[i], WithinAbs(scale[i], 1e-5));
    }
    // q and -q are the same rotation.
    REQUIRE_THAT(std::abs(glm::dot(result.rotation, rotation)), WithinAbs(1.0, 1e-5));
}

TEST_CASE("rotationBetween handles axis-aligned and opposite vectors",
          "[math][transform]")
{
    // Legacy GetRotationQuat returned identity for any direction with x=y=0.
    const Quat q = rotationBetween({1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    const Vec3 rotated = q * Vec3{1.0F, 0.0F, 0.0F};
    REQUIRE_THAT(rotated.z, WithinAbs(-1.0, 1e-5));

    const Quat flip = rotationBetween({0.0F, 1.0F, 0.0F}, {0.0F, -3.0F, 0.0F});
    REQUIRE_THAT((flip * Vec3{0.0F, 1.0F, 0.0F}).y, WithinAbs(-1.0, 1e-5));

    const Quat identity = rotationBetween(Vec3(0.0F), {1.0F, 0.0F, 0.0F});
    REQUIRE(identity == Quat(1.0F, 0.0F, 0.0F, 0.0F));
}
