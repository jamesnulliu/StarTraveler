#pragma once
// The Raster pipeline of GpuRenderer (private): no rays at all. Per frame:
//   1. visibility (VisibilityPass): the nearest surface and the nearest thin
//      transmissive surface in front of it, per pixel;
//   2. shade (raster.comp): preview lighting without shadows, the glass
//      blended over, averaged over jittered frames while nothing changes,
//      tone mapped into GpuRenderer's output image.

#include <array>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "rasterization.hpp"
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
    PathTracerSettings settings;  // Instance mask, exposure, tone mapper, sRGB.
    std::uint32_t frameSlot = 0;
    // Frames averaged before this one (0: start over, sampling pixel centers).
    std::uint32_t accumulated = 0;
    bool writeIds = false;
};

class RasterRenderer
{
public:
    explicit RasterRenderer(vulkan::Context& context);

    /**
     * @brief Record a frame: `accumulation` (RGBA32F) sums the frames,
     *        `output` (RGBA8) gets their mean, `entityIds` (R32UI) the
     *        entities if asked.
     */
    void record(VkCommandBuffer commandBuffer, const RasterFrame& frame, const GpuScene& scene,
                VisibilityPass& visibility, const vulkan::Image& accumulation,
                const vulkan::Image& output, const vulkan::Image& entityIds);

private:
    vulkan::Context* m_context;
    vulkan::ComputePipeline m_shade;
    std::array<vulkan::Buffer, kFramesInFlight> m_uniforms;
};

}  // namespace starcore::gpu
