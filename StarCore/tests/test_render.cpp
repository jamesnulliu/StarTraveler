#include <algorithm>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/geometric.hpp>

#include <starcore/math/color.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/render/overlay.hpp>
#include <starcore/render/path_tracer.hpp>
#include <starcore/render/render_scene.hpp>
#include <starcore/scene/entity.hpp>

using namespace starcore;
using Catch::Matchers::WithinAbs;

TEST_CASE("Camera projection round-trips", "[render][camera]")
{
    Camera camera(60.0F);
    camera.setViewport(320, 200);
    camera.setPosition({1.0F, 2.0F, 3.0F});
    camera.setRotation(-60.0F, 20.0F);

    const Ray center = camera.generateRay({160.0F, 100.0F});
    REQUIRE(glm::distance(center.direction, camera.forward()) < 1e-5F);

    for (const Vec2 pixel : {Vec2{0, 0}, Vec2{319.5F, 12.25F}, Vec2{40, 190}}) {
        for (const float depth : {0.5F, 7.0F, 80.0F}) {
            const Vec3 world = camera.screenToWorld(pixel, depth);
            REQUIRE_THAT(camera.depthOf(world), WithinAbs(depth, 1e-3));
            const auto screen = camera.worldToScreen(world);
            REQUIRE(screen.has_value());
            REQUIRE(glm::distance(*screen, pixel) < 1e-2F);
        }
    }
    // +y is down in pixel space.
    REQUIRE(camera.worldToScreen(camera.position() + camera.forward() * 5.0F +
                                 camera.up())->y < 100.0F);
    REQUIRE_FALSE(camera.worldToScreen(camera.position() - camera.forward()));
}

TEST_CASE("Camera lookAt and default orientation", "[render][camera]")
{
    Camera camera;
    REQUIRE(glm::distance(camera.forward(), Vec3{0, 0, -1}) < 1e-6F);
    camera.setPosition({0, 0, 0});
    camera.lookAt({5, 5, 0});
    REQUIRE(glm::distance(camera.forward(), glm::normalize(Vec3{1, 1, 0})) < 1e-5F);
}

TEST_CASE("Fly camera moves diagonally at a frame-rate independent speed",
          "[render][camera]")
{
    FlyCameraController controller;
    controller.acceleration = 0.0F;  // Constant speed for this test.

    Camera a;
    Camera b;
    CameraInput input{.forward = true, .right = true, .look = true};
    controller.update(a, input, 0.5F);
    controller.update(b, input, 0.25F);
    controller.update(b, input, 0.25F);
    REQUIRE(glm::distance(a.position(), b.position()) < 1e-5F);

    // Both keys at once (the legacy else-if chain ignored the second one).
    const Vec3 moved = a.position() - Vec3{0, 0, 10};
    REQUIRE(moved.x > 0.0F);
    REQUIRE(moved.z < 0.0F);
    REQUIRE_THAT(glm::length(moved), WithinAbs(controller.baseSpeed * 0.5F, 1e-4));

    // Nothing happens without the look button.
    Camera c;
    REQUIRE_FALSE(controller.update(c, CameraInput{.forward = true}, 1.0F));
}

namespace
{

auto makeSphereScene(Scene& scene) -> Entity
{
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(2.0F);
    ball.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = {1.0F, 0.5F, 0.25F},
        .emissionPower = 1.0F});
    scene.environment().skyColor = {0.0F, 0.0F, 1.0F};
    return ball;
}

}  // namespace

TEST_CASE("RenderScene and closest-hit queries", "[render][trace]")
{
    Scene scene;
    Entity ball = makeSphereScene(scene);
    ball.get<TransformComponent>().scale = {1.0F, 1.0F, 2.0F};  // Ellipsoid.

    Entity triangle = scene.createEntity("Triangle");
    triangle.get<TransformComponent>().translation = {10.0F, 0.0F, 0.0F};
    triangle.add<MeshComponent>(MeshComponent{
        {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}}, {0, 1, 2}});

    Entity broken = scene.createEntity("Broken");
    broken.add<MeshComponent>(MeshComponent{{{0, 0, 0}}, {0, 1, 2}});

    const RenderScene renderScene = buildRenderScene(scene);
    REQUIRE(renderScene.instances.size() == 2);  // Broken mesh dropped.
    REQUIRE(renderScene.sceneVersion == scene.version());

    SECTION("sphere hit honors radius and non-uniform scale")
    {
        const auto hit = traceClosestHit(
            renderScene, Ray{{0, 0, 10}, {0, 0, -1}}, 0.0F, kInfinity);
        REQUIRE(hit.has_value());
        REQUIRE_THAT(hit->t, WithinAbs(6.0, 1e-4));  // z radius = 2 * 2.
        REQUIRE(glm::distance(hit->normal, Vec3{0, 0, 1}) < 1e-4F);
        REQUIRE(renderScene.instances[hit->instanceIndex].entityId ==
                entt::to_integral(ball.handle()));
    }
    SECTION("triangle normals face the incoming ray from both sides")
    {
        for (const float side : {1.0F, -1.0F}) {
            const auto hit = traceClosestHit(
                renderScene, Ray{{10, 0, 5 * side}, {0, 0, -side}}, 0.0F, kInfinity);
            REQUIRE(hit.has_value());
            REQUIRE_THAT(hit->normal.z, WithinAbs(side, 1e-5));
        }
    }
    SECTION("misses")
    {
        REQUIRE_FALSE(traceClosestHit(renderScene, Ray{{0, 50, 10}, {0, 0, -1}},
                                      0.0F, kInfinity));
    }
}

TEST_CASE("hashRenderScene ignores non-traced changes", "[render][trace]")
{
    Scene scene;
    Entity ball = makeSphereScene(scene);
    Entity line = scene.createEntity("Line");
    line.add<LineShapeComponent>();

    const auto base = hashRenderScene(buildRenderScene(scene));
    line.patch<LineShapeComponent>([](auto& l) { l.end.x = 5.0F; });
    line.patch<NameComponent>([](auto& n) { n.name = "Renamed"; });
    REQUIRE(hashRenderScene(buildRenderScene(scene)) == base);

    ball.patch<SphereComponent>([](auto& s) { s.radius = 3.0F; });
    REQUIRE(hashRenderScene(buildRenderScene(scene)) != base);
}

TEST_CASE("tracePath sees emission and sky", "[render][trace]")
{
    Scene scene;
    makeSphereScene(scene);
    const RenderScene renderScene = buildRenderScene(scene);
    PathTracerSettings settings;
    Rng rng(1);

    std::uint32_t entityId = 0;
    const Vec3 direct = tracePath(renderScene, Ray{{0, 0, 10}, {0, 0, -1}},
                                  settings, rng, &entityId);
    // Black albedo: exactly the emission, nothing else.
    REQUIRE(direct == Vec3{1.0F, 0.5F, 0.25F});
    REQUIRE(entityId != kNoEntityId);

    const Vec3 sky = tracePath(renderScene, Ray{{0, 0, 10}, {0, 0, 1}},
                               settings, rng, &entityId);
    REQUIRE(sky == Vec3{0.0F, 0.0F, 1.0F});
    REQUIRE(entityId == kNoEntityId);
}

TEST_CASE("CPU path tracer is deterministic across thread counts",
          "[render][trace]")
{
    Scene scene;
    makeSphereScene(scene);
    Entity floor = scene.createEntity("Floor");
    floor.get<TransformComponent>().translation = {0.0F, -102.0F, 0.0F};
    floor.add<SphereComponent>(100.0F);
    floor.add<MaterialComponent>();
    const RenderScene renderScene = buildRenderScene(scene);

    Camera camera;
    camera.setViewport(48, 32);
    PathTracerSettings settings;
    settings.samplesPerPixel = 2;

    CpuPathTracer single(1);
    CpuPathTracer multi(8);
    single.resize(48, 32);
    multi.resize(48, 32);
    for (int frame = 0; frame < 3; ++frame) {
        single.render(renderScene, camera, settings);
        multi.render(renderScene, camera, settings);
    }
    REQUIRE(single.accumulatedFrames() == 3);
    REQUIRE(std::ranges::equal(single.image().pixels(), multi.image().pixels()));
    REQUIRE(std::ranges::equal(single.entityIds().pixels(), multi.entityIds().pixels()));

    // Center pixel shows the emissive ball, corners show sky or floor.
    REQUIRE(single.entityIds().at(24, 16) == entt::to_integral(scene.findEntityByName("Ball").handle()));

    SECTION("accumulation restarts when the camera moves")
    {
        camera.setPosition({0.0F, 0.0F, 9.0F});
        single.render(renderScene, camera, settings);
        REQUIRE(single.accumulatedFrames() == 1);
    }
    SECTION("accumulation restarts when the scene changes")
    {
        scene.markDirty();
        single.render(buildRenderScene(scene), camera, settings);
        REQUIRE(single.accumulatedFrames() == 1);
    }
}

TEST_CASE("Overlay draws shapes and supports picking", "[render][overlay]")
{
    Scene scene;
    Entity line = scene.createEntity("Line");
    line.add<LineShapeComponent>(LineShapeComponent{{-2, 0, 0}, {2, 0, 0}});
    line.add<StrokeComponent>(StrokeComponent{.color = {1, 0, 0, 1}, .thickness = 3});

    Entity square = scene.createEntity("Square");
    square.get<TransformComponent>().translation = {0.0F, 2.0F, 0.0F};
    square.add<PolygonShapeComponent>(PolygonShapeComponent{
        .vertices = {{-0.5F, -0.5F, 0}, {0.5F, -0.5F, 0}, {0.5F, 0.5F, 0}, {-0.5F, 0.5F, 0}},
        .fillColor = {0, 1, 0, 1}});

    Entity behind = scene.createEntity("Behind");
    behind.add<LineShapeComponent>(LineShapeComponent{{-5, 0, 20}, {5, 0, 20}});

    Camera camera;  // At (0, 0, 10) looking down -Z.
    camera.setViewport(200, 200);
    ColorImage color(200, 200, 0U);
    IdImage ids(200, 200, kNoEntityId);
    renderOverlay(scene, camera, color, &ids);

    const auto lineId = entt::to_integral(line.handle());
    const auto squareId = entt::to_integral(square.handle());
    REQUIRE(color.at(100, 100) == packRgba8({1, 0, 0, 1}));
    REQUIRE(ids.at(100, 100) == lineId);
    REQUIRE(ids.at(100, 99) == lineId);  // Thickness 3.

    const auto squareCenter = camera.worldToScreen({0.0F, 2.0F, 0.0F});
    REQUIRE(squareCenter.has_value());
    const IVec2 sc{static_cast<int>(squareCenter->x), static_cast<int>(squareCenter->y)};
    REQUIRE(color.at(sc.x, sc.y) == packRgba8({0, 1, 0, 1}));

    // The line behind the camera draws nothing: only line + square pixels.
    const auto touched = std::ranges::count_if(
        ids.pixels(), [&](std::uint32_t id) { return id != kNoEntityId; });
    const auto lineOrSquare = std::ranges::count_if(
        ids.pixels(), [&](std::uint32_t id) { return id == lineId || id == squareId; });
    REQUIRE(touched == lineOrSquare);

    const RenderScene renderScene = buildRenderScene(scene);
    REQUIRE(pickEntity(renderScene, camera, &ids, {100.5F, 100.5F}) == line.handle());
    REQUIRE_FALSE(pickEntity(renderScene, camera, &ids, {5.5F, 5.5F}).has_value());
}
