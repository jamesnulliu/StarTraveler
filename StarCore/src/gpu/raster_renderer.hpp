#pragma once
// The Raster pipeline of GpuRenderer (private): no rays at all. Per frame:
//   1. shadow maps (ShadowMaps): the scene's depth from the brightest lights;
//   2. visibility (VisibilityPass): the nearest surface and the nearest thin
//      transmissive surface in front of it, per pixel;
//   3. shade (raster.comp): preview lighting, shadows looked up in the shadow
//      maps, the glass blended over, averaged over jittered frames while
//      nothing changes, into GpuRenderer's radiance image; and motion vectors.

#include <array>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "rasterization.hpp"
#include "shadow_maps.hpp"
#include "starcore/gpu/gpu_scene.hpp"
#include "starcore/render/path_tracer.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

/** @brief One frame of the raster pipeline. */
struct RasterFrame
{
    const RenderScene* scene = nullptr;
    const Camera* camera = nullptr;
    const Camera* previousCamera = nullptr;  // For motion vectors.
    PathTracerSettings settings;             // The instance mask.
    ShadowSettings shadows;
    std::uint32_t frameSlot = 0;
    // Frames averaged before this one (0: start over, sampling pixel centers).
    std::uint32_t accumulated = 0;
    bool writeIds = false;
};

class RasterRenderer
{
public:
    explicit RasterRenderer(vulkan::Context& context);

    /** @brief (Re)create the motion image. */
    void resize(std::uint32_t width, std::uint32_t height);

    /**
     * @brief Record a frame: `accumulation` (RGBA32F) sums the frames,
     *        `radiance` (RGBA32F) gets their mean, `entityIds` (R32UI) the
     *        entities if asked, motion() the motion vectors.
     */
    void record(VkCommandBuffer commandBuffer, const RasterFrame& frame, const GpuScene& scene,
                VisibilityPass& visibility, const vulkan::Image& accumulation,
                const vulkan::Image& radiance, const vulkan::Image& entityIds);

    /** @brief RGBA32F, xy: previous minus current pixel position of the surfaces. */
    [[nodiscard]] auto motion() const noexcept -> const vulkan::Image&
    {
        return m_motion;
    }

private:
    vulkan::Context* m_context;
    vulkan::ComputePipeline m_shade;
    ShadowMaps m_shadows;
    std::array<vulkan::Buffer, kFramesInFlight> m_uniforms;
    vulkan::Image m_motion;
};

}  // namespace starcore::gpu
