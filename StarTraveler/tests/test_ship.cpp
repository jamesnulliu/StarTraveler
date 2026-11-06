// The player's ship: spawning the cockpit model, flight, collisions, portals.

#include <algorithm>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/geometric.hpp>

#include <starcore/render/path_tracer.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/scene/serializer.hpp>

#include "gameplay/portals.hpp"
#include "gameplay/ship.hpp"

using namespace starcore;
using namespace startraveler;
using Catch::Matchers::WithinAbs;

namespace
{

const std::filesystem::path kCockpit =
    std::filesystem::path(STARTRAVELER_SOURCE_ASSET_DIR) / "models" / "cockpit.glb";
constexpr float kStep = 1.0F / 60.0F;

/** @brief Fly for `seconds` with fixed input, rebuilding physics every step like the game. */
auto fly(Scene& scene, ShipSystem& ships, Entity ship, const ShipInput& input, float seconds)
    -> ShipEvents
{
    ShipEvents any;
    for (float t = 0.0F; t < seconds; t += kStep) {
        const ShipEvents events =
            ships.update(scene, ship, PhysicsWorld::build(scene), input, kStep);
        any.collided |= events.collided;
        any.teleported |= events.teleported;
    }
    return any;
}

auto positionOf(Entity ship) -> Vec3
{
    return ship.get<TransformComponent>().translation;
}

}  // namespace

TEST_CASE("Game layers are named and locked once", "[gameplay][ship]")
{
    SceneLayers layers;
    configureGameLayers(layers);
    REQUIRE(layers.layers[kShipLayer].name == "Ship");
    REQUIRE(layers.layers[kCockpitLayer].name == "Cockpit");
    REQUIRE_FALSE(layers.layers[kCockpitLayer].pickable);
    REQUIRE(layers.layers[kPortalLayer].pickable);

    // A scene that already uses the layer for something else keeps it.
    SceneLayers custom;
    custom.layers[kCockpitLayer].name = "Asteroids";
    configureGameLayers(custom);
    REQUIRE(custom.layers[kCockpitLayer].name == "Asteroids");
    REQUIRE(custom.layers[kCockpitLayer].pickable);
}

TEST_CASE("The ship spawns as a transient entity tree with its cockpit", "[gameplay][ship]")
{
    Scene scene;
    scene.createEntity("World");
    AssetLibrary assets;
    ShipSystem ships;
    const auto spawned = ships.spawn(scene, assets, kCockpit, {0, 0, 13}, -90.0F);
    REQUIRE(spawned.has_value());
    Entity ship = *spawned;

    REQUIRE(ship.has<TransientComponent>());
    REQUIRE(ship.get<ColliderComponent>().radius == ships.settings.radius);
    REQUIRE(scene.layerOf(ship.handle()) == kShipLayer);
    Entity canopy = scene.findEntityByName("Canopy");
    REQUIRE(canopy);
    REQUIRE(scene.layerOf(canopy.handle()) == kCockpitLayer);
    REQUIRE(canopy.get<MaterialComponent>().thinWalled);
    REQUIRE(scene.layerOf(scene.findEntityByName("Fuselage").handle()) == kShipLayer);
    REQUIRE(scene.findEntityByName("Cabin light").has<SphereComponent>());

    // The ship and everything in it stay out of scene files.
    Scene saved;
    REQUIRE(deserializeScene(serializeScene(scene), saved).has_value());
    REQUIRE(saved.entityCount() == 1);

    // The cabin light and the glowing meshes (screens, buttons, light strips, engine
    // glow) are directly sampled lights; the cockpit is ray traced.
    const RenderScene renderScene = buildRenderScene(scene);
    REQUIRE(std::ranges::count(renderScene.lights, 0U, &RenderLight::triangleCount) == 1);
    REQUIRE(renderScene.lights.size() == 9);
    REQUIRE(renderScene.triangleCount() > 1000);

    REQUIRE_FALSE(ships.spawn(scene, assets, "missing.glb", Vec3(0.0F), -90.0F).has_value());
}

TEST_CASE("Ships fly where they point, coast to a stop and steer with the mouse",
          "[gameplay][ship]")
{
    Scene scene;
    AssetLibrary assets;
    ShipSystem ships;
    Entity ship = *ships.spawn(scene, assets, kCockpit, {0, 0, 0}, -90.0F);

    fly(scene, ships, ship, ShipInput{.thrust = {0, 0, 1}}, 1.0F);
    const Vec3 moved = positionOf(ship);
    REQUIRE(moved.z < -5.0F);  // Forward = -Z at yaw -90.
    REQUIRE_THAT(moved.x, WithinAbs(0.0, 1e-4));
    REQUIRE(glm::length(ship.get<ShipComponent>().velocity) <= ships.settings.maxSpeed);

    // Flight assist brings the ship to rest; then the scene stops changing.
    fly(scene, ships, ship, ShipInput{}, 6.0F);
    REQUIRE(ship.get<ShipComponent>().velocity == Vec3(0.0F));
    const auto version = scene.version();
    fly(scene, ships, ship, ShipInput{}, 0.5F);
    REQUIRE(scene.version() == version);

    // 750 px to the right at 0.12 degrees per pixel: a quarter turn to +X.
    fly(scene, ships, ship, ShipInput{.look = {750.0F, 0.0F}}, kStep);
    REQUIRE_THAT(ship.get<ShipComponent>().yaw, WithinAbs(0.0, 1e-3));
    Camera camera;
    ships.attachCamera(camera, ship);
    // The pilot's eyes are behind the turn at first, and soon catch up.
    REQUIRE(glm::distance(camera.forward(), Vec3{1, 0, 0}) > 0.01F);
    fly(scene, ships, ship, ShipInput{}, 2.0F);
    ships.attachCamera(camera, ship);
    REQUIRE(glm::distance(camera.forward(), Vec3{1, 0, 0}) < 1e-4F);
    // The eyes are inside the cockpit, rotated with the ship.
    const Vec3 eye = positionOf(ship) + Vec3(-ships.settings.eye.z, ships.settings.eye.y, 0.0F);
    REQUIRE(glm::distance(camera.position(), eye) < 1e-4F);
}

TEST_CASE("The pilot feels the flight, and nothing of it at rest", "[gameplay][ship]")
{
    Scene scene;
    AssetLibrary assets;
    ShipSystem ships;
    Entity ship = *ships.spawn(scene, assets, kCockpit, {0, 0, 0}, -90.0F);
    Camera camera;
    ships.attachCamera(camera, ship);
    REQUIRE(camera.verticalFov() == ships.settings.fieldOfView);

    // Flat out with boost: thrown back while speeding up, the view wider, the head shaking.
    fly(scene, ships, ship, ShipInput{.thrust = {0, 0, 1}, .boost = true}, 0.5F);
    REQUIRE(ship.get<ShipComponent>().lean.z > 0.01F);  // Ship space: +Z is back.
    fly(scene, ships, ship, ShipInput{.thrust = {0, 0, 1}, .boost = true}, 4.0F);
    ships.attachCamera(camera, ship);
    const float wide = camera.verticalFov();
    REQUIRE(wide > ships.settings.fieldOfView + 5.0F);
    const Vec3 shaken = camera.position() - positionOf(ship);
    fly(scene, ships, ship, ShipInput{.thrust = {0, 0, 1}, .boost = true}, kStep);
    ships.attachCamera(camera, ship);
    REQUIRE(glm::distance(camera.position() - positionOf(ship), shaken) > 1e-4F);

    // Bolted to the ship, none of that.
    ShipSystem bolted;
    bolted.settings.bodyMotion = 0.0F;
    bolted.attachCamera(camera, ship);
    REQUIRE(camera.verticalFov() == bolted.settings.fieldOfView);

    // At rest, all of it gone exactly, and the scene (the image) stops changing.
    fly(scene, ships, ship, ShipInput{}, 10.0F);
    const ShipComponent& state = ship.get<ShipComponent>();
    REQUIRE(state.velocity == Vec3(0.0F));
    REQUIRE(state.lean == Vec3(0.0F));
    REQUIRE(state.lookLag == Vec2(0.0F));
    REQUIRE(state.fovKick == 0.0F);
    ships.attachCamera(camera, ship);
    REQUIRE(camera.verticalFov() == ships.settings.fieldOfView);
    REQUIRE(glm::distance(camera.position(), positionOf(ship) + Vec3(0.0F, ships.settings.eye.y,
                                                                     ships.settings.eye.z)) <
            1e-4F);
    const auto version = scene.version();
    fly(scene, ships, ship, ShipInput{}, 0.5F);
    REQUIRE(scene.version() == version);
}

TEST_CASE("Ships collide with planets but not with their own cockpit", "[gameplay][ship]")
{
    Scene scene;
    Entity planet = scene.createEntity("Planet");
    planet.get<TransformComponent>().translation = {0, 0, -30};
    planet.add<SphereComponent>(10.0F);
    planet.add<ColliderComponent>(ColliderComponent{.radius = 10.0F});
    AssetLibrary assets;
    ShipSystem ships;
    Entity ship = *ships.spawn(scene, assets, kCockpit, {0, 0, 0}, -90.0F);

    const ShipEvents events = fly(scene, ships, ship, ShipInput{.thrust = {0, 0, 1}, .boost = true},
                                  4.0F);
    REQUIRE(events.collided);
    const float distance = glm::distance(positionOf(ship), Vec3{0, 0, -30});
    REQUIRE(distance >= 10.0F + ships.settings.radius - 1e-3F);
    REQUIRE(distance < 10.0F + ships.settings.radius + 0.1F);

    // Layers that do not collide with the ship let it pass.
    scene.layers().setCollides(kShipLayer, 0, false);
    fly(scene, ships, ship, ShipInput{.thrust = {0, 0, 1}, .boost = true}, 2.0F);
    REQUIRE(positionOf(ship).z < -30.0F);
}

TEST_CASE("Ships fly through portals", "[gameplay][ship][portals]")
{
    Scene scene;
    AssetLibrary assets;
    ShipSystem ships;
    PortalSystem portals;
    Camera camera;  // At (0, 0, 10) facing -Z: a portal at z = 7.
    portals.createPortal(scene, camera, kPortalLayer);
    camera.setPosition({40, 0, 10});
    camera.setRotation(0.0F, 0.0F);  // Facing +X: a portal at x = 43.
    portals.createPortal(scene, camera, kPortalLayer);

    Entity ship = *ships.spawn(scene, assets, kCockpit, {0, 0, 12}, -90.0F);
    const ShipEvents events = fly(scene, ships, ship, ShipInput{.thrust = {0, 0, 1}}, 2.5F);
    REQUIRE(events.teleported);
    // Out of the second portal, heading back where its creator stood (-X).
    REQUIRE(positionOf(ship).x < 43.0F);
    Camera view;
    ships.attachCamera(view, ship);
    REQUIRE(glm::distance(view.forward(), Vec3{-1, 0, 0}) < 1e-3F);
    REQUIRE(ship.get<ShipComponent>().velocity.x < 0.0F);
}
