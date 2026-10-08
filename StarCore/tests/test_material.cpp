// Path tracer materials and lighting, checked against analytic expectations.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <entt/entity/registry.hpp>

import std;
import glm;
import starcore.asset;
import starcore.math;
import starcore.render;
import starcore.scene;

using namespace starcore;
using Catch::Matchers::WithinRel;

namespace
{

/** @brief Mean radiance of `samples` paths along one ray (float, no 8-bit quantization). */
auto meanRadiance(const RenderScene& scene, const Ray& ray, const PathTracerSettings& settings,
                  std::uint32_t samples) -> Vec3
{
    Vec3 sum{0.0F};
    for (std::uint32_t i = 0; i < samples; ++i) {
        Rng rng = Rng::forPixel(i, 17, 3, settings.seed);
        sum += tracePath(scene, ray, settings, rng);
    }
    return sum / static_cast<float>(samples);
}

/** @brief Mean radiance over rays from a ring of viewpoints aimed at the origin. */
auto meanOverSphere(const RenderScene& scene, const PathTracerSettings& settings) -> float
{
    Rng rng(42);
    float sum = 0.0F;
    constexpr int kRays = 4000;
    for (int i = 0; i < kRays; ++i) {
        const Vec3 origin = rng.onUnitSphere() * 10.0F;
        const Vec3 target = rng.inUnitSphere() * 0.9F;  // Inside the unit sphere.
        Rng pathRng(static_cast<std::uint32_t>(i) * 7919U + 1U);
        sum += tracePath(scene, Ray{origin, glm::normalize(target - origin)}, settings, pathRng).g;
    }
    return sum / static_cast<float>(kRays);
}

auto furnaceScene(Scene& scene, const MaterialComponent& material) -> RenderScene
{
    scene.environment().skyColor = Vec3(0.5F);
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(1.0F);
    ball.add<MaterialComponent>(material);
    return buildRenderScene(scene);
}

/** @brief Floor (y = 0) lit by a sphere light of radius 1 and radiance 10 at height 5. */
auto lightTestScene(Scene& scene) -> void
{
    scene.environment().skyColor = Vec3(0.0F);
    Entity floor = scene.createEntity("Floor");
    floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(200.0F)).build()});
    floor.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.5F)});
    Entity light = scene.createEntity("Light");
    light.get<TransformComponent>().translation = {0.0F, 5.0F, 0.0F};
    light.add<SphereComponent>(1.0F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = 10.0F});
}

// A low ray hitting the floor at the origin (not passing near the light).
const Ray kFloorRay{{3.0F, 0.5F, 0.0F}, glm::normalize(Vec3{-3.0F, -0.5F, 0.0F})};
// Lambertian floor under a sphere: L = albedo * radiance * (r / d)^2.
constexpr float kExpectedFloor = 0.5F * 10.0F * (1.0F / 25.0F);

}  // namespace

TEST_CASE("Fresnel and GGX sampling building blocks", "[render][material]")
{
    using namespace sampling;
    REQUIRE_THAT(fresnelDielectric(1.0F, 1.0F / 1.5F), WithinRel(0.04F, 1e-4F));
    REQUIRE(fresnelDielectric(0.1F, 1.5F) == 1.0F);  // Total internal reflection.
    REQUIRE(fresnelDielectric(1.0F, 1.0F) == 0.0F);

    // Sampled visible normals lie in the upper hemisphere and face the viewer.
    Rng rng(3);
    for (int i = 0; i < 1000; ++i) {
        const Vec3 wo = glm::normalize(Vec3(rng.inUnitSphere().x, rng.inUnitSphere().y, 0.2F));
        const Vec3 h = sampleGgxVisibleNormal(wo, 0.5F, rng.nextFloat(), rng.nextFloat());
        REQUIRE(h.z > 0.0F);
        REQUIRE(glm::dot(wo, h) > -1e-4F);
        REQUIRE_THAT(glm::length(h), WithinRel(1.0F, 1e-4F));
    }
    // The cone covers exactly the sphere's solid angle.
    const float oneMinusCos = coneOneMinusCos(1.0F, 25.0F);
    REQUIRE_THAT(kTwoPi * oneMinusCos, WithinRel(kTwoPi * (1.0F - std::sqrt(24.0F / 25.0F)), 1e-4F));
}

TEST_CASE("VNDF sampling matches brute-force integration of the GGX BRDF",
          "[render][material]")
{
    using namespace sampling;
    // Directional albedo E(wo) = integral of D G2 / (4 cos_o cos_i) * cos_i (F = 1),
    // once with VNDF importance sampling (the tracer's estimator: G2 / G1 for
    // directions above the surface) and once with uniform hemisphere samples.
    for (const float alpha : {0.3F, 0.7F, 1.0F}) {
        for (const float cosO : {0.9F, 0.5F, 0.2F}) {
            const Vec3 wo{std::sqrt(1.0F - cosO * cosO), 0.0F, cosO};
            Rng rng(11);
            double vndf = 0.0;
            constexpr int kVndfSamples = 200000;
            for (int i = 0; i < kVndfSamples; ++i) {
                const Vec3 h = sampleGgxVisibleNormal(wo, alpha, rng.nextFloat(), rng.nextFloat());
                const Vec3 wi = 2.0F * glm::dot(wo, h) * h - wo;
                if (wi.z > 0.0F) {
                    vndf += smithWeight(wo, wi, alpha);
                }
            }
            vndf /= kVndfSamples;

            double uniform = 0.0;
            constexpr int kUniformSamples = 2000000;
            for (int i = 0; i < kUniformSamples; ++i) {
                Vec3 wi = rng.onUnitSphere();
                wi.z = std::abs(wi.z);
                const Vec3 h = glm::normalize(wo + wi);
                const float a2 = alpha * alpha;
                const float denominator = h.z * h.z * (a2 - 1.0F) + 1.0F;
                const float d = a2 / (kPi * denominator * denominator);
                const float g2 =
                    1.0F / (1.0F + smithLambda(wo, alpha) + smithLambda(wi, alpha));
                // f * cos_i / pdf, pdf = 1 / (2 pi).
                uniform += d * g2 / (4.0F * wo.z) * kTwoPi;
            }
            uniform /= kUniformSamples;
            INFO("alpha " << alpha << ", cos " << cosO << ": vndf " << vndf << ", uniform "
                          << uniform);
            REQUIRE_THAT(vndf, Catch::Matchers::WithinRel(uniform, 0.02));
            REQUIRE(vndf <= 1.0);
        }
    }
}

TEST_CASE("White furnace: materials do not create energy", "[render][material]")
{
    PathTracerSettings settings;
    settings.maxBounces = 32;

    SECTION("smooth glass volume and thin sheets conserve energy")
    {
        for (const bool thin : {false, true}) {
            Scene scene;
            const RenderScene renderScene = furnaceScene(
                scene, MaterialComponent{.albedo = Vec3(1.0F), .roughness = 0.0F,
                                         .transmission = 1.0F, .thinWalled = thin});
            REQUIRE_THAT(meanOverSphere(renderScene, settings), WithinRel(0.5F, 0.01F));
        }
    }
    SECTION("white rough metal, plastic and glass are invisible")
    {
        // Multiple scattering is approximated energy-conservingly, so white
        // surfaces neither darken (single-scattering GGX loses up to ~60 % at
        // roughness 1) nor brighten.
        for (const float metallic : {0.0F, 1.0F}) {
            for (const float roughness : {0.1F, 0.5F, 1.0F}) {
                for (const float transmission : {0.0F, 1.0F}) {
                    Scene scene;
                    const RenderScene renderScene = furnaceScene(
                        scene, MaterialComponent{.albedo = Vec3(1.0F), .roughness = roughness,
                                                 .metallic = metallic,
                                                 .transmission = transmission});
                    REQUIRE_THAT(meanOverSphere(renderScene, settings), WithinRel(0.5F, 0.01F));
                }
            }
        }
    }
}

TEST_CASE("Sphere lights: NEE and BSDF sampling agree with the analytic result",
          "[render][material][light]")
{
    Scene scene;
    lightTestScene(scene);
    const RenderScene renderScene = buildRenderScene(scene);
    REQUIRE(renderScene.lights.size() == 1);

    PathTracerSettings settings;
    settings.maxBounces = 2;  // Direct light only.
    const Vec3 withNee = meanRadiance(renderScene, kFloorRay, settings, 4000);
    REQUIRE_THAT(withNee.r, WithinRel(kExpectedFloor, 0.03F));

    settings.nextEventEstimation = false;
    const Vec3 bsdfOnly = meanRadiance(renderScene, kFloorRay, settings, 60000);
    REQUIRE_THAT(bsdfOnly.r, WithinRel(kExpectedFloor, 0.06F));

    // The firefly clamp never touches direct light (by either strategy).
    settings.fireflyClamp = 0.01F;
    REQUIRE_THAT(meanRadiance(renderScene, kFloorRay, settings, 60000).r,
                 WithinRel(kExpectedFloor, 0.06F));
    settings.nextEventEstimation = true;
    REQUIRE(meanRadiance(renderScene, kFloorRay, settings, 4000) == withNee);

    // ... but clamps light that bounced twice (floor -> mirror wall -> light).
    Entity wall = scene.createEntity("Wall");
    wall.get<TransformComponent>().translation = {-2.0F, 0.0F, 0.0F};
    wall.get<TransformComponent>().rotation = glm::angleAxis(-kPi / 2.0F, Vec3{0, 0, 1});
    wall.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
    wall.add<MaterialComponent>(
        MaterialComponent{.albedo = Vec3(1.0F), .roughness = 0.0F, .metallic = 1.0F});
    const RenderScene withWall = buildRenderScene(scene);
    settings.maxBounces = 3;
    settings.fireflyClamp = 0.0F;
    const float unclamped = meanRadiance(withWall, kFloorRay, settings, 20000).r;
    settings.fireflyClamp = 0.01F;
    const float clamped = meanRadiance(withWall, kFloorRay, settings, 20000).r;
    REQUIRE(unclamped > kExpectedFloor * 1.2F);  // The mirror adds a caustic.
    REQUIRE(clamped < unclamped * 0.95F);
    REQUIRE(clamped > kExpectedFloor * 0.95F);
}

TEST_CASE("Thin glass lets light through without double counting", "[render][material][light]")
{
    Scene scene;
    lightTestScene(scene);
    Entity pane = scene.createEntity("Pane");
    pane.get<TransformComponent>().translation = {0.0F, 2.5F, 0.0F};
    pane.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
    pane.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    const RenderScene renderScene = buildRenderScene(scene);

    // Fresnel at near-normal incidence reflects ~4 % per sheet.
    const float expected = kExpectedFloor * (1.0F - 0.04F);
    PathTracerSettings settings;
    settings.maxBounces = 3;  // Floor, pane, light.
    REQUIRE_THAT(meanRadiance(renderScene, kFloorRay, settings, 6000).r,
                 WithinRel(expected, 0.04F));
    settings.nextEventEstimation = false;
    REQUIRE_THAT(meanRadiance(renderScene, kFloorRay, settings, 80000).r,
                 WithinRel(expected, 0.07F));

    // An opaque black sheet blocks the light.
    pane.patch<MaterialComponent>([](auto& m) { m = MaterialComponent{.albedo = Vec3(0.0F)}; });
    settings.nextEventEstimation = true;
    REQUIRE(meanRadiance(buildRenderScene(scene), kFloorRay, settings, 2000).r < 1e-3F);
}

TEST_CASE("Mirrors reflect, primary ids look through glass", "[render][material]")
{
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity mirror = scene.createEntity("Mirror");
    mirror.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(50.0F)).build()});
    mirror.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .metallic = 1.0F});
    Entity lamp = scene.createEntity("Lamp");
    lamp.get<TransformComponent>().translation = {0.0F, 3.0F, -6.0F};
    lamp.add<SphereComponent>(1.0F);
    lamp.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = {1.0F, 0.5F, 0.25F}, .emissionPower = 2.0F});
    Entity window = scene.createEntity("Window");
    window.get<TransformComponent>().translation = {0.0F, 3.0F, 3.0F};
    window.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(4.0F)).build()});
    window.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    const RenderScene renderScene = buildRenderScene(scene);

    // Looking down at the mirror towards the reflection of the lamp (at y = -3).
    const Vec3 eye{0.0F, 3.0F, 6.0F};
    const Ray ray{eye, glm::normalize(Vec3{0.0F, -3.0F, -6.0F} - eye)};
    PathTracerSettings settings;
    // Through the window (~4 % reflected), off the mirror (white: F = 1), onto the lamp.
    const Vec3 reflected = meanRadiance(renderScene, ray, settings, 4000);
    REQUIRE(glm::distance(reflected, Vec3{2.0F, 1.0F, 0.5F} * 0.959F) < 0.04F);
    std::uint32_t id = 0;
    Rng rng(1);
    (void) tracePath(renderScene, ray, settings, rng, &id);
    REQUIRE(id == entt::to_integral(mirror.handle()));  // Not the window.

    Camera camera;
    camera.setViewport(64, 64);
    camera.setPosition(eye);
    camera.lookAt(Vec3{0.0F, -3.0F, -6.0F});
    REQUIRE(pickEntity(renderScene, camera, nullptr, {32.0F, 32.0F}) == mirror.handle());
}

TEST_CASE("Preview mode is deterministic and shows lights and shadows", "[render][material]")
{
    Scene scene;
    lightTestScene(scene);
    const RenderScene lit = buildRenderScene(scene);
    PathTracerSettings settings;
    settings.mode = RenderMode::Preview;

    const Vec3 a = shadePreview(lit, kFloorRay, settings);
    REQUIRE(a == shadePreview(lit, kFloorRay, settings));
    // Direct light term = the analytic result; plus ambient and a little headlight.
    REQUIRE(a.r > kExpectedFloor);
    REQUIRE(a.r < kExpectedFloor + 0.5F * (0.04F + 0.15F));

    Entity blocker = scene.createEntity("Blocker");
    blocker.get<TransformComponent>().translation = {0.0F, 2.5F, 0.0F};
    blocker.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(3.0F)).build()});
    const Vec3 shadowed = shadePreview(buildRenderScene(scene), kFloorRay, settings);
    REQUIRE(shadowed.r < a.r - 0.9F * kExpectedFloor);
}
