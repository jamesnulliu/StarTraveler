#pragma once
// Shared by the rasterizing pipelines of GpuRenderer (private), Hybrid and
// Raster: their frame uniforms and the visibility buffer pass.

#include <array>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/render/camera.hpp"
#include "starcore/render/render_scene.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

/** @brief Uniforms of every rasterizing pass (`FrameUniforms` in hybrid.glsl, std140). */
struct FrameUniforms
{
    Vec4 cameraPosition;  // w: near plane of the visibility pass
    Vec4 cameraForward;   // w: firefly clamp (0 = off)
    Vec4 cameraRight;     // right * aspect * tan(fov / 2); w: exposure
    Vec4 cameraUp;        // up * tan(fov / 2)
    Vec4 previousPosition;
    Vec4 previousForward;
    Vec4 previousRight;
    Vec4 previousUp;
    Vec4 skyColor;
    Vec4 viewport;               // xy: size, zw: sample position in the pixel (jitter)
    Vec4 previousJitter;         // xy: the previous frame's
    std::uint32_t frame[4];      // random stream index, seed, samples per pixel, max bounces
    std::uint32_t options[4];    // flags, instance mask, light count, tone mapper
    std::uint32_t history[4];    // cap (0 = none), reset/still bits, view, traced pixels
    Vec4 denoiser;               // color sigma, normal power, depth sigma, -
    // The radiance cache: buffer addresses of its keys and sums; of its values, capacity.
    std::uint32_t cacheBuffers[4];
    std::uint32_t cacheValues[4];
};
static_assert(sizeof(FrameUniforms) == 272, "std140 layout of FrameUniforms");

// hybrid.glsl: options[0] and history[1] bits.
inline constexpr std::uint32_t kFlagPreview = 1U;
inline constexpr std::uint32_t kFlagNextEvent = 2U;
inline constexpr std::uint32_t kFlagSrgb = 4U;
inline constexpr std::uint32_t kFlagWriteIds = 8U;
inline constexpr std::uint32_t kFlagUpscaled = 16U;
inline constexpr std::uint32_t kFlagDirectShadowMaps = 32U;
inline constexpr std::uint32_t kFlagRadianceCache = 64U;
inline constexpr std::uint32_t kHistoryReset = 1U;
inline constexpr std::uint32_t kHistoryStill = 2U;

inline constexpr std::uint32_t kWorkgroupSize = 8;  // local_size of the compute passes.
inline constexpr float kNearPlane = 0.01F;          // Of the visibility pass (reversed depth).

/** @brief Camera rays: forward + right * ndcX + up * ndcY (Camera::generateRay). */
void cameraBasis(const Camera& camera, Vec4& position, Vec4& forward, Vec4& right, Vec4& up);

/** @brief Sample position in the pixel of a frame (Halton 2, 3: low discrepancy over frames). */
[[nodiscard]] auto jitter(std::uint32_t frameIndex) -> Vec2;

/** @brief Write `uniforms` into a frame slot's buffer (created on first use). */
void writeUniforms(vulkan::Context& context, vulkan::Buffer& buffer,
                   const FrameUniforms& uniforms);

/** @brief Which surfaces the second visibility layer holds. */
enum class GlassLayer : std::uint8_t
{
    SmoothPanes,       // Hybrid: smooth see-through panes (isGlassLayer in hybrid.glsl).
    ThinTransmissive,  // Raster: every thin surface letting light through.
};

/**
 * @brief The visibility buffer: per pixel, instance + 1 (0: none) and
 *        triangle of the nearest surface, rasterized with the frame
 *        uniforms' camera and jitter; and the same for a second layer, the
 *        nearest "glass" in front of it.
 *
 * Instances are drawn with vertices pulled from the GpuScene buffers (spheres
 * as cubes intersected exactly in the fragment shader), reversed depth.
 */
class VisibilityPass
{
public:
    explicit VisibilityPass(vulkan::Context& context);

    /** @brief (Re)create the images. */
    void resize(std::uint32_t width, std::uint32_t height);

    /**
     * @brief Rasterize the primary layer, then over a copy of its depth the
     *        glass layer; ends with a barrier for compute shaders reading the
     *        visibility images.
     */
    void record(VkCommandBuffer commandBuffer, VkBuffer uniforms, const GpuScene& scene,
                const RenderScene& renderScene, std::uint32_t instanceMask, GlassLayer glass);

    [[nodiscard]] auto visibility() const noexcept -> const vulkan::Image&
    {
        return m_visibility;
    }
    [[nodiscard]] auto glassVisibility() const noexcept -> const vulkan::Image&
    {
        return m_glassVisibility;
    }
    /** @brief D32, reversed (near / view depth) of the primary layer. */
    [[nodiscard]] auto depth() const noexcept -> const vulkan::Image&
    {
        return m_depth;
    }

private:
    void draw(VkCommandBuffer commandBuffer, VkBuffer uniforms, const GpuScene& scene,
              const RenderScene& renderScene, std::uint32_t instanceMask, GlassLayer glass,
              bool glassLayer);

    vulkan::Context* m_context;
    vulkan::GraphicsPipeline m_triangles;
    vulkan::GraphicsPipeline m_spheres;  // Sphere impostors.
    vulkan::Image m_depth;
    vulkan::Image m_glassDepth;
    vulkan::Image m_visibility;  // RG32UI.
    vulkan::Image m_glassVisibility;
};

}  // namespace starcore::gpu
