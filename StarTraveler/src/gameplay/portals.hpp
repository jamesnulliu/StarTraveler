#pragma once

#include <optional>

#include <starcore/render/camera.hpp>
#include <starcore/scene/entity.hpp>

namespace startraveler
{

/** @brief A path crossing a linked portal. */
struct PortalCrossing
{
    // World -> world: from the entered portal's frame to the linked one's.
    starcore::Mat4 teleport{1.0F};
    entt::entity portal{entt::null};
};

/**
 * @brief Linked portal pairs (legacy Create_Portal / Teleport).
 *
 * Legacy bugs fixed: `std::array::empty()` is always false, so the first
 * portal was never registered and the "teleport" fired whenever the camera
 * came within 0.5 units of the world origin. Here a portal is an entity
 * (emissive quad + PortalComponent) and a traveller teleports when its path
 * crosses a linked portal's rectangle, keeping its relative pose.
 */
class PortalSystem
{
public:
    /**
     * @brief Place a portal 3 units in front of the camera, facing it, on the
     *        given layer. It is linked to the newest unlinked portal, if any.
     */
    auto createPortal(starcore::Scene& scene, const starcore::Camera& camera,
                      std::uint32_t layer = 0) -> starcore::Entity;

    /** @brief The linked portal crossed by the segment `from` -> `to`, if any. */
    [[nodiscard]] static auto findCrossing(starcore::Scene& scene, starcore::Vec3 from,
                                           starcore::Vec3 to) -> std::optional<PortalCrossing>;

    /**
     * @brief Teleport a free camera if it moved through a linked portal since
     *        the previous call.
     * @return true if the camera was teleported.
     */
    auto update(starcore::Scene& scene, starcore::Camera& camera) -> bool;

    /** @brief Forget the previous camera position (after loads/jumps). */
    void reset()
    {
        m_lastCameraPosition.reset();
    }

private:
    std::optional<starcore::Vec3> m_lastCameraPosition;
};

}  // namespace startraveler
