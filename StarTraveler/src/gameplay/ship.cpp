#include <algorithm>
#include <cmath>
#include <format>

#include <glm/common.hpp>
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
// The nozzles of cockpit.glb's engines (tools/build_cockpit.py), exhaust along +Z.
const Vec3 kNozzles[] = {{-0.62F, -0.8F, 3.75F}, {0.62F, -0.8F, 3.75F}};
constexpr float kIdleExhaust = 30.0F;  // Particles per second and engine.
constexpr float kFullExhaust = 400.0F;
// The pilot's body (ShipSettings::bodyMotion). Units of ship space, degrees, per second. Where
// the view points is not part of it: that is the mouse's, exactly, or turning stops feeling
// connected to the hand.
constexpr float kLeanPerAcceleration = 0.0035F;  // Of the head, per unit / s^2.
constexpr float kMaxLean = 0.07F;
constexpr float kLeanRate = 7.0F;
constexpr float kMaxFovKick = 9.0F;  // At top speed with boost.
constexpr float kFovKickRate = 3.0F;
constexpr float kMaxShake = 0.006F;  // Likewise.

/** @brief Hot, fast and short-lived blue exhaust. */
auto exhaust() -> ParticleEmitterComponent
{
    return ParticleEmitterComponent{
        .particles = {.lifetime = {0.25F, 0.45F},
                      .speed = {7.0F, 10.0F},
                      .spread = 0.08F,
                      .size = {0.16F, 0.02F},
                      .startColor = {1.2F, 2.4F, 6.0F},
                      .endColor = {0.05F, 0.1F, 0.4F},
                      .drag = 0.5F},
        .rate = kIdleExhaust,
    };
}

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
    // Irradiance probes moving with the ship, for the cockpit's (cockpit.glb's bounds, some
    // 0.3 apart): used only with the renderer's experimental probes on (ProbeSettings).
    ship.add<ProbeVolumeComponent>(ProbeVolumeComponent{
        .center = {0.0F, 0.155F, -0.15F}, .size = {1.65F, 1.09F, 3.05F}, .probes = {6, 4, 10}});

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

    // Exhaust: emitters look down their -Z, the engines blow along the ship's +Z.
    for (const Vec3& nozzle : kNozzles) {
        Entity engine = scene.createEntity("Engine exhaust");
        scene.setParent(engine, ship);
        engine.patch<TransformComponent>([&](TransformComponent& t) {
            t = TransformComponent{.translation = nozzle,
                                   .rotation = glm::angleAxis(kPi, Vec3{0.0F, 1.0F, 0.0F})};
        });
        engine.add<ParticleEmitterComponent>(exhaust());
        scene.setLayer(engine, kShipLayer);
    }
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
    if (moved.collided && !moved.normals.empty()) {
        events.contactNormal = moved.normals.front();
        events.contactPoint = moved.position - moved.normals.front() * settings.radius;
    }
    Vec3 end = moved.position;

    // Exhaust follows the forward thrust (the emitters are the ship's children).
    const float exhaust =
        kIdleExhaust + (kFullExhaust - kIdleExhaust) * std::max(0.0F, thrust.z) * boost;
    for (const entt::entity child : scene.childrenOf(ship)) {
        if (auto* emitter = scene.registry().try_get<ParticleEmitterComponent>(child)) {
            emitter->rate = exhaust;  // Not traced: no need to bump the scene version.
        }
    }

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

    // The pilot's body. Each of these settles to exactly nothing with the ship.
    const ShipComponent& previous = ship.get<ShipComponent>();
    {
        const float dt = std::max(deltaSeconds, 1e-4F);
        const Vec3 acceleration = (state.velocity - previous.velocity) / dt;
        const Vec3 lean = glm::clamp(glm::inverse(rotation) * acceleration * -kLeanPerAcceleration,
                                     Vec3(-kMaxLean), Vec3(kMaxLean));
        state.lean += (lean - state.lean) * (1.0F - std::exp(-kLeanRate * dt));
        const float speed = glm::length(state.velocity);
        const float fraction = speed / (settings.maxSpeed * settings.boostFactor);
        const float kick = kMaxFovKick * std::pow(fraction, 1.5F);
        state.fovKick += (kick - state.fovKick) * (1.0F - std::exp(-kFovKickRate * dt));
        if (speed > 0.0F) {
            state.travel += dt;
        } else {
            if (glm::dot(state.lean, state.lean) < 1e-8F) {
                state.lean = Vec3(0.0F);
            }

            if (state.fovKick < 1e-3F) {
                state.fovKick = 0.0F;
            }
        }
    }

    // Only touch the components when something changed: a resting ship keeps
    // the scene version (and so the path tracer's accumulation) unchanged.
    if (state.velocity != previous.velocity || state.yaw != previous.yaw ||
        state.pitch != previous.pitch || state.lean != previous.lean ||
        state.fovKick != previous.fovKick || state.travel != previous.travel) {
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
    // Engines and speed through the hull: faster, and it shakes more; nothing at rest.
    const float fraction =
        glm::length(state.velocity) / (settings.maxSpeed * settings.boostFactor);
    const float t = state.travel;
    const Vec3 shake = Vec3(std::sin(t * 37.1F) + 0.5F * std::sin(t * 53.3F),
                            std::sin(t * 41.7F + 1.3F) + 0.5F * std::sin(t * 61.9F), 0.0F) *
                       (kMaxShake * fraction * fraction);
    const Vec3 eye = settings.eye + (state.lean + shake) * settings.bodyMotion;
    camera.setPosition(Vec3(ship.worldTransform() * Vec4(eye, 1.0F)));
    camera.setRotation(state.yaw, state.pitch);
    camera.setVerticalFov(settings.fieldOfView + state.fovKick * settings.bodyMotion);
}

}  // namespace startraveler
