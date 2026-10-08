#include <algorithm>
#include <cmath>
#include <format>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include "gameplay/portals.hpp"
#include "gameplay/ship.hpp"

namespace startraveler
{

using namespace starcore;

namespace
{

constexpr float kMaxPitch = 89.0F;
const Vec3 kCabinLightPosition{0.0F, 0.62F, 0.55F};

}  // namespace

void configureGameLayers(SceneLayers& layers)
{
    auto configure = [&](std::uint32_t index, const char* name, bool locked) {
        LayerSettings& layer = layers.layers[index];
        if (layer.name == std::format("Layer {}", index)) {  // Unused so far.
            layer.name = name;
            layer.pickable = !locked;
        }
    };
    configure(kPortalLayer, "Portals", false);
    configure(kShipLayer, "Ship", true);
    configure(kCockpitLayer, "Cockpit", true);
}

auto ShipSystem::orientation(float yawDegrees, float pitchDegrees) -> Quat
{
    // Yaw -90 (the camera default) looks down -Z: no rotation.
    return glm::angleAxis(glm::radians(-(yawDegrees + 90.0F)), Vec3{0.0F, 1.0F, 0.0F}) *
           glm::angleAxis(glm::radians(pitchDegrees), Vec3{1.0F, 0.0F, 0.0F});
}

auto ShipSystem::spawn(Scene& scene, AssetLibrary& assets, const std::filesystem::path& model,
                       Vec3 position, float yaw, float pitch)
    -> std::expected<Entity, std::string>
{
    const auto loaded = assets.loadModel(model);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    pitch = std::clamp(pitch, -kMaxPitch, kMaxPitch);

    Entity ship = scene.createEntity("Ship");
    ship.add<TransientComponent>();
    ship.add<ShipComponent>(ShipComponent{.yaw = yaw, .pitch = pitch});
    ship.add<ColliderComponent>(ColliderComponent{.radius = settings.radius});
    ship.patch<TransformComponent>([&](TransformComponent& t) {
        t.translation = position;
        t.rotation = orientation(yaw, pitch);
    });
    scene.setLayer(ship, kShipLayer);

    const auto roots = instantiateModel(scene, **loaded, model.string(), ship);
    if (!roots) {
        scene.destroyEntity(ship);
        return std::unexpected(roots.error());
    }
    for (const Entity root : *roots) {
        scene.setLayer(root, root.name() == "Hull" ? kShipLayer : kCockpitLayer, true);
    }

    // A small sphere light (sampled directly by the path tracer) for the cabin.
    Entity light = scene.createEntity("Cabin light");
    scene.setParent(light, ship);
    light.patch<TransformComponent>([](TransformComponent& t) {
        t = TransformComponent{.translation = kCabinLightPosition};
    });
    light.add<SphereComponent>(0.04F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = {1.0F, 0.9F, 0.8F}, .emissionPower = 40.0F});
    scene.setLayer(light, kCockpitLayer);
    return ship;
}

auto ShipSystem::update(Scene& scene, Entity ship, const PhysicsWorld& physics,
                        const ShipInput& input, float deltaSeconds) -> ShipEvents
{
    ShipEvents events;
    ShipComponent state = ship.get<ShipComponent>();
    const Vec3 start = ship.get<TransformComponent>().translation;

    // Steering.
    state.yaw = std::remainder(state.yaw + input.look.x * settings.lookSensitivity, 360.0F);
    state.pitch = std::clamp(state.pitch - input.look.y * settings.lookSensitivity, -kMaxPitch,
                             kMaxPitch);
    Quat rotation = orientation(state.yaw, state.pitch);

    // Thrust (ship space: +z input = forward = local -Z), flight assist, speed cap.
    Vec3 thrust = input.thrust;
    if (const float length = glm::length(thrust); length > 1.0F) {
        thrust /= length;
    }
    const float boost = input.boost ? settings.boostFactor : 1.0F;
    state.velocity += rotation * Vec3(thrust.x, thrust.y, -thrust.z) *
                      (settings.acceleration * boost * deltaSeconds);
    if (glm::dot(thrust, thrust) < 1e-6F) {
        state.velocity *= std::exp(-settings.damping * deltaSeconds);
        if (glm::dot(state.velocity, state.velocity) < 1e-4F) {
            state.velocity = Vec3(0.0F);  // Come to rest (and let the image converge).
        }
    }
    const float maxSpeed = settings.maxSpeed * boost;
    if (const float speed = glm::length(state.velocity); speed > maxSpeed) {
        state.velocity *= maxSpeed / speed;
    }

    // Collide and slide; lose the velocity going into what we hit.
    const QueryFilter filter{.layerMask = scene.layers().collisionMask(kShipLayer),
                             .ignore = ship.handle()};
    const MoveResult moved =
        physics.moveSphere(start, settings.radius, state.velocity * deltaSeconds, filter);
    for (const Vec3& normal : moved.normals) {
        if (const float into = glm::dot(state.velocity, normal); into < 0.0F) {
            state.velocity -= normal * into;
        }
    }
    events.collided = moved.collided;
    Vec3 end = moved.position;

    // Portals keep the pose relative to the portal pair.
    if (const auto crossing = PortalSystem::findCrossing(scene, start, end)) {
        const Mat3 turn(crossing->teleport);
        end = Vec3(crossing->teleport * Vec4(end, 1.0F));
        const Vec3 forward = glm::normalize(turn * (rotation * Vec3{0.0F, 0.0F, -1.0F}));
        state.yaw = glm::degrees(std::atan2(forward.z, forward.x));
        state.pitch = std::clamp(glm::degrees(std::asin(std::clamp(forward.y, -1.0F, 1.0F))),
                                 -kMaxPitch, kMaxPitch);
        state.velocity = turn * state.velocity;
        rotation = orientation(state.yaw, state.pitch);
        events.teleported = true;
    }

    // Only touch the components when something changed: a resting ship keeps
    // the scene version (and so the path tracer's accumulation) unchanged.
    const ShipComponent& previous = ship.get<ShipComponent>();
    if (state.velocity != previous.velocity || state.yaw != previous.yaw ||
        state.pitch != previous.pitch) {
        ship.patch<ShipComponent>([&](ShipComponent& s) { s = state; });
    }
    const TransformComponent& transform = ship.get<TransformComponent>();
    if (end != transform.translation || rotation != transform.rotation) {
        ship.patch<TransformComponent>([&](TransformComponent& t) {
            t.translation = end;
            t.rotation = rotation;
        });
    }
    return events;
}

void ShipSystem::attachCamera(Camera& camera, Entity ship) const
{
    const ShipComponent& state = ship.get<ShipComponent>();
    camera.setPosition(Vec3(ship.worldTransform() * Vec4(settings.eye, 1.0F)));
    camera.setRotation(state.yaw, state.pitch);
}

}  // namespace startraveler
