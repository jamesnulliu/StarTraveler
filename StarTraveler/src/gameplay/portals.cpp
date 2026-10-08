module;

#include <entt/entity/registry.hpp>

#include "starcore/common/log.hpp"

module startraveler.gameplay;

import std;
import glm;
import starcore.asset;
import starcore.math;
import starcore.render;
import starcore.scene;

namespace startraveler
{

using namespace starcore;

namespace
{

constexpr float kPlacementDistance = 3.0F;
const Vec3 kFirstColor{0.0F, 1.0F, 0.0F};   // Legacy green portal.
const Vec3 kSecondColor{1.0F, 0.5F, 0.0F};

auto makeQuad(Vec2 size) -> MeshComponent
{
    return MeshComponent{.mesh = primitives::quad(size).build()};
}

}  // namespace

auto PortalSystem::createPortal(Scene& scene, const Camera& camera, std::uint32_t layer)
    -> Entity
{
    // Newest unlinked portal (EnTT iterates newest first).
    Entity partner;
    for (const auto [handle, portal] : scene.registry().view<PortalComponent>().each()) {
        if (portal.linkedPortal.value() == 0) {
            partner = scene.wrap(handle);
            break;
        }
    }

    Entity portal = scene.createEntity(partner ? "Portal B" : "Portal A");
    auto& transform = portal.get<TransformComponent>();
    transform.translation = camera.position() + camera.forward() * kPlacementDistance;
    // Local XY plane faces the camera: local +Z points back at it.
    transform.rotation = glm::quat_cast(
        Mat3(camera.right(), camera.up(), -camera.forward()));

    const PortalComponent component{.size = {1.6F, 3.2F}};
    portal.add<PortalComponent>(component);
    portal.add<MeshComponent>(makeQuad(component.size));
    portal.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F),
        .emissionColor = partner ? kSecondColor : kFirstColor,
        .emissionPower = 4.0F});
    scene.setLayer(portal, layer);

    if (partner) {
        partner.patch<PortalComponent>(
            [&](auto& p) { p.linkedPortal = portal.uuid(); });
        portal.patch<PortalComponent>(
            [&](auto& p) { p.linkedPortal = partner.uuid(); });
        SC_INFO("Linked {} <-> {}", partner.name(), portal.name());
    }
    return portal;
}

auto PortalSystem::findCrossing(Scene& scene, Vec3 from, Vec3 to) -> std::optional<PortalCrossing>
{
    if (from == to) {
        return std::nullopt;
    }
    for (const auto [handle, portal] : scene.registry().view<PortalComponent>().each()) {
        Entity target = scene.findEntity(portal.linkedPortal);
        if (portal.linkedPortal.value() == 0 || !target) {
            continue;
        }
        const Mat4 toLocal = glm::inverse(scene.worldTransform(handle));
        const Vec3 a = Vec3(toLocal * Vec4(from, 1.0F));
        const Vec3 b = Vec3(toLocal * Vec4(to, 1.0F));
        if ((a.z > 0.0F) == (b.z > 0.0F)) {
            continue;  // Did not cross the portal plane.
        }
        const Vec3 crossing = a + (b - a) * (a.z / (a.z - b.z));
        if (std::abs(crossing.x) > portal.size.x * 0.5F ||
            std::abs(crossing.y) > portal.size.y * 0.5F) {
            continue;  // Crossed the plane outside the rectangle.
        }
        // Source local -> (turned around) target local -> world.
        const Mat4 turnAround = glm::mat4_cast(glm::angleAxis(kPi, Vec3{0, 1, 0}));
        return PortalCrossing{target.worldTransform() * turnAround * toLocal, handle};
    }
    return std::nullopt;
}

auto PortalSystem::update(Scene& scene, Camera& camera) -> bool
{
    const Vec3 current = camera.position();
    const auto previous = std::exchange(m_lastCameraPosition, current);
    if (!previous.has_value()) {
        return false;
    }
    const auto crossing = findCrossing(scene, *previous, current);
    if (!crossing) {
        return false;
    }
    const Vec3 newPosition = Vec3(crossing->teleport * Vec4(current, 1.0F));
    const Vec3 newForward = glm::normalize(Mat3(crossing->teleport) * camera.forward());
    camera.setPosition(newPosition);
    camera.lookAt(newPosition + newForward);
    m_lastCameraPosition = newPosition;
    SC_INFO("Teleported through {}", scene.wrap(crossing->portal).name());
    return true;
}

}  // namespace startraveler
