#pragma once

#include <optional>

#include <entt/entity/entity.hpp>

#include "starcore/common/image.hpp"
#include "starcore/render/camera.hpp"
#include "starcore/render/render_scene.hpp"

namespace starcore
{

class Scene;

/**
 * @brief Rasterize all 2D overlay shapes (line, ellipse, arc, curve, polygon,
 *        point) of the scene on top of `color`.
 *
 * Shapes live in world space: they are projected with the camera, clipped
 * against the near plane in 3D and against the viewport in 2D, then drawn with
 * the algorithms from starcore/raster. When `ids` is given, every written
 * pixel also stores the entity id (for picking).
 *
 * Draw order: polygon fills, then strokes, then points (markers on top).
 */
void renderOverlay(const Scene& scene, const Camera& camera, ColorImage& color,
                   IdImage* ids = nullptr);

/**
 * @brief Entity under a pixel: overlay shapes (via `overlayIds`) win over
 *        ray-traced geometry (via an exact ray cast among instances whose mask
 *        intersects `mask`; window panes are looked through).
 */
[[nodiscard]] auto pickEntity(const RenderScene& renderScene, const Camera& camera,
                              const IdImage* overlayIds, Vec2 pixelPosition,
                              std::uint32_t mask = kAllInstances)
    -> std::optional<entt::entity>;

}  // namespace starcore
