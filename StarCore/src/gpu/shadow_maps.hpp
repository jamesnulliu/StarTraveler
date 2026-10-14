#pragma once
// Shadow maps of the Raster pipeline (private): every frame the brightest
// sphere lights render the scene's depth, as seen from their center, into the
// tiles of one depth atlas; raster.comp looks shadows up in it (shaders/
// shadows.glsl). A light near the camera gets a cube of six views (shadows all
// around it); a far one three nested views around the camera (cascades),
// fixed to a reference direction and shifted by whole texels, so that the
// camera moving does not make shadow edges crawl.

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "starcore/gpu/gpu_renderer.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/render/camera.hpp"
#include "starcore/render/render_scene.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

// shaders/shadows.glsl
inline constexpr std::uint32_t kMaxShadowLights = 4;
inline constexpr std::uint32_t kMaxShadowViews = 16;

/** @brief `ShadowView` in shadows.glsl (std430). */
struct ShadowViewRecord
{
    Vec4 position;  // xyz: the light's center, w: near plane
    Vec4 forward;   // xyz: unit, w: scale (1 / tangent of half the field of view)
    Vec4 right;     // xyz: unit, w: tangent of the view's center
    Vec4 up;        // xyz: unit, w: tangent of the view's center
    Vec4 tile;      // xy: atlas texel of the tile's corner, z: texels per side, w: 1 / atlas size
};

/** @brief `ShadowLight` in shadows.glsl (std430). */
struct ShadowLightRecord
{
    std::uint32_t lightIndex = 0;
    std::uint32_t firstView = 0;
    std::uint32_t viewCount = 0;
    std::uint32_t cube = 0;
    Vec4 axisX{0.0F};
    Vec4 axisY{0.0F};
    Vec4 axisZ{0.0F};
};

/** @brief The `Shadows` buffer of shadows.glsl. */
struct ShadowRecords
{
    std::uint32_t lightCount = 0;
    std::uint32_t padding[3] = {};
    std::array<ShadowLightRecord, kMaxShadowLights> lights{};
    std::array<ShadowViewRecord, kMaxShadowViews> views{};
};

class ShadowMaps
{
public:
    explicit ShadowMaps(vulkan::Context& context);

    /**
     * @brief Pick the lights, fit their views and record rendering them (none
     *        if disabled) for a frame slot; ends with a barrier for compute
     *        shaders sampling the atlas.
     */
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, const RenderScene& scene,
                const GpuScene& gpuScene, const Camera& camera, std::uint32_t instanceMask,
                const ShadowSettings& settings);

    /** @brief The slot's `Shadows` buffer (binding 13 of shadows.glsl). */
    [[nodiscard]] auto records(std::uint32_t frameSlot) const noexcept -> VkBuffer
    {
        return m_records[frameSlot].handle();
    }
    /** @brief D32, reversed depth (binding 14 of shadows.glsl, with sampler()). */
    [[nodiscard]] auto atlas() const noexcept -> VkImageView
    {
        return m_atlas.view();
    }
    [[nodiscard]] auto sampler() const noexcept -> VkSampler
    {
        return m_sampler.handle();
    }

private:
    /** @brief A far light's reference direction (its cascades look along it). */
    struct Reference
    {
        Vec3 center{0.0F};
        Vec3 forward{0.0F};
        Vec3 right{0.0F};
        Vec3 up{0.0F};
    };

    /** @brief An atlas of views of `resolution` texels (0: a placeholder). */
    void resizeAtlas(std::uint32_t resolution);
    /** @brief Pick the lights and fit their views (sets m_viewLights). */
    void fitViews(const RenderScene& scene, const Camera& camera, float distance,
                  std::uint32_t resolution, ShadowRecords& records);
    void draw(VkCommandBuffer commandBuffer, VkBuffer records, const RenderScene& scene,
              const GpuScene& gpuScene, std::uint32_t instanceMask,
              const ShadowRecords& shadowRecords);

    vulkan::Context* m_context;
    vulkan::GraphicsPipeline m_triangles;
    vulkan::GraphicsPipeline m_spheres;
    vulkan::Image m_atlas;
    vulkan::Sampler m_sampler;
    std::array<vulkan::Buffer, kFramesInFlight> m_records;
    std::uint32_t m_resolution = 0;  // Of a view (0: the atlas is a placeholder).
    // Per light instance.
    std::unordered_map<std::uint32_t, Reference> m_references;
    std::vector<std::uint32_t> m_viewLights;  // Per view: its light's instance.
};

}  // namespace starcore::gpu
