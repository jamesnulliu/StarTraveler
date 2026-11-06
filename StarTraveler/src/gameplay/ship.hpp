#pragma once
// The player's ship: an entity tree (hull collider, cockpit model, cabin
// light) flown from the pilot's seat with collision and portals.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include <starcore/asset/model.hpp>
#include <starcore/physics/physics_world.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/scene/entity.hpp>

namespace startraveler
{

// Layer conventions of the game (names are set by configureGameLayers()).
inline constexpr std::uint32_t kPortalLayer = 5;
inline constexpr std::uint32_t kShipLayer = 6;     // Exterior hull, collider.
inline constexpr std::uint32_t kCockpitLayer = 7;  // Interior, seen from the seat.

/**
 * @brief Name the game's layers in a scene that does not use them yet, and
 *        lock the ship's layers (clicks go through the cockpit).
 */
void configureGameLayers(starcore::SceneLayers& layers);

/** @brief Flight state of a ship entity (game component, never saved). */
struct ShipComponent
{
    starcore::Vec3 velocity{0.0F};
    float yaw = -90.0F;  // Degrees, same convention as Camera.
    float pitch = 0.0F;
    // The pilot's body, which all come to rest with the ship (a resting image still converges):
    starcore::Vec3 lean{0.0F};   // Head, ship space: thrown against the acceleration.
    float fovKick = 0.0F;        // Degrees the view widens with speed.
    float travel = 0.0F;         // Seconds under way (what shakes the head).
};

struct ShipSettings
{
    float acceleration = 14.0F;      // Units / s^2 of thrust.
    float maxSpeed = 30.0F;          // Legacy camera cap.
    float boostFactor = 3.0F;        // Acceleration and top speed with boost.
    float damping = 1.6F;            // Flight assist: velocity decay per second.
    float lookSensitivity = 0.12F;   // Degrees per pixel.
    float radius = 1.6F;             // Hull collision sphere.
    starcore::Vec3 eye{0.0F, 0.42F, 0.15F};  // Pilot's eyes in ship space.
    float fieldOfView = 70.0F;       // Vertical, degrees: wider than a free camera's.
    // How the pilot feels the flight (0: a camera bolted to the ship, 1 as designed).
    float bodyMotion = 1.0F;
};

/** @brief Per-frame controls (decoupled from the input backend). */
struct ShipInput
{
    // Ship space: x right, y up, z forward; each in [-1, 1].
    starcore::Vec3 thrust{0.0F};
    starcore::Vec2 look{0.0F};  // Mouse delta in pixels.
    bool boost = false;
};

struct ShipEvents
{
    bool collided = false;
    starcore::Vec3 contactPoint{0.0F};   // Where the hull touched (if collided).
    starcore::Vec3 contactNormal{0.0F};  // Away from what it touched.
    bool teleported = false;
};

class PortalSystem;

/** @brief Spawns, flies and follows ship entities (ECS-style system). */
class ShipSystem
{
public:
    ShipSettings settings;

    /**
     * @brief Create the ship (a transient entity tree) at a pose: hull
     *        collider on kShipLayer, the model's "Cockpit" root on
     *        kCockpitLayer and its "Hull" root on kShipLayer, a small
     *        emissive sphere lighting the cabin and particle emitters at the
     *        engines (their exhaust follows the thrust).
     */
    auto spawn(starcore::Scene& scene, starcore::AssetLibrary& assets,
               const std::filesystem::path& model, starcore::Vec3 position, float yaw,
               float pitch = 0.0F) -> std::expected<starcore::Entity, std::string>;

    /**
     * @brief One flight step: thrust and flight assist, mouse look,
     *        collide-and-slide against colliders the ship's layer collides
     *        with, and teleports through portals.
     */
    auto update(starcore::Scene& scene, starcore::Entity ship,
                const starcore::PhysicsWorld& physics, const ShipInput& input,
                float deltaSeconds) -> ShipEvents;

    /** @brief Put the camera at the pilot's eyes, looking about where the ship points. */
    void attachCamera(starcore::Camera& camera, starcore::Entity ship) const;

    /** @brief Ship rotation for a yaw/pitch (no roll); local -Z is forward. */
    [[nodiscard]] static auto orientation(float yawDegrees, float pitchDegrees)
        -> starcore::Quat;
};

}  // namespace startraveler
