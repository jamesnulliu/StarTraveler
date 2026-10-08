#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <entt/entity/registry.hpp>

import std;
import glm;
import starcore.asset;
import starcore.common;
import starcore.math;
import starcore.render;
import starcore.scene;

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
        .mesh = makeMesh({{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}}, {0, 1, 2}).value()});

    REQUIRE_FALSE(makeMesh({{0, 0, 0}}, {0, 1, 2}).has_value());  // Index out of range.
    Entity empty = scene.createEntity("Empty");
    empty.add<MeshComponent>();  // No mesh: skipped.

    const RenderScene renderScene = buildRenderScene(scene);
    REQUIRE(renderScene.instances.size() == 2);
    REQUIRE(renderScene.version != 0);
    REQUIRE(renderScene.tlasCurrent());

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

namespace
{

auto instanceOf(const RenderScene& renderScene, Entity entity) -> std::uint32_t
{
    for (std::uint32_t i = 0; i < renderScene.instances.size(); ++i) {
        if (renderScene.instances[i].entityId == entt::to_integral(entity.handle())) {
            return i;
        }
    }
    FAIL("no instance of " << entity.name());
    return 0;
}

/** @brief Same instances (by entity), transforms, materials and lights. */
void requireSameContent(const RenderScene& a, const RenderScene& b)
{
    REQUIRE(a.instances.size() == b.instances.size());
    REQUIRE(a.lights.size() == b.lights.size());
    REQUIRE(a.skyColor == b.skyColor);
    for (std::uint32_t i = 0; i < a.instances.size(); ++i) {
        const RenderInstance& instance = a.instances[i];
        const auto match = std::ranges::find_if(b.instances, [&](const RenderInstance& other) {
            return other.entityId == instance.entityId &&
                   a.geometries[instance.geometryIndex].kind ==
                       b.geometries[other.geometryIndex].kind;
        });
        REQUIRE(match != b.instances.end());
        REQUIRE(match->objectToWorld == instance.objectToWorld);
        REQUIRE(match->mask == instance.mask);
        REQUIRE(b.materials[match->materialIndex] == a.materials[instance.materialIndex]);
        REQUIRE(b.geometries[match->geometryIndex].mesh == a.geometries[instance.geometryIndex].mesh);
        const bool lightA = a.lightOfInstance[i] >= 0;
        const bool lightB =
            b.lightOfInstance[static_cast<std::size_t>(match - b.instances.begin())] >= 0;
        REQUIRE(lightA == lightB);
    }
}

}  // namespace

TEST_CASE("RenderSceneBuilder updates and stamps only what changed", "[render][trace]")
{
    Scene scene;
    Entity ball = makeSphereScene(scene);  // An emissive sphere: a light.
    Entity line = scene.createEntity("Line");
    line.add<LineShapeComponent>();
    Entity ship = scene.createEntity("Ship");
    ship.get<TransformComponent>().translation = {6.0F, 0.0F, 0.0F};
    Entity hull = scene.createEntity("Hull");
    hull.add<MeshComponent>(MeshComponent{.mesh = primitives::box(Vec3(0.5F)).build()});
    scene.setParent(hull, ship);
    hull.patch<TransformComponent>([](auto& t) { t.translation = Vec3(0.0F); });  // At the ship.

    RenderSceneBuilder builder;
    const RenderScene& renderScene = builder.update(scene);
    REQUIRE(renderScene.instances.size() == 2);
    REQUIRE(renderScene.lights.size() == 1);
    REQUIRE_FALSE(renderScene.tlasCurrent());  // Only built on request.
    const std::uint64_t first = renderScene.version;
    const std::uint64_t lineage = renderScene.lineage.value();
    REQUIRE(first != 0);
    REQUIRE(renderScene.contentVersion == first);

    SECTION("2D shapes and names change nothing traced")
    {
        line.patch<LineShapeComponent>([](auto& l) { l.end.x = 5.0F; });
        line.patch<NameComponent>([](auto& n) { n.name = "Renamed"; });
        builder.update(scene);
        REQUIRE(renderScene.version == first);
    }
    SECTION("moving a parent restamps the instances below it, as a transform change")
    {
        const std::uint32_t hullIndex = instanceOf(renderScene, hull);
        const std::uint32_t ballIndex = instanceOf(renderScene, ball);
        ship.patch<TransformComponent>([](auto& t) { t.translation.y = 2.0F; });
        builder.update(scene);
        REQUIRE(renderScene.version > first);
        REQUIRE(renderScene.contentVersion == first);
        REQUIRE(renderScene.instanceVersions[hullIndex] == renderScene.version);
        REQUIRE(renderScene.instanceVersions[ballIndex] == first);
        REQUIRE(renderScene.lineage.value() == lineage);

        // Without a current TLAS the queries test every instance.
        const Ray ray{{6.0F, 2.0F, 10.0F}, {0.0F, 0.0F, -1.0F}};
        const auto hit = traceClosestHit(renderScene, ray, 0.0F, kInfinity);
        REQUIRE(hit.has_value());
        REQUIRE(hit->instanceIndex == hullIndex);
        REQUIRE_THAT(hit->t, WithinAbs(9.5, 1e-4));
        REQUIRE(traceAnyHit(renderScene, ray, 0.0F, kInfinity));
        builder.updateTlas();
        REQUIRE(renderScene.tlasCurrent());
        REQUIRE(traceClosestHit(renderScene, ray, 0.0F, kInfinity)->t == hit->t);
    }
    SECTION("materials are content; the lights follow them")
    {
        ball.patch<MaterialComponent>([](auto& m) { m.emissionPower = 0.0F; });
        builder.update(scene);
        REQUIRE(renderScene.contentVersion == renderScene.version);
        REQUIRE(renderScene.lightsVersion == renderScene.version);
        REQUIRE(renderScene.lights.empty());
        REQUIRE(renderScene.lightOfInstance[instanceOf(renderScene, ball)] == -1);
    }
    SECTION("removed entities give their slot to the last instance")
    {
        scene.destroyEntity(ball);
        builder.update(scene);
        REQUIRE(renderScene.instances.size() == 1);
        REQUIRE(renderScene.instances[0].entityId == entt::to_integral(hull.handle()));
        REQUIRE(renderScene.instances[0].materialIndex == 0);
        REQUIRE(renderScene.instanceVersions[0] == renderScene.version);
        REQUIRE(renderScene.lights.empty());
    }

    // Whatever happened, the result is the one of a rebuild.
    builder.update(scene);
    requireSameContent(renderScene, buildRenderScene(scene));
}

TEST_CASE("RenderSceneBuilder follows meshes, layers and new entities", "[render][trace]")
{
    Scene scene;
    const MeshHandle box = primitives::box(Vec3(0.5F)).build();
    const MeshHandle other = primitives::box(Vec3(0.25F)).build();
    std::vector<Entity> boxes;
    for (int i = 0; i < 4; ++i) {
        Entity entity = scene.createEntity("Box");
        entity.get<TransformComponent>().translation = {2.0F * static_cast<float>(i), 0.0F, 0.0F};
        entity.add<MeshComponent>(MeshComponent{.mesh = box});
        boxes.push_back(entity);
    }
    RenderSceneBuilder builder;
    const RenderScene& renderScene = builder.update(scene);
    // The unit sphere's slot and one shared mesh.
    REQUIRE(renderScene.geometries.size() == 2);
    REQUIRE(renderScene.triangleCount() == box->triangleCount());

    boxes[1].patch<MeshComponent>([&](auto& m) { m.mesh = other; });
    scene.setLayer(boxes[2], 3);
    Entity added = scene.createEntity("Ball");
    added.add<SphereComponent>(1.0F);
    builder.update(scene);
    REQUIRE(renderScene.geometries.size() == 3);
    REQUIRE(renderScene.instances[instanceOf(renderScene, boxes[2])].mask == layerBit(3));
    REQUIRE(renderScene.instances.size() == 5);
    requireSameContent(renderScene, buildRenderScene(scene));

    // The last user of a mesh frees its slot, and the next new mesh takes it.
    boxes[1].patch<MeshComponent>([&](auto& m) { m.mesh = box; });
    builder.update(scene);
    REQUIRE(renderScene.triangleCount() == box->triangleCount());
    boxes[3].patch<MeshComponent>([&](auto& m) { m.mesh = primitives::sphere(0.5F, 8, 6).build(); });
    builder.update(scene);
    REQUIRE(renderScene.geometries.size() == 3);
    requireSameContent(renderScene, buildRenderScene(scene));

    // A copy is a lineage of its own.
    const RenderScene copy = renderScene;
    REQUIRE(copy.lineage.value() != renderScene.lineage.value());
    REQUIRE(copy.version == renderScene.version);
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

TEST_CASE("Progressive passes refine from blocks to pixels within a time budget",
          "[render][trace]")
{
    Scene scene;
    makeSphereScene(scene);
    const RenderScene renderScene = buildRenderScene(scene);
    Camera camera;
    camera.setViewport(64, 48);
    CpuPathTracer tracer(4);
    tracer.resize(64, 48);
    PathTracerSettings settings;

    // First pass after a reset: one sample per 4 x 4 block.
    tracer.render(renderScene, camera, settings, {.firstPassStride = 4});
    REQUIRE(tracer.accumulatedFrames() == 1);
    REQUIRE(tracer.passStride() == 2);
    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            REQUIRE(tracer.image().at(x, y) == tracer.image().at(x & ~3U, y & ~3U));
        }
    }
    tracer.render(renderScene, camera, settings, {.firstPassStride = 4});
    REQUIRE(tracer.passStride() == 1);
    tracer.render(renderScene, camera, settings, {.firstPassStride = 4});
    REQUIRE(tracer.accumulatedFrames() == 3);
    REQUIRE(tracer.passStride() == 1);

    // A tiny budget spreads a pass over several calls...
    const std::uint32_t before = tracer.accumulatedFrames();
    int calls = 0;
    while (tracer.accumulatedFrames() == before && calls < 100) {
        tracer.render(renderScene, camera, settings, {.timeBudgetMs = 1e-3F});
        ++calls;
    }
    REQUIRE(tracer.accumulatedFrames() == before + 1);
    REQUIRE(calls > 1);
    // ... a generous one fits several passes in one call...
    tracer.render(renderScene, camera, settings, {.timeBudgetMs = 500.0F});
    REQUIRE(tracer.accumulatedFrames() > before + 2);
    // ... and the first pass after a reset is always complete.
    camera.setPosition({0.0F, 0.0F, 9.0F});
    tracer.render(renderScene, camera, settings, {.timeBudgetMs = 1e-3F, .firstPassStride = 2});
    REQUIRE(tracer.accumulatedFrames() == 1);
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
