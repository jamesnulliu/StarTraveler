#pragma once

#include <starcore/math/types.hpp>
#include <starcore/raster/types.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/scene/scene.hpp>

namespace startraveler
{

/** @brief Mouse/viewport state for the current frame. */
struct ViewportState
{
    starcore::UVec2 size{0};
    // Mouse in viewport pixels (top-left origin, +y down).
    starcore::Vec2 mouse{0.0F};
    // Mouse is over the viewport image (and not over another window).
    bool hovered = false;
    // The camera is being controlled (RMB held): tools ignore input.
    bool cameraActive = false;

    [[nodiscard]] auto acceptsInput() const -> bool
    {
        return hovered && !cameraActive;
    }
};

/** @brief Defaults for newly drawn shapes (legacy "Color 1" / "Color 2"). */
struct StrokeDefaults
{
    starcore::Vec4 primary{1.0F};
    starcore::Vec4 secondary{0.6F, 0.8F, 1.0F, 1.0F};
    std::uint32_t thickness = 3;
    starcore::LinePattern pattern = starcore::LinePattern::Solid;
};

/** @brief Everything editor tools need, rebuilt every frame. */
struct EditorContext
{
    starcore::Scene& scene;
    const starcore::Camera& camera;
    ViewportState viewport;
    StrokeDefaults stroke;
    // New points are placed on the plane at this depth in front of the camera.
    float drawDepth = 5.0F;

    [[nodiscard]] auto mouseWorld() const -> starcore::Vec3
    {
        return camera.screenToWorld(viewport.mouse, drawDepth);
    }

    /**
     * @brief Rotation whose local XY plane faces the camera (local +X =
     *        camera right, +Y = camera up), used for ellipses and arcs.
     */
    [[nodiscard]] auto cameraFacingRotation() const -> starcore::Quat;
};

}  // namespace startraveler
