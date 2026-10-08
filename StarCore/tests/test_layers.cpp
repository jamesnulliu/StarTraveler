#include <catch2/catch_test_macros.hpp>
#include <entt/entity/registry.hpp>

import std;
import starcore.common;
import starcore.math;
import starcore.render;
import starcore.scene;

using namespace starcore;

TEST_CASE("Layer settings and collision matrix", "[scene][layers]")
{
    SceneLayers layers;
    REQUIRE(layers.layers[0].name == "Default");
    REQUIRE(layers.layers[7].name == "Layer 7");
    REQUIRE(layers.visibleMask() == kAllLayers);
    REQUIRE(layers.collides(3, 5));

    layers.layers[2].visible = false;
    layers.layers[4].pickable = false;
    REQUIRE(layers.visibleMask() == (kAllLayers & ~layerBit(2)));
    REQUIRE(layers.pickableMask() == (kAllLayers & ~layerBit(4)));

    layers.setCollides(3, 5, false);
    REQUIRE_FALSE(layers.collides(3, 5));
    REQUIRE_FALSE(layers.collides(5, 3));  // Symmetric.
    REQUIRE(layers.collides(3, 3));
    REQUIRE(layers.collisionMask(3) == (kAllLayers & ~layerBit(5)));
}

TEST_CASE("Entities move between layers, recursively if asked", "[scene][layers]")
{
    Scene scene;
    Entity ship = scene.createEntity("Ship");
    Entity cockpit = scene.createEntity("Cockpit");
    Entity seat = scene.createEntity("Seat");
    scene.setParent(cockpit, ship);
    scene.setParent(seat, cockpit);

    REQUIRE(scene.layerOf(seat.handle()) == 0);
    const auto version = scene.version();
    scene.setLayer(cockpit, 4, /*recursive=*/true);
    REQUIRE(scene.version() > version);
    REQUIRE(scene.layerOf(ship.handle()) == 0);
    REQUIRE(scene.layerOf(cockpit.handle()) == 4);
    REQUIRE(scene.layerOf(seat.handle()) == 4);
    REQUIRE(scene.layerOf(scene.copyEntity(seat).handle()) == 4);

    scene.setLayer(seat, 0);
    REQUIRE_FALSE(seat.has<LayerComponent>());  // Layer 0 is implicit.
}

TEST_CASE("Rendering and picking filter by layer", "[render][layers]")
{
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity front = scene.createEntity("Front");  // Layer 3, in front.
    front.get<TransformComponent>().translation = {0.0F, 0.0F, 2.0F};
    front.add<SphereComponent>(1.0F);
    front.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = {1, 0, 0}, .emissionPower = 1});
    scene.setLayer(front, 3);
    Entity back = scene.createEntity("Back");  // Layer 0, behind it.
    back.add<SphereComponent>(1.0F);
    back.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = {0, 1, 0}, .emissionPower = 1});
    Entity line = scene.createEntity("Line");
    line.add<LineShapeComponent>(LineShapeComponent{{-3, 2, 0}, {3, 2, 0}});
    line.add<StrokeComponent>(StrokeComponent{.thickness = 3});
    scene.setLayer(line, 5);

    const RenderScene renderScene = buildRenderScene(scene);
    REQUIRE(renderScene.instances.size() == 2);
    const Ray ray{{0, 0, 10}, {0, 0, -1}};
    REQUIRE(traceClosestHit(renderScene, ray, 0, kInfinity)->t < 8.0F);  // Front.
    const auto behind = traceClosestHit(renderScene, ray, 0, kInfinity, ~layerBit(3));
    REQUIRE(behind.has_value());
    REQUIRE(renderScene.instances[behind->instanceIndex].entityId ==
            entt::to_integral(back.handle()));

    Camera camera;
    camera.setViewport(64, 64);
    CpuPathTracer tracer;
    tracer.resize(64, 64);
    PathTracerSettings settings;
    tracer.render(renderScene, camera, settings);
    REQUIRE(tracer.image().at(32, 32) == packRgba8({1, 0, 0, 1}));
    settings.instanceMask = kAllLayers & ~layerBit(3);  // Hide layer 3.
    tracer.render(renderScene, camera, settings);
    REQUIRE(tracer.accumulatedFrames() == 1);  // Settings changed: restarted.
    REQUIRE(tracer.image().at(32, 32) == packRgba8({0, 1, 0, 1}));
    REQUIRE(tracer.entityIds().at(32, 32) == entt::to_integral(back.handle()));
    REQUIRE(pickEntity(renderScene, camera, nullptr, {32, 32}, settings.instanceMask) ==
            back.handle());

    // Overlay: hidden layers are not drawn, locked layers draw without ids.
    const auto countLine = [&](std::uint32_t visible, std::uint32_t pickable) {
        ColorImage color(64, 64, 0U);
        IdImage ids(64, 64, kNoEntityId);
        renderOverlay(scene, camera, color, &ids, visible, pickable);
        return std::pair{std::ranges::count(color.pixels(), packRgba8(Vec4(1.0F))),
                         std::ranges::count(ids.pixels(), entt::to_integral(line.handle()))};
    };
    const auto [drawn, picked] = countLine(kAllLayers, kAllLayers);
    REQUIRE(drawn > 0);
    REQUIRE(picked == drawn);
    REQUIRE(countLine(kAllLayers & ~layerBit(5), kAllLayers).first == 0);
    const auto [lockedDrawn, lockedPicked] = countLine(kAllLayers, kAllLayers & ~layerBit(5));
    REQUIRE(lockedDrawn == drawn);
    REQUIRE(lockedPicked == 0);
}

TEST_CASE("Layers round-trip through scene files", "[scene][layers][serializer]")
{
    Scene scene;
    REQUIRE(serializeScene(scene).find("Layers") == std::string::npos);  // Defaults omitted.
    scene.layers().layers[3] = {"Cockpit", false, false};
    scene.layers().setCollides(3, 0, false);
    Entity seat = scene.createEntity("Seat");
    scene.setLayer(seat, 3);

    const std::string yaml = serializeScene(scene);
    Scene loaded;
    REQUIRE(deserializeScene(yaml, loaded).has_value());
    REQUIRE(loaded.layers() == scene.layers());
    REQUIRE(loaded.layerOf(loaded.findEntityByName("Seat").handle()) == 3);
    REQUIRE(serializeScene(loaded) == yaml);

    loaded.clear();
    REQUIRE(loaded.layers() == SceneLayers{});
}
